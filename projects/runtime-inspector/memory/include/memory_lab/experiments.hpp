#pragma once

#include "memory_lab/pmr.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace memory_lab {

struct ExperimentConfig { std::size_t size{128}; std::uint64_t seed{42}; std::size_t repetitions{3}; };
void validate_config(const ExperimentConfig&);
std::optional<double> operations_per_second(std::size_t operations, double nanoseconds) noexcept;

struct Interval { std::size_t offset{}, size{}; };
// Tiny first-fit free-interval teaching model, NOT a host-heap reconstruction or
// general allocator. Release does not merge; coalesce() is an explicit step.
class FreeIntervalModel {
public:
    explicit FreeIntervalModel(std::size_t capacity);
    std::optional<std::size_t> allocate(std::size_t bytes);
    bool release(std::size_t offset);
    void coalesce();
    std::size_t total_free() const noexcept;
    std::size_t largest_free() const noexcept;
    const std::vector<Interval>& holes() const noexcept { return holes_; }
private:
    std::vector<Interval> holes_, live_;
};

struct FragmentationReport {
    std::size_t capacity{}, failed_request{}, fragmented_total{}, fragmented_largest{};
    std::size_t before_merge_total{}, before_merge_largest{}, after_merge_total{}, after_merge_largest{};
    bool fragmented_failed{}, contiguous_succeeded{}, after_merge_succeeded{};
    std::vector<Interval> fragmented_holes, coalesced_holes;
};
FragmentationReport fragmentation_fixture();

struct StorageMetrics {
    std::size_t requested_live_payload{}, backing_capacity{}, resource_acquired_live_bytes{};
    std::size_t resource_acquired_total_bytes{}, resource_acquired_peak_bytes{};
    std::optional<std::size_t> consumed, alignment_slack, slot_rounding, metadata;
    std::size_t retained_not_live_after_deallocate{}, upstream_after_deallocate{};
    std::optional<std::size_t> retained_dead_payload_after_deallocate;
    std::size_t final_upstream_bytes{};
};

struct PhaseSample {
    double setup_ns{}, allocate_ns{}, teardown_ns{};
    std::optional<double> deallocate_ns, construct_ns, destroy_ns, release_ns;
    std::optional<double> allocation_operations_per_second;
    std::size_t allocations{}, deallocations{}, constructed{}, destroyed{}, payload_bytes{};
    std::uint64_t checksum{}, expected_checksum{};
    bool fixture_ok{}, timing_valid{};
    StorageMetrics storage;
    std::optional<ResourceCounters> frontend;
    ResourceCounters upstream{};
};
struct AllocationSample { std::size_t repetition{}; PhaseSample raw, objects; };
struct BackendReport { std::string name; double warmup_ns{}; std::vector<AllocationSample> samples; };
struct ExperimentReport {
    ExperimentConfig config;
    std::vector<std::size_t> trace_bytes;
    std::vector<BackendReport> backends;
    FragmentationReport fragmentation;
};

ExperimentReport run_experiments(const ExperimentConfig&);
void write_experiments_json(std::ostream&, const ExperimentReport&);

} // namespace memory_lab
