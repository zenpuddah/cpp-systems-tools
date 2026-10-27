#include "memory_lab/experiments.hpp"
#include "memory_lab/arena.hpp"
#include "memory_lab/pool.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace memory_lab {

void validate_config(const ExperimentConfig& c) {
    if (c.size < 1 || c.size > 2048 || c.repetitions < 1 || c.repetitions > 20)
        throw std::invalid_argument("size must be 1..2048; repetitions must be 1..20");
}

std::optional<double> operations_per_second(std::size_t n, double ns) noexcept {
    if (!(ns > 0) || !std::isfinite(ns)) return std::nullopt;
    const double rate = static_cast<double>(n) * 1.0e9 / ns;
    if (!std::isfinite(rate)) return std::nullopt;
    return rate;
}

FreeIntervalModel::FreeIntervalModel(std::size_t capacity) {
    if (capacity == 0 || capacity > 4096) throw std::invalid_argument("model capacity must be 1..4096");
    holes_.reserve(65);
    live_.reserve(64);
    holes_.push_back({0, capacity});
}

std::optional<std::size_t> FreeIntervalModel::allocate(std::size_t bytes) {
    if (bytes == 0 || live_.size() == 64) return std::nullopt;
    for (auto it = holes_.begin(); it != holes_.end(); ++it) {
        if (it->size < bytes) continue;
        const auto offset = it->offset;
        live_.push_back({offset, bytes});
        it->offset += bytes;
        it->size -= bytes;
        if (it->size == 0) holes_.erase(it);
        return offset;
    }
    return std::nullopt;
}

bool FreeIntervalModel::release(std::size_t offset) {
    const auto it = std::find_if(live_.begin(), live_.end(), [=](Interval i) { return i.offset == offset; });
    if (it == live_.end()) return false;
    holes_.push_back(*it);
    live_.erase(it);
    std::sort(holes_.begin(), holes_.end(), [](Interval a, Interval b) { return a.offset < b.offset; });
    return true;
}

void FreeIntervalModel::coalesce() {
    std::vector<Interval> merged;
    merged.reserve(holes_.size());
    for (const auto hole : holes_) {
        if (!merged.empty() && merged.back().offset + merged.back().size == hole.offset)
            merged.back().size += hole.size;
        else merged.push_back(hole);
    }
    holes_ = std::move(merged);
}

std::size_t FreeIntervalModel::total_free() const noexcept {
    return std::accumulate(holes_.begin(), holes_.end(), std::size_t{},
                           [](std::size_t sum, Interval i) { return sum + i.size; });
}

std::size_t FreeIntervalModel::largest_free() const noexcept {
    std::size_t largest = 0;
    for (const auto hole : holes_) largest = std::max(largest, hole.size);
    return largest;
}

FragmentationReport fragmentation_fixture() {
    FreeIntervalModel fragmented(64), contiguous(64);
    FragmentationReport result;
    result.capacity = 64;
    result.failed_request = 24;
    for (int i = 0; i < 4; ++i) {
        if (fragmented.allocate(16) != std::optional<std::size_t>(static_cast<std::size_t>(i) * 16))
            throw std::logic_error("fragmentation fixture allocation");
    }
    if (!fragmented.release(0) || !fragmented.release(32)) throw std::logic_error("model release");
    result.fragmented_holes = fragmented.holes();
    result.fragmented_total = fragmented.total_free();
    result.fragmented_largest = fragmented.largest_free();
    result.fragmented_failed = !fragmented.allocate(result.failed_request).has_value();
    if (!contiguous.allocate(40)) throw std::logic_error("contiguous fixture");
    result.contiguous_succeeded = contiguous.allocate(24).has_value();
    if (!fragmented.release(16)) throw std::logic_error("adjacent release");
    result.before_merge_total = fragmented.total_free();
    result.before_merge_largest = fragmented.largest_free();
    fragmented.coalesce();
    result.coalesced_holes = fragmented.holes();
    result.after_merge_total = fragmented.total_free();
    result.after_merge_largest = fragmented.largest_free();
    result.after_merge_succeeded = fragmented.allocate(24).has_value();
    return result;
}

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::nano>(Clock::now() - start).count(); }

