#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime {
struct Config {
    std::size_t size = 200000;
    std::size_t threads = 4;
    std::uint64_t seed = 42;
    std::size_t repetitions = 5;
};
struct Sample {
    std::string variant;
    std::size_t repetition;
    double seconds;
    std::uint64_t checksum;
};
struct Result {
    Config config;
    std::uint64_t expected;
    std::vector<Sample> samples;
};
std::uint64_t value_at(std::size_t index, std::uint64_t seed);
Result run(const Config& config);
std::string to_json(const Result& result);
} // namespace runtime
