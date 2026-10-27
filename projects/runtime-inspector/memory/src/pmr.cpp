#include "memory_lab/pmr.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <new>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace memory_lab {

CountingResource::CountingResource(std::pmr::memory_resource* upstream, std::size_t limit)
    : upstream_(upstream), limit_(limit) {
    if (upstream == nullptr) throw std::invalid_argument("null counting upstream");
}

void* CountingResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    ++counters_.allocation_requests;
    try {
        // Invalid alignments are rejected before forwarding, not used to exercise
        // undefined behavior in a standard resource.
        if (!std::has_single_bit(alignment) || bytes > limit_ - counters_.current_bytes
            || bytes > std::numeric_limits<std::size_t>::max() - counters_.allocated_bytes)
            throw std::bad_alloc();
        void* result = upstream_->allocate(bytes, alignment);
        ++counters_.allocations;
        counters_.allocated_bytes += bytes;
        counters_.current_bytes += bytes;
        counters_.peak_bytes = std::max(counters_.peak_bytes, counters_.current_bytes);
        ++counters_.alignments[std::countr_zero(alignment)];
        return result;
    } catch (...) {
        ++counters_.failures;
        throw;
    }
}

void CountingResource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
    upstream_->deallocate(pointer, bytes, alignment);
    ++counters_.deallocations;
    counters_.deallocated_bytes += bytes;
    counters_.current_bytes -= bytes;
}

bool CountingResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

