# Fixed interfaces for independent memory-extension work

All files below are under projects/runtime-inspector/memory; C++20 namespace memory_lab. Common POSIX targets support std::uintptr_t, but invalid pool pointer validation uses equality against generated valid slot starts, not unrelated-pointer subtraction or dereference. Raw APIs are single-threaded and non-owning. Caller buffers and metadata must outlive views/objects. No hidden heap allocation in arena/pool allocate/deallocate paths. No typed helper/rewind policy is needed; callers construct/destroy explicitly. No copies/moves that duplicate active allocation state.

## Core agent: include/memory_lab/layout.hpp

- `MemberExtent { std::string name; std::size_t offset, size, alignment; }`
- `PaddingExtent { std::size_t offset, size; bool tail; }`
- `LayoutMap { std::string name; std::size_t object_size, object_alignment, array_stride; std::vector<MemberExtent> members; std::vector<PaddingExtent> padding; std::vector<int> byte_owner; std::size_t internal_padding, tail_padding; }`
- `LayoutMap layout_map(std::string name, std::size_t size, std::size_t alignment, std::size_t stride, std::span<const MemberExtent> members)`; reject invalid/overlapping/out-of-object extents deterministically. byte_owner has one cell per byte: member index >=0, -1 internal padding, -2 tail padding. No padding values read.
- `std::vector<LayoutMap> fixture_layouts()` supplies standard-layout original/reordered structs with identical char/double/uint32_t fields plus a supported alignas(64) observation. Owned strings avoid name/view lifetime pitfalls. Gate fixtures with is_standard_layout; no universal offsets/padding savings. Actual array stride, sizeof, alignof, offsetof and field extents drive tests.

## Core agent: include/memory_lab/arena.hpp

- `class BumpArena`, `static constexpr std::size_t max_alignment = 256`.
- `explicit BumpArena(std::span<std::byte>) noexcept`; `void* allocate(std::size_t bytes, std::size_t alignment) noexcept`; `void reset() noexcept`.
- const noexcept getters `capacity()`, `consumed()`, `remaining()`, `payload_bytes()`, `padding_bytes()` returning size_t.
- Supported alignments: power of two 1..256. Reject zero bytes/invalid alignment/exhaustion/huge requests with nullptr and unchanged state. consumed includes accepted payload + alignment padding relative to supplied (possibly unaligned) view. reset invalidates old handles, runs no destructor. No individual free and no unsafe constructor-failure rewinds. Copy/move disabled.

## Core agent: include/memory_lab/pool.hpp

- `SlotState { std::size_t next; bool allocated; }` is bounded caller-provided typed metadata, exclusively loaned to the pool while active.
- `class FixedPool`, `static constexpr std::size_t max_alignment = 256`.
- `FixedPool(std::span<std::byte> storage, std::span<SlotState> metadata, std::size_t payload, std::size_t alignment) noexcept`; `void* allocate() noexcept`; `bool deallocate(void*) noexcept`.
- const noexcept getters: `valid()` bool; size_t `payload_size()`, `alignment()`, `slot_stride()`, `capacity()`, `live_count()`, `free_count()`, `alignment_padding()`, `tail_bytes()`, `metadata_reserved_bytes()`, `metadata_used_bytes()`, `backing_capacity()`.
- Nonzero payload, power-of-two alignment 1..256; check stride round-up overflow. Invalid configuration: valid=false, no metadata writes, null allocate/false free. Valid zero-capacity pool allowed. Align initial storage view; capacity=min(metadata slots, aligned bytes/stride). Front slack + usable slot-stride bytes + tail reconcile backing_capacity; used/reserved metadata bytes are separate caller storage. Allocate/free use free-list; validate release by comparing to slot starts (O(capacity)), then allocated state, before touching links. Foreign/interior/double/null release returns false with unchanged state. No pointer-provenance/generation guarantee after a slot is legitimately reused. Raw lifetime management remains caller responsibility. Copy/move disabled.

## Experiments agent

Own include/memory_lab/{pmr,experiments}.hpp, src/{pmr,experiments,main}.cpp, tests/{pmr,costs}.cpp. Use the exact core interfaces above; do not edit core files. Public functions/JSON schema can be chosen and documented in EXPERIMENT-NOTES.md.

Executable `runtime-memory --size N --seed N --repetitions N` emits one valid JSON document covering all five contract areas; keep size/repetition bounds explicit. At minimum provide object layout maps, PMR routes/counters/lifetimes, raw supported-equivalent allocation samples/checksums/timing units and a labeled deterministic fragmentation model. Primitive byte/address/timing observations must not be fabricated. Separate setup/warmup, alloc/dealloc, object construction/destruction and reset/release where meaningful. Report unsupported shapes (pool oversized requests / individual arena frees), and unavailable PMR cursor/host allocator internals as null/reason rather than zero guesses. Do not override global new or the default PMR resource.

Tests: runtime-memory-pmr (tests/pmr.cpp), runtime-memory-costs (tests/costs.cpp); core selector executable runtime-memory-core (tests/core.cpp). Coordinator supplies standalone/subdirectory CMake, adds runtime-memory library/executable/tests and integrates existing report.py after interfaces stabilize. Do not time sanitizer binaries as performance evidence.
