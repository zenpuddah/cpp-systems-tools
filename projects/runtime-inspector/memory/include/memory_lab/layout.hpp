#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace memory_lab {

struct MemberExtent {
    std::string name;
    std::size_t offset, size, alignment;
};

struct PaddingExtent {
    std::size_t offset, size;
    bool tail;
};

struct LayoutMap {
    std::string name;
    std::size_t object_size, object_alignment, array_stride;
    std::vector<MemberExtent> members;
    std::vector<PaddingExtent> padding;
    std::vector<int> byte_owner;
    std::size_t internal_padding, tail_padding;
};

// Describes ordinary, non-overlapping standard-layout members, not base classes,
// bit-fields or [[no_unique_address]]. A C++ object array has stride sizeof(T).
// No bytes of any object (including its padding) are read by this function.
inline LayoutMap layout_map(std::string name, std::size_t size,
                            std::size_t alignment, std::size_t stride,
                            std::span<const MemberExtent> members) {
    const auto power_of_two = [](std::size_t n) { return n != 0 && (n & (n - 1)) == 0; };
    if (size == 0 || !power_of_two(alignment) || size % alignment != 0 ||
        stride != size || members.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("invalid object size/alignment/array stride");
    }
    std::size_t last_end = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        if (member.size == 0 || !power_of_two(member.alignment) ||
            member.alignment > alignment || member.offset % member.alignment != 0 ||
            member.size % member.alignment != 0 || member.offset > size ||
            member.size > size - member.offset) {
            throw std::invalid_argument("invalid member extent");
        }
        const auto end = member.offset + member.size; // Bounds checked above.
        if (end > last_end) last_end = end;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = members[j];
            if (member.offset < other.offset + other.size && other.offset < end) {
                throw std::invalid_argument("overlapping member extents");
            }
        }
    }

    LayoutMap map{std::move(name), size, alignment, stride,
                  std::vector<MemberExtent>(members.begin(), members.end()), {},
                  std::vector<int>(size, -1), 0, size - last_end};
    for (std::size_t i = last_end; i < size; ++i) map.byte_owner[i] = -2;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        for (std::size_t b = member.offset; b < member.offset + member.size; ++b) {
            map.byte_owner[b] = static_cast<int>(i);
        }
    }
    for (std::size_t b = 0; b < size;) {
        if (map.byte_owner[b] >= 0) {
            ++b;
            continue;
        }
        const auto start = b;
        const bool tail = map.byte_owner[b] == -2;
        while (b < size && map.byte_owner[b] == (tail ? -2 : -1)) ++b;
        map.padding.push_back({start, b - start, tail});
        if (!tail) map.internal_padding += b - start;
    }
    return map;
}

namespace layout_detail {
struct Original {
    char label;
    double value;
    std::uint32_t count;
};
struct Reordered {
    double value;
    std::uint32_t count;
    char label;
};
struct alignas(64) Aligned {
    char label;
    double value;
    std::uint32_t count;
};
static_assert(std::is_standard_layout_v<Original>);
static_assert(std::is_standard_layout_v<Reordered>);
static_assert(std::is_standard_layout_v<Aligned>);

template <class T>
inline std::size_t measured_stride() {
    std::array<T, 2> objects{};
    // Numeric address observations on the supported uintptr_t POSIX targets;
    // neither padding reads nor subtraction of unrelated C++ pointers.
    return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(&objects[1]) -
                                    reinterpret_cast<std::uintptr_t>(&objects[0]));
}

template <class T>
inline LayoutMap fixture(std::string name) {
    static_assert(std::is_standard_layout_v<T>);
    const std::array fields{
        MemberExtent{"label", offsetof(T, label), sizeof(T::label), alignof(decltype(T::label))},
        MemberExtent{"value", offsetof(T, value), sizeof(T::value), alignof(decltype(T::value))},
        MemberExtent{"count", offsetof(T, count), sizeof(T::count), alignof(decltype(T::count))}};
    return layout_map(std::move(name), sizeof(T), alignof(T), measured_stride<T>(), fields);
}
} // namespace layout_detail

inline std::vector<LayoutMap> fixture_layouts() {
    return {layout_detail::fixture<layout_detail::Original>("original"),
            layout_detail::fixture<layout_detail::Reordered>("reordered"),
            layout_detail::fixture<layout_detail::Aligned>("alignas(64)")};
}

} // namespace memory_lab
