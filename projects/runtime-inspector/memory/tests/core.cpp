#include "memory_lab/arena.hpp"
#include "memory_lab/layout.hpp"
#include "memory_lab/pool.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

[[noreturn]] void failure(const char* expression, int line) {
    throw std::runtime_error("core.cpp:" + std::to_string(line) + ": CHECK(" + expression + ")");
}
#define CHECK(...) do { if (!(__VA_ARGS__)) failure(#__VA_ARGS__, __LINE__); } while (false)

using memory_lab::BumpArena;
using memory_lab::FixedPool;
using memory_lab::LayoutMap;
using memory_lab::MemberExtent;
using memory_lab::SlotState;

static_assert(!std::is_copy_constructible_v<BumpArena> && !std::is_move_constructible_v<BumpArena>);
static_assert(!std::is_copy_assignable_v<BumpArena> && !std::is_move_assignable_v<BumpArena>);
static_assert(!std::is_copy_constructible_v<FixedPool> && !std::is_move_constructible_v<FixedPool>);
static_assert(!std::is_copy_assignable_v<FixedPool> && !std::is_move_assignable_v<FixedPool>);
static_assert(BumpArena::max_alignment == 256 && FixedPool::max_alignment == 256);

std::uintptr_t address(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }

// Independent fixture definitions: expected extents come from this compiler,
// not a universal ABI constant or the layout implementation's helper.
struct Original { char label; double value; std::uint32_t count; };
struct Reordered { double value; std::uint32_t count; char label; };
struct alignas(64) Aligned { char label; double value; std::uint32_t count; };
static_assert(std::is_standard_layout_v<Original> && std::is_standard_layout_v<Reordered> &&
              std::is_standard_layout_v<Aligned>);

template <class T>
void check_layout(const LayoutMap& map) {
    const std::array expected{
        MemberExtent{"label", offsetof(T, label), sizeof(T::label), alignof(decltype(T::label))},
        MemberExtent{"value", offsetof(T, value), sizeof(T::value), alignof(decltype(T::value))},
        MemberExtent{"count", offsetof(T, count), sizeof(T::count), alignof(decltype(T::count))}};
    std::array<T, 2> objects{};
    CHECK(map.object_size == sizeof(T));
    CHECK(map.object_alignment == alignof(T));
    CHECK(map.array_stride == address(&objects[1]) - address(&objects[0]));
    CHECK(map.array_stride == sizeof(T));
    CHECK(address(&objects[0]) % alignof(T) == 0);
    CHECK(address(&objects[1]) % alignof(T) == 0);
    CHECK(map.members.size() == expected.size());
    std::size_t last_end = 0, member_bytes = 0;
    for (const auto& member : expected) {
        last_end = std::max(last_end, member.offset + member.size);
        member_bytes += member.size;
        const auto found = std::find_if(map.members.begin(), map.members.end(), [&](const auto& actual) {
            return actual.name == member.name;
        });
        CHECK(found != map.members.end());
        CHECK(found->offset == member.offset && found->size == member.size &&
              found->alignment == member.alignment);
    }
    CHECK(map.byte_owner.size() == sizeof(T));
    std::size_t internal = 0, tail = 0;
    for (std::size_t b = 0; b < sizeof(T); ++b) {
        int owner = b >= last_end ? -2 : -1;
        for (std::size_t i = 0; i < map.members.size(); ++i) {
            const auto& member = map.members[i];
            if (b >= member.offset && b - member.offset < member.size) owner = static_cast<int>(i);
        }
        CHECK(map.byte_owner[b] == owner);
        if (owner == -1) ++internal;
        if (owner == -2) ++tail;
    }
    std::vector<unsigned> coverage(sizeof(T), 0);
    for (const auto& padding : map.padding) {
        CHECK(padding.size > 0 && padding.offset <= sizeof(T));
        CHECK(padding.size <= sizeof(T) - padding.offset);
        for (std::size_t b = padding.offset; b < padding.offset + padding.size; ++b) {
            CHECK(map.byte_owner[b] == (padding.tail ? -2 : -1));
            CHECK(++coverage[b] == 1);
        }
    }
    for (std::size_t b = 0; b < sizeof(T); ++b) CHECK(coverage[b] == (map.byte_owner[b] < 0 ? 1U : 0U));
    CHECK(internal == map.internal_padding && tail == map.tail_padding);
    CHECK(member_bytes + internal + tail == sizeof(T));
    std::cout << map.name << " size=" << map.object_size << " align=" << map.object_alignment
              << " stride=" << map.array_stride << " internal=" << internal << " tail=" << tail << '\n';
    for (const auto& padding : map.padding) {
        std::cout << "  " << (padding.tail ? "tail" : "internal") << " offset=" << padding.offset
                  << " bytes=" << padding.size << '\n';
    }
}