enum class Kind { host, arena, fixed, monotonic, pool };
constexpr Kind kinds[]{Kind::host, Kind::arena, Kind::fixed, Kind::monotonic, Kind::pool};
const char* name(Kind k) {
    switch (k) {
    case Kind::host: return "new_delete_resource";
    case Kind::arena: return "bump_arena";
    case Kind::fixed: return "fixed_pool";
    case Kind::monotonic: return "pmr_monotonic";
    case Kind::pool: return "pmr_pool";
    }
    throw std::logic_error("unknown backend");
}

struct ObjectCounts { std::size_t constructed{}, destroyed{}; };
struct alignas(16) Object {
    std::uint64_t value;
    ObjectCounts* counts;
    Object(std::uint64_t v, ObjectCounts& c) : value(v), counts(&c) { ++counts->constructed; }
    ~Object() { ++counts->destroyed; }
};
static_assert(sizeof(Object) <= 64 && alignof(Object) == 16);
constexpr std::size_t alignment = alignof(Object), slot_payload = 64;

std::vector<std::size_t> make_trace(std::size_t count, std::uint64_t seed) {
    std::vector<std::size_t> trace;
    trace.reserve(count);
    // Declared unsigned xorshift trace, same sequence for every backend/trial.
    auto state = seed ^ UINT64_C(0x9e3779b97f4a7c15);
    for (std::size_t i = 0; i < count; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        trace.push_back(sizeof(Object) + state % (slot_payload - sizeof(Object) + 1));
    }
    return trace;
}

// All allocations go through the same out-of-line dispatch; pointer table and
// full payload reads survive optimized builds. No LTO-specific barrier needed.
class Backend {
public:
    Backend(Kind kind, std::size_t count, CountingResource& upstream) : kind_(kind), upstream_(upstream) {
        if (kind == Kind::arena || kind == Kind::fixed || kind == Kind::monotonic)
            storage_.resize(count * 128 + 512);
        switch (kind) {
        case Kind::host: break;
        case Kind::arena: arena_ = std::make_unique<BumpArena>(storage_); break;
        case Kind::fixed:
            states_.resize(count);
            fixed_ = std::make_unique<FixedPool>(storage_, states_, slot_payload, alignment);
            if (!fixed_->valid() || fixed_->capacity() < count) throw std::logic_error("fixed pool capacity");
            break;
        case Kind::monotonic:
            monotonic_ = std::make_unique<std::pmr::monotonic_buffer_resource>(storage_.data(), storage_.size(), &upstream_);
            break;
        case Kind::pool: {
            std::pmr::pool_options options;
            options.max_blocks_per_chunk = 64;
            options.largest_required_pool_block = 256;
            pool_ = std::make_unique<std::pmr::unsynchronized_pool_resource>(options, &upstream_);
            break;
        }
        }
        if (kind == Kind::host) frontend_ = std::make_unique<CountingResource>(&upstream_);
        if (kind == Kind::monotonic) frontend_ = std::make_unique<CountingResource>(monotonic_.get());
        if (kind == Kind::pool) frontend_ = std::make_unique<CountingResource>(pool_.get());
    }

    [[gnu::noinline]] void* allocate(std::size_t bytes) {
        if (kind_ == Kind::arena) return arena_->allocate(bytes, alignment);
        if (kind_ == Kind::fixed) {
            if (bytes > slot_payload) return nullptr;
            return fixed_->allocate();
        }
        return frontend_->allocate(bytes, alignment);
    }

    [[gnu::noinline]] void deallocate(void* pointer, std::size_t bytes) {
        if (kind_ == Kind::fixed) {
            if (!fixed_->deallocate(pointer)) throw std::logic_error("valid fixed pool release rejected");
        } else if (frontend_) frontend_->deallocate(pointer, bytes, alignment);
        else throw std::logic_error("arena individual free is unsupported");
    }

    void release() {
        if (arena_) arena_->reset();
        if (monotonic_) monotonic_->release();
        if (pool_) pool_->release();
    }

    StorageMetrics metrics(std::size_t requested, std::size_t count) const {
        StorageMetrics m;
        m.requested_live_payload = requested;
        m.backing_capacity = storage_.size();
        m.resource_acquired_live_bytes = upstream_.counters().current_bytes;
        m.resource_acquired_total_bytes = upstream_.counters().allocated_bytes;
        m.resource_acquired_peak_bytes = upstream_.counters().peak_bytes;
        if (arena_) {
            m.consumed = arena_->consumed();
            m.alignment_slack = arena_->padding_bytes();
            m.slot_rounding = 0;
            m.metadata = sizeof(BumpArena); // control object, not hidden allocator metadata
        } else if (fixed_) {
            m.consumed = fixed_->live_count() * fixed_->slot_stride();
            m.alignment_slack = fixed_->alignment_padding();
            m.slot_rounding = count * fixed_->slot_stride() - requested;
            m.metadata = fixed_->metadata_reserved_bytes() + sizeof(FixedPool);
        } else if (kind_ == Kind::host) {
            m.consumed = requested; // client-visible only; host internals unknown
            m.alignment_slack = std::nullopt;
            m.slot_rounding = 0;
            m.metadata = std::nullopt;
        } // PMR internal cursors, slack and metadata are deliberately unknown.
        return m;
    }

