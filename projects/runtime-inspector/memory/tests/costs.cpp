#include "memory_lab/experiments.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("CHECK failed: " #condition); } while (false)

namespace {
void rate_tests() {
    CHECK(memory_lab::operations_per_second(3, 2.0e9) == 1.5);
    CHECK(memory_lab::operations_per_second(0, 1) == 0);
    for (double value : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        CHECK(!memory_lab::operations_per_second(1, value));
    CHECK(!memory_lab::operations_per_second(2048, std::numeric_limits<double>::denorm_min()));
}

void model_tests() {
    const auto f = memory_lab::fragmentation_fixture();
    CHECK(f.capacity == 64 && f.failed_request == 24);
    CHECK(f.fragmented_total == 32 && f.fragmented_largest == 16);
    CHECK(f.fragmented_failed && f.contiguous_succeeded && f.after_merge_succeeded);
    CHECK(f.before_merge_total == 48 && f.before_merge_largest == 16);
    CHECK(f.after_merge_total == 48 && f.after_merge_largest == 48);
    CHECK(f.fragmented_holes.size() == 2 && f.fragmented_holes[0].offset == 0 && f.fragmented_holes[1].offset == 32);
    CHECK(f.coalesced_holes.size() == 1 && f.coalesced_holes[0].offset == 0 && f.coalesced_holes[0].size == 48);
    memory_lab::FreeIntervalModel m(17);
    CHECK(!m.allocate(0) && !m.allocate(18));
    CHECK(m.total_free() == 17 && m.largest_free() == 17);
    CHECK(m.allocate(1) == 0 && m.allocate(16) == 1);
    CHECK(m.total_free() == 0 && m.largest_free() == 0 && !m.allocate(1));
    CHECK(!m.release(2) && !m.release(18));
    CHECK(m.release(0) && !m.release(0) && m.release(1));
    CHECK(m.total_free() == 17 && m.largest_free() == 16);
    m.coalesce();
    CHECK(m.holes().size() == 1 && m.largest_free() == 17);
    m.coalesce();
    CHECK(m.allocate(17) == 0 && m.total_free() == 0);
    for (std::size_t invalid : {std::size_t{0}, std::size_t{4097}}) {
        bool caught = false;
        try { memory_lab::FreeIntervalModel bad(invalid); } catch (const std::invalid_argument&) { caught = true; }
        CHECK(caught);
    }
}

void equivalence_tests() {
    for (const std::size_t size : {std::size_t{1}, std::size_t{23}, std::size_t{2048}}) {
        const auto seed = std::numeric_limits<std::uint64_t>::max() - size;
        const auto r = memory_lab::run_experiments({size, seed, 2});
        CHECK(r.trace_bytes.size() == size && r.backends.size() == 5);
        const auto payload = std::accumulate(r.trace_bytes.begin(), r.trace_bytes.end(), std::size_t{});
        // Independent byte fixture, not copied from any observed resource result.
        std::uint64_t raw_sum = 0, object_sum = 0;
        for (std::size_t i = 0; i < size; ++i) {
            CHECK(r.trace_bytes[i] >= 16 && r.trace_bytes[i] <= 64);
            for (std::size_t j = 0; j < r.trace_bytes[i]; ++j)
                raw_sum += static_cast<unsigned char>(seed + i * 17 + j * 3);
            object_sum += seed + static_cast<std::uint64_t>(i) * UINT64_C(0x100000001b3);
        }
        for (const auto& backend : r.backends) {
            CHECK(backend.samples.size() == 2);
            for (const auto& sample : backend.samples) {
                CHECK(sample.raw.checksum == raw_sum && sample.objects.checksum == object_sum);
                for (const auto* p : {&sample.raw, &sample.objects}) {
                    CHECK(p->fixture_ok && p->allocations == size && p->payload_bytes == payload);
                    CHECK(p->storage.requested_live_payload == payload && p->storage.final_upstream_bytes == 0);
                    CHECK(p->upstream.current_bytes == 0 && p->upstream.allocated_bytes == p->upstream.deallocated_bytes);
                    if (backend.name == "bump_arena") {
                        CHECK(p->deallocations == 0 && !p->deallocate_ns && p->release_ns.has_value());
                        CHECK(p->storage.consumed == payload + *p->storage.alignment_slack);
                        CHECK(p->storage.retained_dead_payload_after_deallocate == payload);
                    } else {
                        CHECK(p->deallocations == size && p->deallocate_ns.has_value());
                    }
                    if (backend.name == "fixed_pool") {
                        CHECK(*p->storage.consumed == size * 64);
                        CHECK(*p->storage.slot_rounding == size * 64 - payload);
                        CHECK(*p->storage.metadata > 0);
                        CHECK(p->storage.retained_dead_payload_after_deallocate == 0);
                    }
                    if (backend.name == "pmr_monotonic" || backend.name == "pmr_pool") {
                        CHECK(!p->storage.consumed && !p->storage.alignment_slack && !p->storage.metadata);
                        CHECK(p->frontend && p->frontend->allocations == size && p->frontend->current_bytes == 0);
                    }
                    if (backend.name == "new_delete_resource") {
                        CHECK(p->upstream.allocations == size && p->upstream.peak_bytes == payload);
                        CHECK(p->storage.retained_not_live_after_deallocate == 0);
                    }
                }
                CHECK(sample.objects.constructed == size && sample.objects.destroyed == size);
                CHECK(sample.raw.constructed == 0 && sample.raw.destroyed == 0);
                CHECK(sample.objects.construct_ns && sample.objects.destroy_ns);
                CHECK(!sample.raw.construct_ns && !sample.raw.destroy_ns);
            }
        }
        std::ostringstream json;
        memory_lab::write_experiments_json(json, r);
        CHECK(json.str().find("\"host_allocator_hidden_bytes\":null") != std::string::npos);
    }
    for (const memory_lab::ExperimentConfig c : {memory_lab::ExperimentConfig{0, 0, 1}, {2049, 0, 1}, {1, 0, 0}, {1, 0, 21}}) {
        bool caught = false;
        try { (void)memory_lab::run_experiments(c); } catch (const std::invalid_argument&) { caught = true; }
        CHECK(caught);
    }
    const auto a = memory_lab::run_experiments({7, 3, 1});
    const auto b = memory_lab::run_experiments({7, 3, 1});
    CHECK(a.trace_bytes == b.trace_bytes);
    const auto different = memory_lab::run_experiments({7, 4, 1});
    CHECK(a.trace_bytes != different.trace_bytes);
}
} // namespace

int main() {
    try { rate_tests(); model_tests(); equivalence_tests(); std::cout << "Cost/model checks passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
