#pragma once
#include <cstddef>
#include <cstdint>

extern std::uint64_t specimen_global;
extern "C" std::uint64_t specimen_transform(std::uint64_t value, std::uint64_t salt);

struct Layout {
    char tag;
    std::uint64_t value;
    std::uint32_t count;
};