void layout_tests() {
    auto layouts = memory_lab::fixture_layouts();
    CHECK(layouts.size() == 3);
    check_layout<Original>(layouts[0]);
    check_layout<Reordered>(layouts[1]);
    check_layout<Aligned>(layouts[2]);
    CHECK(layouts[2].object_alignment >= 64);
    for (const auto& field : layouts[0].members) {
        const auto found = std::find_if(layouts[1].members.begin(), layouts[1].members.end(),
                                        [&](const auto& member) { return member.name == field.name; });
        CHECK(found != layouts[1].members.end());
        CHECK(found->size == field.size && found->alignment == field.alignment);
    }
    // These objects carry identical field values, without comparing padding.
    const Original original{'x', 2.5, 17};
    const Reordered reordered{original.value, original.count, original.label};
    CHECK(original.label == reordered.label && original.value == reordered.value &&
          original.count == reordered.count);

    const auto rejects = [](std::size_t size, std::size_t alignment, std::size_t stride,
                            std::initializer_list<MemberExtent> fields) {
        bool rejected = false;
        try { (void)memory_lab::layout_map("invalid", size, alignment, stride, fields); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    };
    rejects(0, 1, 0, {});
    rejects(8, 0, 8, {});
    rejects(8, 3, 8, {});
    rejects(7, 4, 7, {});
    rejects(8, 4, 16, {});
    rejects(8, 4, 8, {{"zero", 0, 0, 1}});
    rejects(8, 4, 8, {{"zero-align", 0, 1, 0}});
    rejects(8, 4, 8, {{"bad-align", 0, 3, 3}});
    rejects(8, 4, 8, {{"over-align", 0, 8, 8}});
    rejects(8, 4, 8, {{"misaligned", 1, 4, 4}});
    rejects(8, 4, 8, {{"bad-size", 0, 3, 2}});
    rejects(8, 4, 8, {{"outside", 9, 1, 1}});
    rejects(8, 4, 8, {{"crosses", 7, 2, 1}});
    rejects(8, 4, 8, {{"overflow-offset", std::numeric_limits<std::size_t>::max(), 2, 1}});
    rejects(8, 4, 8, {{"overflow-size", 1, std::numeric_limits<std::size_t>::max(), 1}});
    rejects(8, 4, 8, {{"first", 0, 4, 4}, {"overlap", 2, 2, 2}});
    rejects(8, 4, 8, {{"same", 0, 4, 4}, {"same", 0, 4, 4}});
    const std::array unsorted{MemberExtent{"last", 3, 1, 1}, MemberExtent{"first", 0, 1, 1}};
    auto custom = memory_lab::layout_map(std::string("owned name"), 8, 4, 8, unsorted);
    CHECK(custom.name == "owned name");
    CHECK(custom.byte_owner == std::vector<int>({1, -1, -1, 0, -2, -2, -2, -2}));
    CHECK(custom.internal_padding == 2 && custom.tail_padding == 4);
    const auto empty = memory_lab::layout_map("no member extents", 1, 1, 1, {});
    CHECK(empty.byte_owner == std::vector<int>({-2}) && empty.tail_padding == 1);
}

std::array<std::size_t, 5> arena_state(const BumpArena& arena) {
    return {arena.capacity(), arena.consumed(), arena.remaining(), arena.payload_bytes(), arena.padding_bytes()};
}

void arena_reject(BumpArena& arena, std::size_t bytes, std::size_t alignment) {
    const auto before = arena_state(arena);
    CHECK(arena.allocate(bytes, alignment) == nullptr);
    CHECK(arena_state(arena) == before);
}

template <class T> struct Destroy {
    void operator()(T* object) const noexcept { std::destroy_at(object); }
};
template <class T> using Object = std::unique_ptr<T, Destroy<T>>;

struct alignas(256) OverAligned {
    std::uint64_t value;
};

struct Counted {
    static inline int attempts = 0, constructed = 0, destroyed = 0, live = 0;
    int value;
    explicit Counted(int input, bool fail = false, BumpArena* nested_arena = nullptr,
                     Counted** nested_result = nullptr) : value(input) {
        ++attempts;
        if (nested_arena != nullptr) {
            void* storage = nested_arena->allocate(sizeof(Counted), alignof(Counted));
            if (storage == nullptr) throw std::bad_alloc();
            *nested_result = std::construct_at(static_cast<Counted*>(storage), input + 1);
        }
        if (fail) throw std::runtime_error("requested constructor failure");
        ++constructed;
        ++live;
    }
    ~Counted() { ++destroyed; --live; }
    static void clear_counts() {
        CHECK(live == 0);
        attempts = constructed = destroyed = 0;
    }
};

void arena_lifetimes() {
    Counted::clear_counts();
    alignas(256) std::array<std::byte, 1024> backing{};
    BumpArena arena(backing);
    {
        void* storage = arena.allocate(sizeof(Counted), alignof(Counted));
        CHECK(storage != nullptr);
        Object<Counted> object(std::construct_at(static_cast<Counted*>(storage), 42));
        CHECK(object->value == 42 && Counted::live == 1);
        const auto state = arena_state(arena);
        arena_reject(arena, std::numeric_limits<std::size_t>::max(), 1);
        CHECK(arena_state(arena) == state && object->value == 42);
    }
    CHECK(Counted::constructed == 1 && Counted::destroyed == 1 && Counted::live == 0);

    void* throwing_storage = arena.allocate(sizeof(Counted), alignof(Counted));
    CHECK(throwing_storage != nullptr);
    const auto reserved = arena_state(arena);
    bool threw = false;
    try { (void)std::construct_at(static_cast<Counted*>(throwing_storage), 7, true); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && arena_state(arena) == reserved);
    CHECK(Counted::attempts == 2 && Counted::constructed == 1 && Counted::destroyed == 1);

    void* outer_storage = arena.allocate(sizeof(Counted), alignof(Counted));
    CHECK(outer_storage != nullptr);
    const auto before_nested = arena.consumed();
    Counted* nested_raw = nullptr;
    threw = false;
    try {
        (void)std::construct_at(static_cast<Counted*>(outer_storage), 88, true, &arena, &nested_raw);
    } catch (const std::runtime_error&) { threw = true; }
    Object<Counted> nested(nested_raw);
    CHECK(threw && nested != nullptr && nested->value == 89);
    CHECK(arena.consumed() >= before_nested + sizeof(Counted));
    CHECK(Counted::attempts == 4 && Counted::constructed == 2 && Counted::live == 1);
    // A subsequent allocation cannot overwrite the nested object: no rewind on
    // the outer constructor's failure. The raw outer reservation stays consumed.
    auto* subsequent = static_cast<std::byte*>(arena.allocate(sizeof(Counted), alignof(Counted)));
    CHECK(subsequent != nullptr);
    CHECK(address(subsequent) >= address(nested.get()) + sizeof(Counted));
    std::fill_n(subsequent, sizeof(Counted), std::byte{0x55});
    CHECK(nested->value == 89);
    nested.reset();
    CHECK(Counted::live == 0 && Counted::constructed == Counted::destroyed);
    const auto destroyed = Counted::destroyed;
    arena.reset(); // No live objects or subsequently used old handles.
    CHECK(Counted::destroyed == destroyed && arena.consumed() == 0);
    {
        void* fresh_storage = arena.allocate(sizeof(Counted), alignof(Counted));
        CHECK(fresh_storage != nullptr);
        Object<Counted> fresh(std::construct_at(static_cast<Counted*>(fresh_storage), 101));
        CHECK(fresh->value == 101);
    }
    CHECK(Counted::constructed == 3 && Counted::destroyed == 3 && Counted::live == 0);
}

void arena_tests() {
    BumpArena empty(std::span<std::byte>{});
    CHECK(arena_state(empty) == (std::array<std::size_t, 5>{0, 0, 0, 0, 0}));
    arena_reject(empty, 1, 1);
    empty.reset();

    alignas(256) std::array<std::byte, 1024> backing{};
    BumpArena exact(std::span(backing).first(17));
    arena_reject(exact, 18, 1); // One byte over a fresh view.
    CHECK(exact.allocate(17, 1) == backing.data());
    CHECK(arena_state(exact) == (std::array<std::size_t, 5>{17, 17, 0, 17, 0}));
    arena_reject(exact, 1, 1);
    exact.reset();
    CHECK(exact.allocate(16, 1) != nullptr);
    arena_reject(exact, 2, 1); // One byte over remaining capacity.
    CHECK(exact.allocate(1, 1) != nullptr);

    BumpArena unaligned(std::span(backing).subspan(1, 319));
    arena_reject(unaligned, 65, 256); // Payload fits alone, padding makes it fail.
    auto* aligned = unaligned.allocate(64, 256);
    CHECK(aligned == backing.data() + 256 && address(aligned) % 256 == 0);
    CHECK(arena_state(unaligned) == (std::array<std::size_t, 5>{319, 319, 0, 64, 255}));
    arena_reject(unaligned, 1, 1);

    BumpArena object_arena(std::span(backing).subspan(1));
    void* object_storage = object_arena.allocate(sizeof(OverAligned), alignof(OverAligned));
    CHECK(object_storage != nullptr && address(object_storage) % alignof(OverAligned) == 0);
    {
        Object<OverAligned> object(std::construct_at(static_cast<OverAligned*>(object_storage), 1234U));
        CHECK(object->value == 1234U);
    }
    object_arena.reset();

    BumpArena invalid(backing);
    CHECK(invalid.allocate(3, 1) != nullptr);
    for (const auto alignment : {std::size_t{0}, std::size_t{3}, std::size_t{257}, std::size_t{512},
                                 std::numeric_limits<std::size_t>::max()}) {
        arena_reject(invalid, 1, alignment);
    }
    arena_reject(invalid, 0, 1);
    arena_reject(invalid, 0, 256);
    arena_reject(invalid, std::numeric_limits<std::size_t>::max(), 1);
    arena_reject(invalid, std::numeric_limits<std::size_t>::max(), 256);
    arena_reject(invalid, std::numeric_limits<std::size_t>::max() - 127, 256);

    alignas(256) std::array<std::byte, 65536> trace_backing{};
    const auto view = std::span(trace_backing).subspan(1);
    BumpArena trace(view);
    struct Block { std::byte* pointer; std::size_t size; std::byte marker; };
    std::vector<Block> blocks;
    std::size_t consumed = 0, payload = 0, padding = 0;
    std::uint32_t random = 0x1a2b3c4dU;
    for (std::size_t i = 0; i < 256; ++i) {
        random = random * 1664525U + 1013904223U;
        const auto bytes = std::size_t{1} + random % 47U;
        const auto alignment = std::size_t{1} << (i % 9);
        const auto remainder = (address(view.data()) + consumed) % alignment;
        const auto gap = remainder == 0 ? 0 : alignment - remainder;
        CHECK(bytes + gap <= view.size() - consumed);
        auto* block = static_cast<std::byte*>(trace.allocate(bytes, alignment));
        CHECK(block != nullptr && address(block) % alignment == 0);
        CHECK(address(block) == address(view.data()) + consumed + gap);
        for (const auto& previous : blocks) {
            CHECK(address(block) >= address(previous.pointer) + previous.size);
        }
        const auto marker = static_cast<std::byte>(i % 251 + 1);
        std::fill_n(block, bytes, marker);
        blocks.push_back({block, bytes, marker});
        consumed += gap + bytes;
        padding += gap;
        payload += bytes;
        CHECK(arena_state(trace) == (std::array<std::size_t, 5>{view.size(), consumed,
                                                             view.size() - consumed, payload, padding}));
        arena_reject(trace, std::numeric_limits<std::size_t>::max(), alignment);
    }
    for (const auto& block : blocks) {
        CHECK(std::all_of(block.pointer, block.pointer + block.size,
                          [&](std::byte value) { return value == block.marker; }));
    }
    // No handles from this generation survive reset, even in the test trace.
    blocks.clear();
    trace.reset();
    CHECK(arena_state(trace) == (std::array<std::size_t, 5>{view.size(), 0, view.size(), 0, 0}));
    CHECK(trace.allocate(1, 1) == view.data());
    arena_lifetimes();
}

std::array<std::size_t, 13> pool_state(const FixedPool& pool) {
    return {static_cast<std::size_t>(pool.valid()), pool.payload_size(), pool.alignment(), pool.slot_stride(),
            pool.capacity(), pool.live_count(), pool.free_count(), pool.alignment_padding(), pool.tail_bytes(),
            pool.metadata_reserved_bytes(), pool.metadata_used_bytes(), pool.backing_capacity(),
            pool.live_count() * pool.payload_size()};
}

void same_metadata(std::span<const SlotState> actual, std::span<const SlotState> expected) {
    CHECK(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i].next == expected[i].next && actual[i].allocated == expected[i].allocated);
    }
}

