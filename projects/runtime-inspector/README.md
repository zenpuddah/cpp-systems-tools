# C++ Runtime & Performance Inspector

A small native developer tool that builds and inspects its **own bundled C++ specimen**, then reports equivalent CPU/memory/concurrency workloads. Not an arbitrary-process debugger, executable parser or production profiler.

## Features

- Two translation units with a global, automatic object and owned dynamic allocation; actual preprocess, assembly, object, symbols, relocation, disassembly and layout artifacts.
- Diagnosed missing-translation-unit link failure, static archive and runnable shared-library client, dependencies and live process maps.
- Clearly labeled native **Mach-O/macOS** or **ELF/Linux** observations; an explicitly invented page translator model.
- Runtime-input AoS/SoA and shared/adjacent/128-byte-spaced/local aggregation, checked modulo-2^64 sums, raw timings/checksums and an independent Python reference.
- Separate owned bounded mutex/CV queue with monotonic closure, draining, rejected pushes, blocked-waiter wakeups and joined-worker tests. It is not part of the TCP server.
- M01–M05 memory module: actual member/padding byte maps, non-owning bump arena, bounded checked fixed pool, counted PMR routes/lifetimes, equivalent allocation samples and a labeled free-interval model. See [memory guide](../../docs/MEMORY.md).

## Quick start

From the repository root, with C++20, CMake 3.25+, Ninja and Python 3.10+:

```sh
python3 projects/runtime-inspector/report.py --output reports/runtime
cmake --preset release
cmake --build --preset release
ctest --preset release -R '^runtime-' --output-on-failure
ctest --preset release -L memory --output-on-failure
build/release/projects/runtime-inspector/runtime-workloads --size 200000 --threads 4 --seed 42 --repetitions 7
```

The report entry point needs only a compiler and Python, not a prior CMake build. macOS uses Apple command-line tools (`nm`, `otool`, `ar`, `vmmap`); Linux uses binutils (`nm`, `readelf`, `objdump`, `ar`), `file`, and `/proc`. Use `--cxx clang++` or `$CXX` to select the compiler. This workflow is POSIX, not native Windows support.

Example output:

```text
passed: .../reports/runtime/report.md
Native observation: Mach-O / macOS, arm64
layout: size=24, alignment=8, offsets tag=0 value=8 count=16
missing operations.o: deliberate undefined specimen_transform/specimen_global
```

Open `report.md`; `report.json` contains exact commands, return codes, compiler/flags, OS/ISA, source/test SHA-256 and capability outcomes. `workloads.json` holds every raw timing/checksum, seed, size, thread/repetition count and timing boundaries. `.ii`, `.s`, `.o`, `.a`, native shared library and executable artifacts remain locally inspectable. Reuse only a tool-owned output directory: known output names are regenerated. Tool logs are capped at 2 MB each; subprocesses have deadlines and owned process-group cleanup. Optional profiler/debugger failures are visible but do not turn required artifact success into failure. Inspect `errors` and individual statuses, not merely the exit code.

## Architecture and observations

See [architecture.md](architecture.md) for the diagrams and memory model. `specimen/main.cpp` calls `specimen_transform` in `operations.cpp`; `-O0` preserves an understandable native call. The CMake Release target may optimize differently: the report records its own explicit flags (`-O0` specimen, separate `-O2` workloads). Shared and static clients run the same main object and must return the same computed value. Linking only main.o must fail for both missing symbols. `call-trace.txt` excerpts the actual linked disassembly; ABI annotation describes the observed native ISA, not a C++ calling-convention guarantee.

### C++ lifetime, layout and language boundaries

- `specimen_global` has static storage duration; `automatic` lasts to the end of main's scope; a `unique_ptr` owns the dynamic object. Automatic/dynamic storage duration are language concepts; the particular stack/heap mappings and address order are implementation observations, not required by C++.
- The standard-layout `Layout` uses `offsetof`, `sizeof` and `alignof` to observe padding/alignment. Members are not necessarily packed; copying raw padding bytes is not a portable serialization format.
- `unique_ptr` is move-only ownership, not shared lifetime. Queue `push(unique_ptr<T>)` takes ownership even when rejected (destroying the argument); `pop()` transfers ownership out. Null items are invalid; null pop means closed and fully drained. Destruction is nonthrowing. Callers close and join before destroying a queue.
- Vector growth is forced past the **actual** prior capacity. The old address is captured as an integer, never dereferenced after reallocation. Capacity-changing reallocation invalidates every element pointer/reference/iterator even if an allocator later reuses an address. Reserved capacity is not object count. No growth factor is assumed.
- A reference aliases an existing object and does not own or extend arbitrary lifetimes. Copying a value creates a different object; moving transfers resources according to the type's contract and does not make every moved-from object empty. The code uses RAII, explicit transfer and joined thread lifetimes rather than raw owning pointers.
- Unsigned arithmetic intentionally wraps modulo 2^64; no signed overflow, dangling dereferences, `volatile` timing tricks or `-ffast-math`. Atomic relaxed updates supply atomicity; local results use one explicit release/acquire publication per worker in addition to the completion barrier.