    std::optional<ResourceCounters> frontend_counters() const {
        if (frontend_) return frontend_->counters();
        return std::nullopt;
    }

private:
    Kind kind_;
    CountingResource& upstream_;
    std::vector<std::byte> storage_;
    std::vector<SlotState> states_;
    std::unique_ptr<BumpArena> arena_;
    std::unique_ptr<FixedPool> fixed_;
    std::unique_ptr<std::pmr::monotonic_buffer_resource> monotonic_;
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> pool_;
    std::unique_ptr<CountingResource> frontend_; // destroyed before its underlying resource
};

std::uint64_t object_value(std::uint64_t seed, std::size_t index) {
    return seed + static_cast<std::uint64_t>(index) * UINT64_C(0x100000001b3);
}

[[gnu::noinline]] void construct_object(void* p, std::uint64_t value, ObjectCounts& counts) {
    std::construct_at(static_cast<Object*>(p), value, counts);
}
[[gnu::noinline]] void destroy_object(void* p) { std::destroy_at(static_cast<Object*>(p)); }

unsigned char byte_value(std::uint64_t seed, std::size_t i, std::size_t j) {
    return static_cast<unsigned char>(seed + static_cast<std::uint64_t>(i) * 17 + static_cast<std::uint64_t>(j) * 3);
}

[[gnu::noinline]] std::uint64_t touch_and_checksum(const std::vector<void*>& pointers,
                                                  const std::vector<std::size_t>& trace,
                                                  std::uint64_t seed) {
    for (std::size_t i = 0; i < trace.size(); ++i) {
        auto* bytes = static_cast<unsigned char*>(pointers[i]);
        for (std::size_t j = 0; j < trace[i]; ++j) bytes[j] = byte_value(seed, i, j);
    }
    // Separate pass observes all simultaneously-live allocations; overlaps would
    // corrupt prior blocks instead of being hidden by immediate write/read pairs.
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const auto* bytes = static_cast<const unsigned char*>(pointers[i]);
        for (std::size_t j = 0; j < trace[i]; ++j) checksum += bytes[j];
    }
    return checksum;
}

std::uint64_t expected_raw(const std::vector<std::size_t>& trace, std::uint64_t seed) {
    // Independent fixture expression, no allocator, pointer reads or helper above.
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < trace.size(); ++i)
        for (std::size_t j = 0; j < trace[i]; ++j)
            sum += (seed + static_cast<std::uint64_t>(i) * 17 + static_cast<std::uint64_t>(j) * 3) & 255U;
    return sum;
}