void pool_reject(FixedPool& pool, std::span<const SlotState> metadata, void* pointer) {
    const auto state = pool_state(pool);
    const std::vector<SlotState> saved(metadata.begin(), metadata.end());
    CHECK(!pool.deallocate(pointer));
    CHECK(pool_state(pool) == state);
    same_metadata(metadata, saved);
}

void check_accounting(const FixedPool& pool, std::size_t metadata_size) {
    CHECK(pool.valid());
    CHECK(pool.live_count() + pool.free_count() == pool.capacity());
    CHECK(pool.slot_stride() >= pool.payload_size());
    CHECK(pool.slot_stride() % pool.alignment() == 0);
    CHECK(pool.alignment_padding() + pool.capacity() * pool.slot_stride() + pool.tail_bytes() ==
          pool.backing_capacity());
    CHECK(pool.metadata_used_bytes() == pool.capacity() * sizeof(SlotState));
    CHECK(pool.metadata_reserved_bytes() == metadata_size * sizeof(SlotState));
    CHECK(pool.metadata_used_bytes() <= pool.metadata_reserved_bytes());
}

void pool_configuration_tests() {
    alignas(256) std::array<std::byte, 1024> backing{};
    std::array<SlotState, 16> metadata{};
    for (std::size_t i = 0; i < metadata.size(); ++i) metadata[i] = {100 + i, true};
    const auto untouched = metadata;
    const auto invalid = [&](std::size_t payload, std::size_t alignment) {
        FixedPool pool(backing, metadata, payload, alignment);
        CHECK(!pool.valid());
        same_metadata(metadata, untouched);
        const auto state = pool_state(pool);
        CHECK(pool.allocate() == nullptr && pool_state(pool) == state);
        pool_reject(pool, metadata, backing.data());
        same_metadata(metadata, untouched);
    };
    invalid(0, 1);
    for (const auto alignment : {std::size_t{0}, std::size_t{3}, std::size_t{257}, std::size_t{512},
                                 std::numeric_limits<std::size_t>::max()}) invalid(1, alignment);
    invalid(std::numeric_limits<std::size_t>::max(), 2);
    invalid(std::numeric_limits<std::size_t>::max() - 126, 256);
    {
        FixedPool huge(backing, metadata, std::numeric_limits<std::size_t>::max(), 1);
        check_accounting(huge, metadata.size());
        CHECK(huge.capacity() == 0 && huge.allocate() == nullptr);
        same_metadata(metadata, untouched);
    }
    {
        FixedPool no_storage({}, metadata, 3, 4);
        check_accounting(no_storage, metadata.size());
        CHECK(no_storage.capacity() == 0 && no_storage.allocate() == nullptr);
        pool_reject(no_storage, metadata, backing.data());
        same_metadata(metadata, untouched);
    }
    {
        FixedPool no_metadata(backing, {}, 3, 4);
        check_accounting(no_metadata, 0);
        CHECK(no_metadata.capacity() == 0 && no_metadata.tail_bytes() == backing.size());
        CHECK(no_metadata.allocate() == nullptr);
    }
    {
        FixedPool both_empty({}, {}, 1, 1);
        check_accounting(both_empty, 0);
        CHECK(both_empty.capacity() == 0 && both_empty.allocate() == nullptr);
    }
    {
        FixedPool too_short(std::span(backing).subspan(1, 1), metadata, 1, 256);
        check_accounting(too_short, metadata.size());
        CHECK(too_short.capacity() == 0 && too_short.alignment_padding() == 1 && too_short.tail_bytes() == 0);
        CHECK(too_short.allocate() == nullptr);
        same_metadata(metadata, untouched);
    }
    {
        FixedPool exact(std::span(backing).first(64), metadata, 17, 16);
        check_accounting(exact, metadata.size());
        CHECK(exact.slot_stride() == 32 && exact.capacity() == 2 && exact.tail_bytes() == 0);
        CHECK(exact.allocate() != nullptr && exact.allocate() != nullptr && exact.allocate() == nullptr);
        // Unused metadata is never written by the pool.
        same_metadata(std::span(metadata).subspan(2), std::span(untouched).subspan(2));
    }
    {
        FixedPool one_byte_short(std::span(backing).first(63), metadata, 17, 16);
        check_accounting(one_byte_short, metadata.size());
        CHECK(one_byte_short.capacity() == 1 && one_byte_short.tail_bytes() == 31);
        CHECK(one_byte_short.allocate() != nullptr && one_byte_short.allocate() == nullptr);
    }
    {
        FixedPool one_byte_extra(std::span(backing).first(65), metadata, 17, 16);
        check_accounting(one_byte_extra, metadata.size());
        CHECK(one_byte_extra.capacity() == 2 && one_byte_extra.tail_bytes() == 1);
    }
    // Exercise all supported alignments, including maximum, from an unaligned
    // view; metadata, rather than bytes, limits this pool's capacity.
    for (std::size_t alignment = 1; alignment <= 256; alignment *= 2) {
        std::array<SlotState, 2> slots{};
        FixedPool pool(std::span(backing).subspan(1), slots, 1, alignment);
        check_accounting(pool, slots.size());
        CHECK(pool.capacity() == slots.size());
        void* first = pool.allocate();
        void* second = pool.allocate();
        CHECK(first != nullptr && second != nullptr && first != second);
        CHECK(address(first) % alignment == 0 && address(second) % alignment == 0);
        CHECK(address(second) - address(first) == pool.slot_stride());
        CHECK(pool.deallocate(second) && pool.deallocate(first));
    }
}

