#include "specimen.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const std::uint64_t input = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 42;
    std::uint64_t automatic = input;
    auto dynamic = std::make_unique<std::uint64_t>(specimen_transform(input, 5));
    std::vector<std::uint64_t> values;
    values.reserve(2);
    values.push_back(input);
    const auto before = reinterpret_cast<std::uintptr_t>(values.data());
    const auto old_capacity = values.capacity();
    // Exceed actual capacity, not an assumed growth factor. Never dereference
    // or compare dangling pointers: the old address was captured as an integer.
    while (values.size() <= old_capacity) values.push_back(input);
    const auto after = reinterpret_cast<std::uintptr_t>(values.data());
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "{\"pid\":" << getpid()
              << ",\"pointer_bytes\":" << sizeof(void*)
              << ",\"native_page_bytes\":" << sysconf(_SC_PAGESIZE)
              << ",\"global_address\":" << reinterpret_cast<std::uintptr_t>(&specimen_global)
              << ",\"automatic_address\":" << reinterpret_cast<std::uintptr_t>(&automatic)
              << ",\"dynamic_address\":" << reinterpret_cast<std::uintptr_t>(dynamic.get())
              << ",\"result\":" << *dynamic
              << ",\"minor_faults_since_start\":" << usage.ru_minflt
              << ",\"major_faults_since_start\":" << usage.ru_majflt
              << ",\"layout\":{\"size\":" << sizeof(Layout)
              << ",\"alignment\":" << alignof(Layout)
              << ",\"tag_offset\":" << offsetof(Layout, tag)
              << ",\"value_offset\":" << offsetof(Layout, value)
              << ",\"count_offset\":" << offsetof(Layout, count) << "}"
              << ",\"vector\":{\"old_capacity\":" << old_capacity
              << ",\"new_capacity\":" << values.capacity()
              << ",\"old_address\":" << before << ",\"new_address\":" << after << "}}\n" << std::flush;
    // Only the inspector asks for a bounded observation window for process maps.
    if (argc > 2 && std::string_view(argv[2]) == "--hold")
        std::this_thread::sleep_for(std::chrono::seconds(10));
}