PhaseSample phase(Kind kind, const std::vector<std::size_t>& trace, std::uint64_t seed, bool objects) {
    PhaseSample result;
    const auto start_setup = Clock::now();
    CountingResource upstream(std::pmr::new_delete_resource(), 16 * 1024 * 1024);
    auto backend = std::make_unique<Backend>(kind, trace.size(), upstream);
    std::vector<void*> pointers(trace.size());
    ObjectCounts counts;
    result.setup_ns = elapsed(start_setup);
    result.payload_bytes = std::accumulate(trace.begin(), trace.end(), std::size_t{});
    auto start = Clock::now();
    std::size_t allocated = 0;
    try {
        for (; allocated < trace.size(); ++allocated) {
            pointers[allocated] = backend->allocate(trace[allocated]);
            if (!pointers[allocated]) throw std::bad_alloc();
        }
    } catch (...) {
        // All configurations are bounded to fit, but still honor raw lifetimes
        // if the system allocator fails before the trial is complete.
        if (kind != Kind::arena)
            for (std::size_t i = 0; i < allocated; ++i) backend->deallocate(pointers[i], trace[i]);
        throw;
    }
    result.allocate_ns = elapsed(start);
    result.allocations = allocated;
    for (void* pointer : pointers)
        if (reinterpret_cast<std::uintptr_t>(pointer) % alignment != 0) throw std::logic_error("misaligned workload block");
    result.storage = backend->metrics(result.payload_bytes, allocated);
    if (objects) {
        start = Clock::now();
        for (std::size_t i = 0; i < pointers.size(); ++i) construct_object(pointers[i], object_value(seed, i), counts);
        result.construct_ns = elapsed(start);
        for (void* pointer : pointers) result.checksum += static_cast<Object*>(pointer)->value;
        // Closed-form unsigned sum, distinct from the per-object generator.
        const auto n = static_cast<std::uint64_t>(pointers.size());
        result.expected_checksum = n * seed + ((n * (n - 1)) / 2) * UINT64_C(0x100000001b3);
        start = Clock::now();
        for (void* pointer : pointers) destroy_object(pointer);
        result.destroy_ns = elapsed(start);
        result.constructed = counts.constructed;
        result.destroyed = counts.destroyed;
    } else {
        result.checksum = touch_and_checksum(pointers, trace, seed);
        result.expected_checksum = expected_raw(trace, seed);
    }
    if (kind != Kind::arena) {
        start = Clock::now();
        for (std::size_t i = 0; i < pointers.size(); ++i) backend->deallocate(pointers[i], trace[i]);
        result.deallocate_ns = elapsed(start);
        result.deallocations = pointers.size();
    }
    result.storage.upstream_after_deallocate = upstream.counters().current_bytes;
    result.storage.retained_not_live_after_deallocate = result.storage.backing_capacity + upstream.counters().current_bytes;
    if (kind == Kind::arena || kind == Kind::monotonic)
        result.storage.retained_dead_payload_after_deallocate = result.payload_bytes;
    if (kind == Kind::fixed || kind == Kind::host) result.storage.retained_dead_payload_after_deallocate = 0;
    result.frontend = backend->frontend_counters();
    if (kind == Kind::arena || kind == Kind::monotonic || kind == Kind::pool) {
        start = Clock::now();
        backend->release(); // all objects are dead, no owning containers remain
        result.release_ns = elapsed(start);
    }
    start = Clock::now();
    backend.reset(); // releases resources before the upstream counter dies
    result.teardown_ns = elapsed(start);
    result.upstream = upstream.counters();
    result.storage.final_upstream_bytes = upstream.counters().current_bytes;
    result.fixture_ok = result.checksum == result.expected_checksum && result.storage.final_upstream_bytes == 0
        && result.constructed == (objects ? pointers.size() : 0) && result.constructed == result.destroyed;
    result.allocation_operations_per_second = operations_per_second(allocated, result.allocate_ns);
    result.timing_valid = result.allocation_operations_per_second.has_value();
    const auto valid_optional = [](std::optional<double> ns) { return !ns || (*ns > 0 && std::isfinite(*ns)); };
    result.timing_valid = result.timing_valid && result.setup_ns > 0 && result.teardown_ns > 0
        && valid_optional(result.construct_ns) && valid_optional(result.destroy_ns)
        && valid_optional(result.deallocate_ns) && valid_optional(result.release_ns);
    return result;
}

template <class T> void optional_json(std::ostream& out, const std::optional<T>& value) {
    if (value) out << *value;
    else out << "null";
}

void metrics_json(std::ostream& out, const StorageMetrics& m) {
    out << "{\"requested_live_payload\":" << m.requested_live_payload
        << ",\"backing_capacity\":" << m.backing_capacity
        << ",\"resource_acquired_live_bytes\":" << m.resource_acquired_live_bytes
        << ",\"resource_acquired_total_bytes\":" << m.resource_acquired_total_bytes
        << ",\"resource_acquired_peak_bytes\":" << m.resource_acquired_peak_bytes << ",\"consumed\":";
    optional_json(out, m.consumed);
    out << ",\"alignment_slack\":"; optional_json(out, m.alignment_slack);
    out << ",\"slot_rounding\":"; optional_json(out, m.slot_rounding);
    out << ",\"metadata\":"; optional_json(out, m.metadata);
    out << ",\"retained_not_live_after_deallocate\":" << m.retained_not_live_after_deallocate
        << ",\"requested_live_payload_after_deallocate\":0,\"upstream_after_deallocate\":" << m.upstream_after_deallocate
        << ",\"retained_dead_payload_after_deallocate\":";
    optional_json(out, m.retained_dead_payload_after_deallocate);
    out << ",\"final_upstream_bytes\":" << m.final_upstream_bytes
        << ",\"host_allocator_hidden_bytes\":null,\"pmr_cursor_or_internal_metadata\":null}";
}