void pool_trace_tests() {
    alignas(256) std::array<std::byte, 4096> backing{};
    std::array<SlotState, 19> metadata{};
    const auto view = std::span(backing).subspan(1);
    FixedPool pool(view, metadata, 19, 16);
    check_accounting(pool, metadata.size());
    CHECK(pool.capacity() == metadata.size() && pool.slot_stride() == 32);
    CHECK(pool.alignment_padding() == 15);
    std::vector<void*> slots;
    for (std::size_t i = 0; i < pool.capacity(); ++i) {
        void* slot = pool.allocate();
        CHECK(slot != nullptr && address(slot) % 16 == 0);
        CHECK(address(slot) == address(view.data()) + pool.alignment_padding() + i * pool.slot_stride());
        CHECK(address(slot) + pool.payload_size() <= address(view.data()) + view.size());
        for (void* other : slots) {
            CHECK(slot != other);
            CHECK(address(slot) >= address(other) + pool.slot_stride());
        }
        std::fill_n(static_cast<std::byte*>(slot), pool.payload_size(), static_cast<std::byte>(i + 1));
        slots.push_back(slot);
        CHECK(pool.live_count() == i + 1);
        check_accounting(pool, metadata.size());
    }
    const auto exhausted = pool_state(pool);
    const auto exhausted_metadata = metadata;
    CHECK(pool.allocate() == nullptr && pool_state(pool) == exhausted);
    same_metadata(metadata, exhausted_metadata);
    std::byte foreign{};
    alignas(256) std::array<std::byte, 512> other_backing{};
    std::array<SlotState, 2> other_metadata{};
    FixedPool other_pool(other_backing, other_metadata, 19, 16);
    void* other_slot = other_pool.allocate();
    CHECK(other_slot != nullptr);
    for (void* bad : {static_cast<void*>(nullptr), static_cast<void*>(&foreign), other_slot,
                      static_cast<void*>(backing.data()), static_cast<void*>(view.data()),
                      static_cast<void*>(backing.data() + backing.size()),
                      static_cast<void*>(static_cast<std::byte*>(slots[0]) + 1),
                      static_cast<void*>(static_cast<std::byte*>(slots[0]) + pool.payload_size()),
                      static_cast<void*>(static_cast<std::byte*>(slots.back()) + pool.slot_stride())}) {
        pool_reject(pool, metadata, bad);
    }
    CHECK(other_pool.deallocate(other_slot));

    // Repeated full release in a permutation, followed by LIFO reuse, exposes
    // lost/duplicate links. 7 is coprime with 19, so every slot is released once.
    for (std::size_t round = 0; round < 8; ++round) {
        std::vector<void*> released;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            void* slot = slots[(i * 7 + round) % slots.size()];
            CHECK(pool.deallocate(slot));
            released.push_back(slot);
            CHECK(pool.live_count() == slots.size() - i - 1);
            pool_reject(pool, metadata, slot); // Double release while still free.
            check_accounting(pool, metadata.size());
        }
        CHECK(pool.free_count() == pool.capacity());
        std::vector<void*> reacquired;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            void* slot = pool.allocate();
            CHECK(slot == released[released.size() - 1 - i]);
            CHECK(std::find(reacquired.begin(), reacquired.end(), slot) == reacquired.end());
            reacquired.push_back(slot);
            std::fill_n(static_cast<std::byte*>(slot), pool.payload_size(), std::byte{0x37});
            check_accounting(pool, metadata.size());
        }
        CHECK(pool.allocate() == nullptr);
        slots = std::move(reacquired);
    }
    // Mixed allocate/release while other slots stay live; validate their payload
    // remains intact and every slot is eventually recovered.
    for (void* slot : slots) CHECK(pool.deallocate(slot));
    slots.clear();
    std::vector<void*> live;
    std::uint32_t random = 0x76543210U;
    for (std::size_t operation = 0; operation < 1000; ++operation) {
        random = random * 1664525U + 1013904223U;
        if (!live.empty() && ((random >> 8) % 3 == 0 || live.size() == pool.capacity())) {
            const auto index = static_cast<std::size_t>(random) % live.size();
            void* slot = live[index];
            CHECK(pool.deallocate(slot));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            pool_reject(pool, metadata, slot);
        } else {
            void* slot = pool.allocate();
            CHECK(slot != nullptr && std::find(live.begin(), live.end(), slot) == live.end());
            std::fill_n(static_cast<std::byte*>(slot), pool.payload_size(), std::byte{0x6b});
            live.push_back(slot);
        }
        for (void* slot : live) {
            const auto* bytes = static_cast<const std::byte*>(slot);
            CHECK(std::all_of(bytes, bytes + pool.payload_size(), [](std::byte b) { return b == std::byte{0x6b}; }));
        }
        CHECK(pool.live_count() == live.size());
        check_accounting(pool, metadata.size());
    }
    for (void* slot : live) CHECK(pool.deallocate(slot));
    live.clear();
    for (std::size_t i = 0; i < pool.capacity(); ++i) {
        void* slot = pool.allocate();
        CHECK(slot != nullptr && std::find(live.begin(), live.end(), slot) == live.end());
        live.push_back(slot);
    }
    CHECK(pool.allocate() == nullptr);
    for (void* slot : live) CHECK(pool.deallocate(slot));
    CHECK(pool.live_count() == 0 && pool.free_count() == pool.capacity());
}

