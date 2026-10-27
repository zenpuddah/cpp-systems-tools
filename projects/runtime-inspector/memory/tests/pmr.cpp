#include "memory_lab/pmr.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <sstream>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("CHECK failed: " #condition); } while (false)

namespace {
void counter_tests() {
    using memory_lab::CountingResource;
    CountingResource resource(std::pmr::new_delete_resource(), 128);
    void* a = resource.allocate(32, 16);
    void* b = resource.allocate(64, 64);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % 16 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
    CHECK(resource.counters().current_bytes == 96);
    CHECK(resource.counters().peak_bytes == 96);
    CHECK(resource.counters().alignments[4] == 1 && resource.counters().alignments[6] == 1);
    for (const auto bytes : {std::size_t{33}, std::numeric_limits<std::size_t>::max()}) {
        bool caught = false;
        try { (void)resource.allocate(bytes, 16); } catch (const std::bad_alloc&) { caught = true; }
        CHECK(caught);
        CHECK(resource.counters().current_bytes == 96 && resource.counters().allocations == 2);
    }
    void* exact = resource.allocate(32, 8);
    CHECK(resource.counters().current_bytes == 128 && resource.counters().peak_bytes == 128);
    resource.deallocate(exact, 32, 8);
    resource.deallocate(a, 32, 16);
    resource.deallocate(b, 64, 64);
    CHECK(resource.counters().current_bytes == 0);
    CHECK(resource.counters().allocated_bytes == resource.counters().deallocated_bytes);
    CHECK(resource.counters().allocation_requests == 5 && resource.counters().failures == 2);
    CHECK(resource.counters().deallocations == 3);
    void* zero = resource.allocate(0, 1); // standard-resource policy, not arena policy
    resource.deallocate(zero, 0, 1);
    CHECK(resource.counters().current_bytes == 0 && resource.counters().deallocations == 4);
    CHECK(resource.is_equal(resource));
    CountingResource other(std::pmr::new_delete_resource());
    CHECK(!resource.is_equal(other));
    bool invalid = false;
    try { CountingResource null(nullptr); } catch (const std::invalid_argument&) { invalid = true; }
    CHECK(invalid);
    CountingResource denied(std::pmr::null_memory_resource());
    bool caught = false;
    try { (void)denied.allocate(1, 1); } catch (const std::bad_alloc&) { caught = true; }
    CHECK(caught && denied.counters().failures == 1 && denied.counters().allocations == 0);
    CHECK(denied.counters().current_bytes == 0 && denied.counters().peak_bytes == 0);
}

struct Throws {
    static inline int destructors = 0;
    Throws() { throw std::runtime_error("constructor fixture"); }
    ~Throws() { ++destructors; }
};

void lifetime_test() {
    memory_lab::CountingResource upstream(std::pmr::new_delete_resource());
    alignas(64) std::array<std::byte, 128> buffer{};
    {
        std::pmr::monotonic_buffer_resource resource(buffer.data(), buffer.size(), &upstream);
        memory_lab::CountingResource frontend(&resource);
        void* raw = frontend.allocate(sizeof(Throws), alignof(Throws));
        bool caught = false;
        try { std::construct_at(static_cast<Throws*>(raw)); } catch (const std::runtime_error&) { caught = true; }
        CHECK(caught && Throws::destructors == 0);
        frontend.deallocate(raw, sizeof(Throws), alignof(Throws));
        CHECK(frontend.counters().current_bytes == 0);
        CHECK(upstream.counters().allocations == 0); // supplied buffer, not upstream
        resource.release(); // failed constructor created no live Throws object
    }
    CHECK(upstream.counters().current_bytes == 0);
}

void route_tests() {
    for (const std::size_t size : {std::size_t{1}, std::size_t{64}, std::size_t{2048}}) {
        const auto r = memory_lab::observe_pmr(size, UINT64_MAX);
        const auto& fallback = r.monotonic_fallback;
        CHECK(fallback.fixture_ok && !fallback.bad_alloc_observed);
        CHECK(fallback.upstream.allocations > 0);
        CHECK(fallback.upstream_before_block_free && *fallback.upstream_before_block_free > 0);
        CHECK(fallback.upstream_before_block_free == fallback.upstream_after_block_free);
        CHECK(fallback.upstream_before_release > 0 && fallback.upstream_after_release == 0);
        const auto& hard = r.monotonic_hardcap;
        CHECK(hard.fixture_ok && hard.bad_alloc_observed);
        CHECK(hard.frontend.failures == 1 && hard.upstream.failures == 1);
        CHECK(hard.upstream.allocations == 0 && hard.upstream.allocated_bytes == 0);
        CHECK(!hard.upstream_before_block_free && !hard.upstream_after_block_free);
        CHECK(hard.frontend.allocations > 0); // counter sees initial-buffer requests too
        CHECK(r.pool.fixture_ok && r.pool.upstream.allocations > 0);
        // No expected chunk count, retained bytes or per-free upstream behavior.
        for (const auto* route : {&fallback, &hard, &r.pool}) {
            CHECK(route->constructed > 0 && route->constructed == route->destroyed);
            CHECK(route->frontend.current_bytes == 0);
            CHECK(route->frontend.allocations == route->frontend.deallocations);
            CHECK(route->upstream_after_release == 0 && route->final_upstream_bytes == 0);
            CHECK(route->upstream.current_bytes == 0);
            CHECK(route->upstream.allocated_bytes == route->upstream.deallocated_bytes);
        }
        CHECK(r.copy_uses_default && r.explicit_copy_uses_destination);
        CHECK(r.move_uses_source && r.explicit_move_uses_destination && r.move_assignment_keeps_destination);
        CHECK(r.copy_move_values_ok);
        std::ostringstream json;
        memory_lab::write_pmr_json(json, r);
        CHECK(json.str().find("\"final_upstream_bytes\":0") != std::string::npos);
    }
    for (std::size_t size : {std::size_t{0}, std::size_t{2049}}) {
        bool caught = false;
        try { (void)memory_lab::observe_pmr(size, 1); } catch (const std::invalid_argument&) { caught = true; }
        CHECK(caught);
    }
}
} // namespace

int main() {
    try { counter_tests(); lifetime_test(); route_tests(); std::cout << "PMR checks passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
