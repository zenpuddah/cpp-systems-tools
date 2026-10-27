#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <memory_resource>
#include <optional>

namespace memory_lab {

// Counts requests reaching this layer, not host allocator overhead. No per-call
// allocation ledger: callers must obey memory_resource deallocation preconditions.
struct ResourceCounters {
    std::size_t allocation_requests{}, allocations{}, failures{}, deallocations{};
    std::size_t allocated_bytes{}, deallocated_bytes{}, current_bytes{}, peak_bytes{};
    // Index log2(alignment); counts successful allocations, including zero bytes.
    std::array<std::size_t, std::numeric_limits<std::size_t>::digits> alignments{};
};

class CountingResource final : public std::pmr::memory_resource {
public:
    explicit CountingResource(std::pmr::memory_resource* upstream,
                              std::size_t live_byte_limit = std::numeric_limits<std::size_t>::max());
    const ResourceCounters& counters() const noexcept { return counters_; }
    CountingResource(const CountingResource&) = delete;
    CountingResource& operator=(const CountingResource&) = delete;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
    std::pmr::memory_resource* upstream_;
    std::size_t limit_;
    ResourceCounters counters_{};
};

struct PmrRoute {
    ResourceCounters frontend{}, upstream{};
    std::size_t initial_buffer_bytes{};
    std::optional<std::size_t> upstream_before_block_free, upstream_after_block_free;
    std::size_t upstream_before_release{}, upstream_after_release{}, final_upstream_bytes{};
    std::size_t constructed{}, destroyed{};
    bool fixture_ok{}, bad_alloc_observed{};
};

struct PmrReport {
    PmrRoute monotonic_fallback, monotonic_hardcap, pool;
    bool copy_uses_default{}, explicit_copy_uses_destination{};
    bool move_uses_source{}, explicit_move_uses_destination{}, move_assignment_keeps_destination{};
    bool copy_move_values_ok{};
};

// Bounded demonstrations: size 1..2048, containers at most 64 elements.
PmrReport observe_pmr(std::size_t size, std::uint64_t seed);
void write_counters_json(std::ostream&, const ResourceCounters&);
void write_pmr_json(std::ostream&, const PmrReport&);

} // namespace memory_lab
