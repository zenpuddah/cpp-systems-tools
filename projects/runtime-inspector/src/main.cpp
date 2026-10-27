#include "runtime/workloads.hpp"
#include <charconv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        runtime::Config config;
        for (int i=1; i<argc; i+=2) {
            const std::string_view key(argv[i]);
            if (key == "--help") {
                std::cout << "runtime-workloads [--size 0..10000000] [--threads 1..32] [--seed uint64] [--repetitions 1..30]\n";
                return 0;
            }
            if (i+1 == argc) throw std::invalid_argument("missing option value");
            const std::string_view text(argv[i+1]);
            std::uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
            if (error != std::errc{} || end != text.data()+text.size()) throw std::invalid_argument("expected unsigned integer");
            if (key != "--seed" && value > std::numeric_limits<std::size_t>::max())
                throw std::invalid_argument("option exceeds size_t range");
            if (key == "--size") config.size = value;
            else if (key == "--threads") config.threads = value;
            else if (key == "--seed") config.seed = value;
            else if (key == "--repetitions") config.repetitions = value;
            else throw std::invalid_argument("unknown option");
        }
        std::cout << runtime::to_json(runtime::run(config));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime-workloads: " << error.what() << '\n';
        return 1;
    }
}
