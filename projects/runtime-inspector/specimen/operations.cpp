#include "specimen.hpp"

std::uint64_t specimen_global = 17;

// A stable C symbol makes the actual ABI/disassembly easy to find; this is not
// an assertion that a particular optimization level preserves a call.
extern "C" std::uint64_t specimen_transform(std::uint64_t value, std::uint64_t salt) {
    return value * 33U + salt + specimen_global;
}
