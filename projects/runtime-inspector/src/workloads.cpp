#include "runtime/workloads.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace runtime {
namespace {
using Clock = std::chrono::steady_clock;
struct Record { std::uint64_t hot; std::array<std::uint64_t, 7> cold; };
struct Adjacent { std::atomic<std::uint64_t> sum{0}; };
// 128-byte spacing is a stated experiment parameter, NOT a portable guarantee
// of cache-line size or proof that a measured difference is false sharing.
struct alignas(128) Spaced { std::atomic<std::uint64_t> sum{0}; };

template<class F> Sample timed(std::string name, std::size_t repetition, F&& work) {
    const auto start = Clock::now();
    const auto checksum = work();
    const auto stop = Clock::now();
    return {std::move(name), repetition, std::chrono::duration<double>(stop-start).count(), checksum};
}

Sample aggregation(const std::vector<std::uint64_t>& data, std::size_t threads,
                   const std::string& variant, std::size_t repetition) {
    std::atomic<std::uint64_t> shared{0};
    auto adjacent = std::make_unique<Adjacent[]>(threads);
    auto spaced = std::make_unique<Spaced[]>(threads);
    std::barrier ready(static_cast<std::ptrdiff_t>(threads + 1));
    std::barrier start(static_cast<std::ptrdiff_t>(threads + 1));
    std::barrier finish(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<std::thread> workers;
    workers.reserve(threads);
    // All allocation/thread startup precedes the ready gate. Dispatch is once
    // per thread, outside the hot loop. Each mode covers identical strided data.
    // If startup fails, release the already-created participants before joining.
    try {
        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                ready.arrive_and_wait();
                start.arrive_and_wait();
                if (variant == "shared") {
                    for (std::size_t i=t; i<data.size(); i+=threads)
                        shared.fetch_add(data[i], std::memory_order_relaxed);
                } else if (variant == "adjacent") {
                    for (std::size_t i=t; i<data.size(); i+=threads)
                        adjacent[t].sum.fetch_add(data[i], std::memory_order_relaxed);
                } else if (variant == "spaced") {
                    for (std::size_t i=t; i<data.size(); i+=threads)
                        spaced[t].sum.fetch_add(data[i], std::memory_order_relaxed);
                } else {
                    std::uint64_t sum = 0;
                    for (std::size_t i=t; i<data.size(); i+=threads) sum += data[i];
                    // One explicit release/acquire publication per worker.
                    // Keep every hot-loop addition private and make result
                    // synchronization visible independently of barrier internals.
                    spaced[t].sum.store(sum, std::memory_order_release);
                }
                finish.arrive_and_wait();
            });
        }
    } catch (...) {
        for (std::size_t t=workers.size(); t<threads; ++t) {
            ready.arrive_and_drop(); start.arrive_and_drop(); finish.arrive_and_drop();
        }
        ready.arrive_and_wait(); start.arrive_and_wait(); finish.arrive_and_wait();
        for (auto& worker : workers) worker.join();
        throw;
    }
    ready.arrive_and_wait();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    finish.arrive_and_wait();
    // Include final reduction for all partitioned variants, but not joins.
    std::uint64_t sum = shared.load(std::memory_order_relaxed);
    for (std::size_t t=0; t<threads; ++t) {
        if (variant == "adjacent") sum += adjacent[t].sum.load(std::memory_order_relaxed);
        else if (variant == "spaced") sum += spaced[t].sum.load(std::memory_order_relaxed);
        else if (variant == "local") sum += spaced[t].sum.load(std::memory_order_acquire);
    }
    const auto end = Clock::now();
    for (auto& worker : workers) worker.join();
    return {variant, repetition, std::chrono::duration<double>(end-begin).count(), sum};
}
} // namespace

std::uint64_t value_at(std::size_t index, std::uint64_t seed) {
    return ((static_cast<std::uint64_t>(index) ^ seed) * UINT64_C(0x9e3779b97f4a7c15) + seed) >> 17U;
}

Result run(const Config& config) {
    if (config.size > 10000000 || config.threads == 0 || config.threads > 32 ||
        config.repetitions == 0 || config.repetitions > 30)
        throw std::invalid_argument("limits: size 0..10000000, threads 1..32, repetitions 1..30");
    std::vector<Record> aos(config.size);
    std::vector<std::uint64_t> soa(config.size);
    std::uint64_t expected = 0;
    for (std::size_t i=0; i<config.size; ++i) {
        soa[i] = value_at(i, config.seed);
        aos[i] = {soa[i], {1,2,3,4,5,6,7}};
        expected += soa[i];
    }
    Result result{config, expected, {}};
    auto check = [&](Sample sample, bool record) {
        if (sample.checksum != expected) throw std::runtime_error("checksum mismatch: " + sample.variant);
        if (sample.seconds < 0) throw std::runtime_error("negative elapsed duration");
        if (record) result.samples.push_back(std::move(sample));
    };
    auto aos_work = [&] { std::uint64_t sum=0; for (const auto& row : aos) sum += row.hot; return sum; };
    auto soa_work = [&] { std::uint64_t sum=0; for (auto value : soa) sum += value; return sum; };
    // One checked untimed-to-output warm-up per variant. Rotate measured order
    // each repetition to reduce (not eliminate) systematic order effects.
    const std::array<std::string, 6> modes{"aos", "soa", "shared", "adjacent", "spaced", "local"};
    auto sample = [&](const std::string& mode, std::size_t r) {
        if (mode == "aos") return timed(mode, r, aos_work);
        if (mode == "soa") return timed(mode, r, soa_work);
        return aggregation(soa, config.threads, mode, r);
    };
    for (const auto& mode : modes) check(sample(mode, 0), false);
    for (std::size_t r=0; r<config.repetitions; ++r)
        for (std::size_t j=0; j<modes.size(); ++j)
            check(sample(modes[(r+j)%modes.size()], r), true);
    return result;
}

std::string to_json(const Result& result) {
    std::ostringstream out;
    out << std::setprecision(17) << "{\"schema\":1,\"size\":" << result.config.size
        << ",\"threads\":" << result.config.threads << ",\"seed\":" << result.config.seed
        << ",\"repetitions\":" << result.config.repetitions << ",\"expected\":" << result.expected
        << ",\"clock\":\"steady_clock\",\"arithmetic\":\"uint64 modulo 2^64\","
        << "\"warmup_per_variant\":1,\"spacing_bytes\":128,\"atomic_uint64_lock_free\":"
        << (std::atomic<std::uint64_t>::is_always_lock_free ? "true" : "false")
        << ",\"timing\":\"AoS/SoA: traversal only; aggregation: release gate through finish gate and reduction; no allocation, thread creation or joins\","
        << "\"errors\":[],\"samples\":[";
    bool first = true;
    for (const auto& s : result.samples) {
        if (!first) out << ',';
        first = false;
        out << "{\"variant\":\"" << s.variant << "\",\"repetition\":" << s.repetition
            << ",\"seconds\":" << s.seconds << ",\"checksum\":" << s.checksum
            << ",\"timing_status\":\"" << (s.seconds == 0 ? "below_clock_resolution" : "measured")
            << "\",\"items_per_second\":";
        if (s.seconds == 0) out << "null";
        else out << static_cast<double>(result.config.size)/s.seconds;
        out << '}';
    }
    out << "]}\n";
    return out.str();
}
} // namespace runtime