namespace {
struct Counts { std::size_t constructed{}, destroyed{}; };
struct Counted {
    Counts* counts;
    std::uint64_t value;
    Counted(Counts& c, std::uint64_t v) : counts(&c), value(v) { ++c.constructed; }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    Counted(Counted&& other) noexcept : counts(other.counts), value(other.value) { ++counts->constructed; }
    ~Counted() { ++counts->destroyed; }
};

PmrRoute monotonic_route(std::size_t n, std::uint64_t seed, bool hardcap) {
    PmrRoute result;
    alignas(64) std::array<std::byte, 512> buffer{};
    result.initial_buffer_bytes = buffer.size();
    CountingResource upstream(hardcap ? std::pmr::null_memory_resource() : std::pmr::new_delete_resource(),
                              1024 * 1024);
    Counts counts;
    {
        std::pmr::monotonic_buffer_resource resource(buffer.data(), buffer.size(), &upstream);
        CountingResource frontend(&resource);
        {
            // Small fixtures first prove the supplied-buffer route independent of
            // vector growth heuristics or string small-string optimization.
            std::pmr::vector<std::uint64_t> values(&frontend);
            const std::size_t used = hardcap ? 4 : n;
            values.reserve(used);
            for (std::size_t i = 0; i < used; ++i) values.push_back(seed + i);
            std::pmr::string text(&frontend);
            text.assign(hardcap ? 40 : n + 96, 'm');
            std::pmr::vector<Counted> objects(&frontend);
            objects.reserve(2);
            objects.emplace_back(counts, seed);
            objects.emplace_back(counts, seed + 1);
            bool ok = text.size() == (hardcap ? 40 : n + 96)
                      && std::all_of(text.begin(), text.end(), [](char c) { return c == 'm'; })
                      && objects[1].value == seed + 1;
            for (std::size_t i = 0; i < used; ++i) ok = ok && values[i] == seed + i;
            result.fixture_ok = ok;
            // Larger than the entire supplied buffer: fallback/failure cannot
            // depend on an implementation-specific cursor or growth factor.
            try {
                void* block = frontend.allocate(4096, 64);
                result.upstream_before_block_free = upstream.counters().current_bytes;
                frontend.deallocate(block, 4096, 64);
                result.upstream_after_block_free = upstream.counters().current_bytes;
            } catch (const std::bad_alloc&) {
                result.bad_alloc_observed = true;
                if (!hardcap) throw;
            }
        } // all containers destroy their elements AND surrender their capacity
        result.frontend = frontend.counters();
        result.upstream_before_release = upstream.counters().current_bytes;
        resource.release(); // never while a cleared-but-capacity-owning vector lives
        result.upstream_after_release = upstream.counters().current_bytes;
    }
    result.upstream = upstream.counters();
    result.final_upstream_bytes = upstream.counters().current_bytes;
    result.constructed = counts.constructed;
    result.destroyed = counts.destroyed;
    return result;
}

PmrRoute pool_route(std::size_t n, std::uint64_t seed) {
    PmrRoute result;
    // A bounded *upstream byte budget*, not an assertion about chunk count.
    CountingResource upstream(std::pmr::new_delete_resource(), 1024 * 1024);
    Counts counts;
    {
        std::pmr::pool_options options;
        options.max_blocks_per_chunk = 8;
        options.largest_required_pool_block = 256;
        std::pmr::unsynchronized_pool_resource resource(options, &upstream);
        CountingResource frontend(&resource);
        bool ok = true;
        for (int round = 0; round < 3; ++round) {
            std::pmr::vector<std::uint64_t> values(&frontend);
            values.reserve(n);
            for (std::size_t i = 0; i < n; ++i) values.push_back(seed + i);
            std::pmr::string text(n + 48, 'p', &frontend);
            for (std::size_t i = 0; i < n; ++i) ok = ok && values[i] == seed + i;
            ok = ok && text.size() == n + 48
                 && std::all_of(text.begin(), text.end(), [](char c) { return c == 'p'; });
            std::pmr::vector<Counted> objects(&frontend);
            objects.reserve(2);
            objects.emplace_back(counts, seed);
            objects.emplace_back(counts, seed + 1);
        }
        result.frontend = frontend.counters();
        result.fixture_ok = ok;
        result.upstream_before_release = upstream.counters().current_bytes;
        resource.release();
        result.upstream_after_release = upstream.counters().current_bytes;
    }
    result.upstream = upstream.counters();
    result.final_upstream_bytes = upstream.counters().current_bytes;
    result.constructed = counts.constructed;
    result.destroyed = counts.destroyed;
    return result;
}

void observe_copy_move(PmrReport& report) {
    // Resources declared before containers; no process-wide default mutation.
    CountingResource left(std::pmr::new_delete_resource());
    CountingResource right(std::pmr::new_delete_resource());
    {
        std::pmr::vector<int> source({7, 11, 19}, &left);
        std::pmr::vector<int> copy(source);
        report.copy_uses_default = copy.get_allocator().resource() == std::pmr::get_default_resource();
        std::pmr::vector<int> explicit_copy(source, &right);
        report.explicit_copy_uses_destination = explicit_copy.get_allocator().resource() == &right;
        std::pmr::vector<int> moved(std::move(source));
        report.move_uses_source = moved.get_allocator().resource() == &left;
        std::pmr::vector<int> explicit_move(std::move(moved), &right);
        report.explicit_move_uses_destination = explicit_move.get_allocator().resource() == &right;
        std::pmr::vector<int> assigned(&left);
        assigned = std::move(explicit_move);
        report.move_assignment_keeps_destination = assigned.get_allocator().resource() == &left;
        const std::vector<int> fixture{7, 11, 19};
        const auto equal = [&](const auto& values) {
            return values.size() == fixture.size() && std::equal(values.begin(), values.end(), fixture.begin());
        };
        report.copy_move_values_ok = equal(copy) && equal(explicit_copy) && equal(assigned);
    }
    report.copy_move_values_ok = report.copy_move_values_ok && left.counters().current_bytes == 0
                               && right.counters().current_bytes == 0;
}

void write_route(std::ostream& out, const PmrRoute& r) {
    out << "{\"frontend\":";
    write_counters_json(out, r.frontend);
    out << ",\"upstream\":";
    write_counters_json(out, r.upstream);
    out << ",\"initial_buffer_bytes\":" << r.initial_buffer_bytes
        << ",\"upstream_before_block_free\":";
    if (r.upstream_before_block_free) out << *r.upstream_before_block_free;
    else out << "null";
    out << ",\"upstream_after_block_free\":";
    if (r.upstream_after_block_free) out << *r.upstream_after_block_free;
    else out << "null";
    out << ",\"block_free_snapshot_null_reason\":";
    if (r.upstream_before_block_free) out << "null";
    else out << "\"no successful explicit block/free pair in this route\"";
    out << ",\"upstream_before_release\":" << r.upstream_before_release
        << ",\"upstream_after_release\":" << r.upstream_after_release
        << ",\"final_upstream_bytes\":" << r.final_upstream_bytes
        << ",\"constructed\":" << r.constructed << ",\"destroyed\":" << r.destroyed
        << ",\"fixture_ok\":" << (r.fixture_ok ? "true" : "false")
        << ",\"bad_alloc_observed\":" << (r.bad_alloc_observed ? "true" : "false") << '}';
}
} // namespace