void pool_lifetimes() {
    Counted::clear_counts();
    alignas(256) std::array<std::byte, 256> backing{};
    std::array<SlotState, 4> metadata{};
    FixedPool pool(backing, metadata, sizeof(Counted), alignof(Counted));
    void* storage = pool.allocate();
    CHECK(storage != nullptr);
    {
        Object<Counted> object(std::construct_at(static_cast<Counted*>(storage), 123));
        CHECK(object->value == 123 && Counted::live == 1);
    }
    CHECK(Counted::destroyed == 1 && Counted::live == 0);
    CHECK(pool.deallocate(storage));
    void* throwing_storage = pool.allocate();
    CHECK(throwing_storage == storage);
    bool threw = false;
    const auto before = pool_state(pool);
    try { (void)std::construct_at(static_cast<Counted*>(throwing_storage), 0, true); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && pool_state(pool) == before);
    CHECK(Counted::attempts == 2 && Counted::constructed == 1 && Counted::destroyed == 1);
    // A failed construction has no complete Counted object to destroy; caller
    // explicitly returns its raw reservation. The pool never guesses lifetimes.
    CHECK(pool.deallocate(throwing_storage));
    storage = pool.allocate();
    CHECK(storage != nullptr);
    {
        Object<Counted> fresh(std::construct_at(static_cast<Counted*>(storage), 456));
        CHECK(fresh->value == 456);
    }
    CHECK(pool.deallocate(storage));
    CHECK(Counted::constructed == 2 && Counted::destroyed == 2 && Counted::live == 0);
    CHECK(pool.live_count() == 0 && pool.free_count() == pool.capacity());

    alignas(256) std::array<std::byte, 1024> aligned_backing{};
    std::array<SlotState, 4> aligned_metadata{};
    FixedPool aligned_pool(std::span(aligned_backing).subspan(1), aligned_metadata,
                           sizeof(OverAligned), alignof(OverAligned));
    check_accounting(aligned_pool, aligned_metadata.size());
    void* aligned_storage = aligned_pool.allocate();
    CHECK(aligned_storage != nullptr && address(aligned_storage) % alignof(OverAligned) == 0);
    {
        Object<OverAligned> object(std::construct_at(static_cast<OverAligned*>(aligned_storage), 9876U));
        CHECK(object->value == 9876U);
    }
    CHECK(aligned_pool.deallocate(aligned_storage));
}

void pool_tests() {
    pool_configuration_tests();
    pool_trace_tests();
    pool_lifetimes();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: runtime-memory-core layout|arena|pool\n";
        return 2;
    }
    const std::string_view selector(argv[1]);
    try {
        if (selector == "layout") layout_tests();
        else if (selector == "arena") arena_tests();
        else if (selector == "pool") pool_tests();
        else { std::cerr << "unknown selector: " << selector << '\n'; return 2; }
        std::cout << selector << ": passed (release-enabled CHECK)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << selector << ": failed: " << error.what() << '\n';
        return 1;
    }
}
