#include "runtime/bounded_queue.hpp"
#include "runtime/workloads.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

// Deliberately not assert(): release and sanitizer configurations run the same
// behavioral checks. CTest supplies the final process deadline for deadlocks.
#define CHECK(x) do { if (!(x)) throw std::runtime_error("check failed: " #x); } while(false)
using Queue = runtime::BoundedQueue<unsigned>;
using namespace std::chrono_literals;
struct CloseOnExit { Queue& queue; ~CloseOnExit() { queue.close(); } };
template<class F> void await(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("waiter deadline exceeded");
        std::this_thread::yield();
    }
}

void core() {
    CHECK(runtime::value_at(0, 0) == 0);
    CHECK(runtime::value_at(1, 0) == UINT64_C(86980551294885));
    CHECK(runtime::value_at(0, 1) == UINT64_C(86980551294885));
    // Literal independently calculated fixtures; Python additionally checks the
    // complete runtime-index/seed formula for several nontrivial cases.
    for (const auto size : {0U, 1U, 17U}) {
        auto result = runtime::run({size, 3, 0, 1});
        CHECK(result.samples.size() == 6);
        for (const auto& sample : result.samples) {
            CHECK(sample.checksum == result.expected);
            CHECK(sample.seconds >= 0);
        }
    }
    const auto zero = runtime::to_json({{0, 1, 0, 1}, 0, {{"aos", 0, 0.0, 0}}});
    CHECK(zero.find("\"items_per_second\":null") != std::string::npos);
    CHECK(zero.find("below_clock_resolution") != std::string::npos);
    bool rejected = false;
    try { (void)runtime::run({1, 0, 1, 1}); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

void queue_empty() {
    bool rejected = false;
    try { Queue invalid(0); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    Queue queue(2);
    std::atomic<unsigned> ended{0};
    std::vector<std::jthread> consumers;
    CloseOnExit cleanup{queue}; // closes before jthread destructors on failure
    for (unsigned i=0; i<3; ++i) consumers.emplace_back([&] {
        if (!queue.pop()) ended.fetch_add(1);
    });
    await([&] { return queue.state().waiting_pop == 3; });
    queue.close();
    queue.close();
    await([&] { return ended.load() == 3; });
    for (auto& worker : consumers) worker.join();
    CHECK(!queue.pop());
    CHECK(!queue.push(std::make_unique<unsigned>(9)));
    CHECK(queue.state().high_water == 0);
}

void queue_full() {
    Queue queue(1);
    CHECK(queue.push(std::make_unique<unsigned>(7)));
    std::atomic<unsigned> rejected{0};
    std::vector<std::jthread> producers;
    CloseOnExit cleanup{queue};
    for (unsigned i=0; i<3; ++i) producers.emplace_back([&] {
        if (!queue.push(std::make_unique<unsigned>(8))) rejected.fetch_add(1);
    });
    await([&] { return queue.state().waiting_push == 3; });
    CHECK(queue.state().size == 1);
    queue.close();
    await([&] { return rejected.load() == 3; });
    for (auto& worker : producers) worker.join();
    auto item = queue.pop();
    CHECK(item && *item == 7);
    CHECK(!queue.pop());
    CHECK(queue.state().high_water == 1);
}

void queue_ownership() {
    struct Item {
        std::atomic<unsigned>& alive;
        explicit Item(std::atomic<unsigned>& count) : alive(count) { ++alive; }
        ~Item() { --alive; }
    };
    std::atomic<unsigned> alive{0};
    runtime::BoundedQueue<Item> queue(1);
    CHECK(queue.push(std::make_unique<Item>(alive)));
    CHECK(alive == 1);
    queue.close();
    CHECK(!queue.push(std::make_unique<Item>(alive)));
    CHECK(alive == 1); // rejected item was destroyed, accepted item remains owned
    auto item = queue.pop();
    CHECK(item && alive == 1);
    item.reset();
    CHECK(alive == 0);
    CHECK(!queue.pop());
    bool rejected_null = false;
    try { queue.push({}); } catch (const std::invalid_argument&) { rejected_null = true; }
    CHECK(rejected_null);
}

void queue_uneven(std::size_t capacity, unsigned producer_count, unsigned consumer_count, unsigned count) {
    Queue queue(capacity);
    std::vector<std::atomic<unsigned>> seen(count);
    for (auto& item : seen) item.store(0);
    std::atomic<unsigned> accepted{0}, delivered{0}, invalid{0}, finished{0};
    std::vector<std::jthread> consumers, producers;
    CloseOnExit cleanup{queue};
    for (unsigned t=0; t<consumer_count; ++t) consumers.emplace_back([&] {
        while (auto item = queue.pop()) {
            if (*item >= count) invalid.fetch_add(1);
            else seen[*item].fetch_add(1);
            delivered.fetch_add(1);
        }
        finished.fetch_add(1);
    });
    for (unsigned t=0; t<producer_count; ++t) producers.emplace_back([&, t] {
        for (unsigned i=t; i<count; i+=producer_count)
            if (queue.push(std::make_unique<unsigned>(i))) accepted.fetch_add(1);
    });
    for (auto& worker : producers) worker.join();
    queue.close();
    await([&] { return finished.load() == consumer_count; });
    for (auto& worker : consumers) worker.join();
    CHECK(accepted == count);
    CHECK(delivered == count);
    CHECK(invalid == 0);
    for (const auto& item : seen) CHECK(item == 1);
    CHECK(queue.state().size == 0);
    CHECK(queue.state().high_water <= capacity);
    CHECK(!queue.push(std::make_unique<unsigned>(0)));
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("choose core or queue");
        if (std::string_view(argv[1]) == "core") core();
        else if (std::string_view(argv[1]) == "queue") {
            queue_empty(); queue_full(); queue_ownership();
            queue_uneven(1, 5, 2, 1003);
            queue_uneven(7, 2, 5, 997);
            queue_uneven(3, 3, 4, 0);
        } else throw std::invalid_argument("unknown test suite");
        std::cout << "passed: " << argv[1] << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