PmrReport observe_pmr(std::size_t size, std::uint64_t seed) {
    if (size == 0 || size > 2048) throw std::invalid_argument("PMR size must be 1..2048");
    PmrReport result;
    const auto n = std::min<std::size_t>(size, 64);
    result.monotonic_fallback = monotonic_route(n, seed, false);
    result.monotonic_hardcap = monotonic_route(n, seed, true);
    result.pool = pool_route(n, seed);
    observe_copy_move(result);
    return result;
}

void write_counters_json(std::ostream& out, const ResourceCounters& c) {
    out << "{\"allocation_requests\":" << c.allocation_requests << ",\"allocations\":" << c.allocations
        << ",\"failures\":" << c.failures << ",\"deallocations\":" << c.deallocations
        << ",\"allocated_bytes\":" << c.allocated_bytes << ",\"deallocated_bytes\":" << c.deallocated_bytes
        << ",\"current_bytes\":" << c.current_bytes << ",\"peak_bytes\":" << c.peak_bytes
        << ",\"successful_alignment_counts\":[";
    bool comma = false;
    for (std::size_t i = 0; i < c.alignments.size(); ++i) {
        if (c.alignments[i] == 0) continue;
        if (comma) out << ',';
        comma = true;
        out << "{\"alignment\":" << (std::size_t{1} << i) << ",\"count\":" << c.alignments[i] << '}';
    }
    out << "]}";
}

void write_pmr_json(std::ostream& out, const PmrReport& r) {
    out << "{\"monotonic_fallback\":";
    write_route(out, r.monotonic_fallback);
    out << ",\"monotonic_hardcap\":";
    write_route(out, r.monotonic_hardcap);
    out << ",\"pool\":";
    write_route(out, r.pool);
    out << ",\"copy_move\":{\"copy_uses_default\":" << (r.copy_uses_default ? "true" : "false")
        << ",\"explicit_copy_uses_destination\":" << (r.explicit_copy_uses_destination ? "true" : "false")
        << ",\"move_uses_source\":" << (r.move_uses_source ? "true" : "false")
        << ",\"explicit_move_uses_destination\":" << (r.explicit_move_uses_destination ? "true" : "false")
        << ",\"move_assignment_keeps_destination\":" << (r.move_assignment_keeps_destination ? "true" : "false")
        << ",\"fixture_ok\":" << (r.copy_move_values_ok ? "true" : "false")
        << "},\"policies\":{\"pool_upstream_live_byte_limit\":1048576,"
           "\"pool_chunk_counts\":\"observed, not fixed; metadata may allocate upstream\","
           "\"release\":\"destroy containers and objects before collective release; clear alone is insufficient\","
           "\"counter_limit\":\"counts explicit requests only; no host allocator internals or per-call heap ledger\"}}";
}

} // namespace memory_lab