void phase_json(std::ostream& out, const PhaseSample& p) {
    out << "{\"timings_ns\":{\"setup\":" << p.setup_ns << ",\"allocate\":" << p.allocate_ns << ",\"deallocate\":";
    optional_json(out, p.deallocate_ns);
    out << ",\"construct\":"; optional_json(out, p.construct_ns);
    out << ",\"destroy\":"; optional_json(out, p.destroy_ns);
    out << ",\"release_or_reset\":"; optional_json(out, p.release_ns);
    out << ",\"teardown\":" << p.teardown_ns << "},\"allocation_operations_per_second\":";
    optional_json(out, p.allocation_operations_per_second);
    out << ",\"allocations\":" << p.allocations << ",\"deallocations\":" << p.deallocations
        << ",\"constructed\":" << p.constructed << ",\"destroyed\":" << p.destroyed
        << ",\"payload_bytes\":" << p.payload_bytes << ",\"checksum\":" << p.checksum
        << ",\"expected_checksum\":" << p.expected_checksum
        << ",\"fixture_ok\":" << (p.fixture_ok ? "true" : "false")
        << ",\"timing_valid\":" << (p.timing_valid ? "true" : "false")
        << ",\"errors\":[";
    bool comma = false;
    if (!p.fixture_ok) { out << "\"fixture mismatch\""; comma = true; }
    if (!p.timing_valid) { if (comma) out << ','; out << "\"zero, negative or non-finite elapsed; invalid rate is null\""; }
    out << "],\"storage_bytes\":";
    metrics_json(out, p.storage);
    out << ",\"frontend\":";
    if (p.frontend) write_counters_json(out, *p.frontend);
    else out << "null";
    out << ",\"frontend_null_reason\":";
    if (p.frontend) out << "null";
    else out << "\"custom raw API has no PMR frontend; dispatch counts appear in allocations/deallocations\"";
    out << ",\"upstream\":"; write_counters_json(out, p.upstream);
    out << '}';
}

void holes_json(std::ostream& out, const std::vector<Interval>& holes) {
    out << '[';
    for (std::size_t i = 0; i < holes.size(); ++i) {
        if (i) out << ',';
        out << "{\"offset\":" << holes[i].offset << ",\"size\":" << holes[i].size << '}';
    }
    out << ']';
}
} // namespace

ExperimentReport run_experiments(const ExperimentConfig& config) {
    validate_config(config);
    ExperimentReport result;
    result.config = config;
    result.trace_bytes = make_trace(config.size, config.seed);
    result.fragmentation = fragmentation_fixture();
    for (const Kind kind : kinds) {
        BackendReport backend;
        backend.name = name(kind);
        const auto warm_trace = make_trace(std::min<std::size_t>(config.size, 16), config.seed);
        const auto start = Clock::now();
        const auto warm_raw = phase(kind, warm_trace, config.seed, false);
        const auto warm_objects = phase(kind, warm_trace, config.seed, true);
        backend.warmup_ns = elapsed(start);
        if (!warm_raw.fixture_ok || !warm_objects.fixture_ok) throw std::logic_error("warmup fixture mismatch");
        for (std::size_t repetition = 0; repetition < config.repetitions; ++repetition)
            backend.samples.push_back({repetition, phase(kind, result.trace_bytes, config.seed, false),
                                       phase(kind, result.trace_bytes, config.seed, true)});
        result.backends.push_back(std::move(backend));
    }
    return result;
}