### Workload contract and timing

`value(i, seed) = (((uint64(i) XOR seed) * 0x9e3779b97f4a7c15 + seed) mod 2^64) >> 17`. Sum values modulo 2^64. Python arbitrary-precision tests implement the masked formula independently, with empty, small, uneven, maximal-seed and sum-overflow fixtures; C++ includes literal reference values.

AoS records contain the hot value and seven cold uint64 fields; SoA traverses just the hot-value array. They intentionally compute the same sum while touching different layouts. Shared uses a single relaxed atomic; adjacent uses consecutive atomic slots; spaced uses `alignas(128)` slots; local uses a private scalar and one per-thread publication followed by reduction. Each worker covers identical strided indices. 128 bytes is an experiment parameter, not a discovered universal cache-line size. Atomic lock-free availability is reported.

Allocations, generation, expected sum and thread creation occur before timing. One checked warm-up per variant is omitted from output; measured order rotates each repetition. AoS/SoA time only traversal. Aggregation times release/finish gates, hot loop and final reduction; it excludes thread startup/join. All checksums are observed after timing. Synchronization overhead means tiny-workload comparisons are not pure ALU timings. A zero clock duration is retained with `timing_status=below_clock_resolution` and `items_per_second=null`; no divide-by-zero or fabricated duration. Nonzero rates use fractional floating division. Large-size inputs are bounded at 10,000,000 items, threads 1–32, repetitions 1–30; peak layout storage is roughly 72 bytes/item plus runtime overhead.

The report asks one CPU/concurrency question: does private accumulation reduce observed time relative to the shared atomic? It records shared baseline, spaced and local candidates (at most two), adjacent diagnostic control, independent correctness and repeated timing. Results do not automatically establish cache/false-sharing causality. No performance-driven source change or universal speedup is asserted. Counter/trace/debugger capabilities are separately probed; a sample of the held specimen normally shows sleeping, not a CPU workload hotspot.

## Tests and results

Real CTest cases: `runtime-core`, `runtime-queue`, `runtime-workloads-oracle`, `runtime-report`. Checks use throwing `CHECK`, never release-disabled assert. Concurrent tests carry `concurrency`; the report is `inspection`. Queue tests deterministically observe blocked-waiter state before closure (not arbitrary sleeps), cover empty/full/uneven producer/consumer counts, capacity 1 and 7, accepted exact-once identity, owned-item destruction, reject/drain, repeated close and joins. Every test/subprocess has a deadline.

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan -R '^runtime-' --output-on-failure
# Repeat with ubsan; TSan is separate, never combined with ASan:
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -R '^runtime-' -L concurrency --output-on-failure
```

Actual outcomes and exact evidence identities: [progress.md](progress.md). The report test compiles its own uninstrumented inspected specimen/workload even when CTest is running under a sanitizer preset; the core/queue/oracle CMake executables are instrumented by that preset. Timings retained as performance evidence come only from separate optimized, uninstrumented runs.

## Limitations and platform comparison

Linux is the reference route: use a Linux machine or a local Ubuntu/Debian VM matching the intended ISA; install compiler/CMake/Ninja/Python/binutils/file and optionally perf/strace/gdb, then run the same presets/report. macOS output is not Linux verification. VM hardware counters and ptrace/perf permissions may be unavailable even when tools are installed; do not use elevated permissions silently. Host/guest and ISA must be recorded for any new VM run.

No physical-address reading, arbitrary-process inspection, custom ELF/Mach-O parsing, intentionally crashing memory demo, GUI, server thread pool or lock-free queue. ASLR, allocator and linker behavior vary. Dynamic-loader startup, MMU/TLB and COW explanations are documented models unless specifically observed. Windows **PE** executable sections, image mappings, import tables, `.lib` static/import libraries and **DLL** loading provide analogous stages, but PE/DLL commands/results are documented comparison only, not verified here. `PGM` remains **unresolved**, not a guessed acronym. Sanitizer outcomes cannot prove absence of all races or memory bugs; CLion interactive debugging is not claimed.
