# Memory layout and allocation investigation

M01–M05 extend the existing Inspector; the server, CPU arithmetic and queue are unchanged. Components are small C++20 raw-storage views, not a production allocator framework. Implementation verification is not evidence of independent learner skill.

## Run

```sh
python3 projects/runtime-inspector/report.py --output reports/memory --memory-size 128 --memory-repetitions 5
cmake --preset release
cmake --build --preset release
ctest --preset release -L memory --output-on-failure
```

The same report retains compiler/build commands, OS/ISA, flags, binary/source identities, `memory.json` and raw samples alongside original native build/link/load/workload observations. Memory timings use an independent optimized uninstrumented binary; sanitizer passes are correctness checks only. Read current ledgers for actual outcomes, not this command recipe.

Memory driver bounds: size 1–2048, repetitions 1–20, uint64 seed. Report options use `--memory-size` and `--memory-repetitions`; the standalone executable uses `--size` and `--repetitions`. `python3 scripts/measure_memory.py` retains a separate seven-repetition optimized Inspector investigation below `evidence/memory-v1/investigation/`, with exact source/measurement identities and no stale-artifact fallback.

## Layout: space is not semantic data

`memory/include/memory_lab/layout.hpp` observes standard-layout fixtures with identical `char`, `double`, `uint32_t` fields in two orders, plus `alignas(64)`. It uses actual `sizeof`, `alignof`, `offsetof` and array stride. A computed cell per byte denotes a member, internal gap or tail padding; no padding contents are read. Reordering savings and offsets are ABI observations, not universal assertions.

For example, native ARM64/Clang 21 core checks observed the original char ending at byte 1 and double starting at byte 8: seven bytes of internal padding satisfy double alignment. Four tail bytes rounded its last member end (20) to size/stride 24, keeping array elements aligned. The reordered fixture measured 16 bytes here. Other ABIs may differ. Member alignment, object alignment, allocator alignment and cache-line spacing are separate quantities. `alignas(64)` does not discover a universal cache-line size. Do not serialize raw struct images or compare padding with `memcmp`; use explicit wire fields and byte order.

## Storage, object lifetimes and checked policies

`BumpArena` borrows a caller byte span. It supports power-of-two alignments 1–256; zero sizes, unsupported alignment, overflow-sized requests and exhaustion return null without state changes. Successful `consumed = payload + padding`; unaligned starts are supported. There is no individual free. Destroy constructed objects before reset: reset runs no destructors and invalidates old handles. A throwing constructor leaves its reservation consumed; no unsafe rewind can erase nested allocations.

`FixedPool` borrows separate byte storage and existing typed `SlotState` metadata. It rounds nonzero payload to aligned stride and takes the minimum of metadata count and storage capacity. Front slack + slot-count × stride + tail = backing capacity; reserved/used metadata are separate storage. Allocate/free-list manipulation is O(1), but the checked deallocation API validates starts using O(capacity) pointer equality. Null, foreign, interior and double releases leave state unchanged; it never subtracts unrelated pointers or dereferences an untrusted pointer. It cannot distinguish a stale pointer from a later valid allocation at the same slot address. Both views are single-threaded/noncopyable; callers own object construction/destruction and all backing lifetimes.

Variable-size individual reclamation needs a different interface/policy, not a fiction that arena reset or fixed slots implement general malloc/free.

## PMR and comparison boundaries

Client request counters and counting-upstream observations answer different questions: initial-buffer requests need not reach upstream. A supplied-buffer monotonic resource explicitly selects fallback or `null_memory_resource` hard-cap behavior. Standard pool metadata/chunks may allocate upstream; their implementation-dependent growth is not hard-coded. Standard deallocate preconditions require matching resource, original size and alignment—custom-pool checked-invalid-release tests do not apply to PMR.

Destroy containers/objects before collective release, then reconstruct them. `clear()` does not discard container capacity or grant permission to release backing storage and keep using that container. Resources and supplied buffers must outlive clients. Copy/move resource selection must be explicit; no process-wide default resource replacement is used. Inspect final upstream accounting after resource destruction, not just per-client frees.

Equivalent bulk-lifetime traces compare new/delete-backed, arena, compatible fixed-slot and PMR routes. Raw storage, object construction/destruction, collective reset/release, setup and warm-up are separate timing boundaries. Checked pool deallocation includes its validation cost. Unsupported arbitrary-size/fine-grained arena lifetimes are declared, not forced into misleading comparisons. Tiny nonpositive timing intervals have no fabricated throughput or guaranteed speedup.

Requested payload, backing capacity, acquired upstream bytes, consumed arena storage, alignment gaps, slot round-up, metadata and retained dead storage have distinct meanings. Unknown PMR cursor/metadata and host allocator internals are not guessed from RSS. Unused fixed slots are not automatically external fragmentation. A deterministic free-interval **model**, not a libc heap reconstruction, contrasts total-free versus largest-contiguous space and adjacent-hole coalescing. Temporal retention is distinct from both internal and external fragmentation.

## Verification and references

`python3 scripts/verify_memory.py dev release asan ubsan tsan` runs the combined regression matrix into versioned evidence. Focused checks keep Release checks enabled. No intentionally dangling allocation, use-after-release or invalid standard-resource deallocation is executed. CLion interactive demonstration stays pending until actually shown.

Primary C++20 references: [layout](https://timsong-cpp.github.io/cppwp/n4861/support.types.layout), [alignment](https://timsong-cpp.github.io/cppwp/n4861/basic.align), [std::align](https://timsong-cpp.github.io/cppwp/n4861/ptr.align), [monotonic resource](https://timsong-cpp.github.io/cppwp/n4861/mem.res.monotonic.buffer), [pool resources](https://timsong-cpp.github.io/cppwp/n4861/mem.res.pool). Component details: `projects/runtime-inspector/memory/CORE-NOTES.md` and `EXPERIMENT-NOTES.md`.