void write_experiments_json(std::ostream& out, const ExperimentReport& r) {
    out << "{\"workload\":\"bulk lifetime: allocate all, observe all live payloads, destroy objects, free where supported, reset/release\","
           "\"trace_algorithm\":\"xorshift64(13,7,17), initial seed xor 0x9e3779b97f4a7c15; sizeof(Object)+state%(65-sizeof(Object))\","
           "\"size\":" << r.config.size << ",\"seed\":" << r.config.seed << ",\"repetitions\":" << r.config.repetitions
        << ",\"alignment\":" << alignment << ",\"object_size\":" << sizeof(Object)
        << ",\"fixed_slot_payload\":64,\"upstream_live_byte_limit\":16777216,\"trace_bytes\":[";
    for (std::size_t i = 0; i < r.trace_bytes.size(); ++i) { if (i) out << ','; out << r.trace_bytes[i]; }
    out << "],\"measurement\":{\"clock\":\"steady_clock\",\"units\":\"ns and operations/second\","
           "\"separation\":\"fresh resource per phase/trial; setup and bounded warmup separate; fill/checksum outside allocation timer; object lifetime separate; teardown includes resource and backing-buffer destruction\","
           "\"dispatch\":\"common noinline allocation/deallocation dispatch and externally observed full payload/pointer table\","
           "\"instrumentation\":\"PMR frontend/upstream counting and fixed-pool checked-free overhead are included, not subtracted; custom raw APIs are not PMR-wrapped\","
           "\"interpretation\":\"no speedup requirement; differing reclamation mechanics and fixed-pool O(capacity) checked frees are included\","
           "\"null_reason\":\"unsupported timer/metric or opaque PMR/host internals; not measured zeros\"},"
           "\"metric_definitions\":{"
           "\"requested_live_payload\":\"sum trace bytes while all blocks live; object phase includes reserved bytes beyond object size\","
           "\"backing_capacity\":\"caller byte-buffer size, excluding typed slot metadata\","
           "\"resource_acquired\":\"counted upstream requested bytes, not host hidden size or RSS\","
           "\"consumed\":\"arena cursor; live fixed-slot strides; host client-visible payload only; PMR unknown\","
           "\"alignment_slack\":\"arena cumulative gaps or fixed-pool leading alignment gap; PMR/host unknown\","
           "\"slot_rounding\":\"live fixed-slot strides minus requested payload; non-slot host/arena zero, PMR unknown\","
           "\"metadata\":\"custom control object sizeof plus fixed-pool reserved typed metadata; excludes common benchmark wrappers/tables; PMR/host unknown\","
           "\"retained_not_live_after_deallocate\":\"caller byte-buffer capacity plus upstream current bytes when client live payload is zero; includes unused capacity, excludes separately listed typed metadata\","
           "\"retained_dead_payload_after_deallocate\":\"known unreclaimed workload payload for arena/monotonic, zero after immediate custom-pool/host frees; standard pool reuse/retention opaque\"},"
           "\"unsupported\":[\"arena individual free\",\"fixed-slot requests over 64 bytes or alignment over configured 16\","
           "\"general variable-size individually reclaimed arena/fixed-pool equivalence\"],\"backends\":[";
    for (std::size_t i = 0; i < r.backends.size(); ++i) {
        if (i) out << ',';
        const auto& b = r.backends[i];
        out << "{\"name\":\"" << b.name << "\",\"warmup_ns\":" << b.warmup_ns << ",\"samples\":[";
        for (std::size_t j = 0; j < b.samples.size(); ++j) {
            if (j) out << ',';
            out << "{\"repetition\":" << b.samples[j].repetition << ",\"raw\":";
            phase_json(out, b.samples[j].raw);
            out << ",\"objects\":"; phase_json(out, b.samples[j].objects);
            out << '}';
        }
        out << "]}";
    }
    const auto& f = r.fragmentation;
    out << "],\"fragmentation\":{\"kind\":\"deterministic free-interval model, not libc heap\",\"capacity\":" << f.capacity
        << ",\"failed_request\":" << f.failed_request << ",\"fragmented_total_free\":" << f.fragmented_total
        << ",\"fragmented_largest_free\":" << f.fragmented_largest << ",\"fragmented_failed\":" << (f.fragmented_failed ? "true" : "false")
        << ",\"contiguous_succeeded\":" << (f.contiguous_succeeded ? "true" : "false") << ",\"fragmented_holes\":";
    holes_json(out, f.fragmented_holes);
    out << ",\"before_merge_total\":" << f.before_merge_total << ",\"before_merge_largest\":" << f.before_merge_largest
        << ",\"after_merge_total\":" << f.after_merge_total << ",\"after_merge_largest\":" << f.after_merge_largest
        << ",\"after_merge_succeeded\":" << (f.after_merge_succeeded ? "true" : "false") << ",\"coalesced_holes\":";
    holes_json(out, f.coalesced_holes);
    out << ",\"interpretation\":\"external fragmentation is separated holes; adjacent-hole coalescing is explicit. Slot rounding is internal loss, free slots are not automatically external fragmentation. Arena/monotonic dead allocations are temporal retention until reset/release.\"}}";
}

} // namespace memory_lab
