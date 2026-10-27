# M04–M05 implementation and evidence

## Executable plan / ownership

Only `include/memory_lab/{pmr,experiments}.hpp`, `src/{pmr,experiments,main}.cpp`,
`tests/{pmr,costs}.cpp` and this file belong to this slice. No core implementation,
CMake, report script, root ledger or commits are changed by this agent.

1. Concrete `CountingResource` and typed `observe_pmr` results: fixed-size alignment
   histogram, bounded upstream budgets, explicit vector/string resource routes,
   collective release only after destruction, copied/moved allocator observations.
2. `run_experiments`: common noinline dispatch, deterministic compatible sizes,
   separate raw/object phases, setup/warmup/teardown and reclamation boundaries.
   Caller-buffer custom allocators use the exact `MEMORY_API.md` interfaces.
3. Deterministic `FreeIntervalModel` and focused release-enabled `CHECK` tests.
4. Compile focused dev/release/ASan/UBSan binaries in an approved temporary build
   directory after core headers arrive; run timings only on optimized uninstrumented
   binaries. Record actual evidence below, not intended passes.

## Public API / driver schema (integration contract)

`runtime-memory [--size N] [--seed N] [--repetitions N]`, defaults `128/42/3`.
Size **1..2048**, seed **0..UINT64_MAX**, repetitions **1..20**. Duplicate/unknown,
signed, overflowing or missing arguments fail with exit 2 and a diagnostic on
stderr; no partial JSON is emitted. `--help` prints usage. Successful invocation
emits exactly one JSON document, schema `memory-lab-v1`, and exits 0 if fixtures
match (1 for a reported fixture mismatch, 2 for exceptions).

Top-level keys: `schema`, `fixture_ok`, `config`, `environment`,
`layout.fixture_layouts`, `arena`, `fixed_pool`, `pmr`, `experiments`.
`layout.fixture_layouts[]` includes `name`, `object_size`, `object_alignment`,
`array_stride`, `members[]`, `padding[]`, `byte_owner[]`, `internal_padding` and
`tail_padding`, directly from core `fixture_layouts()`.

`pmr` has `monotonic_fallback`, `monotonic_hardcap`, `pool`, `copy_move`, `policies`.
Each route records `frontend`/`upstream` counter objects, initial buffer bytes,
upstream snapshots before/after block free and release, `final_upstream_bytes`,
`constructed`, `destroyed`, `fixture_ok`, `bad_alloc_observed`. Counter objects:
`allocation_requests`, `allocations` (successful), `failures`, `deallocations`,
`allocated_bytes`, `deallocated_bytes`, `current_bytes`, `peak_bytes`,
`successful_alignment_counts: [{alignment,count}]`. Counters have no per-call heap
ledger; standard correct pointer/byte/alignment/resource deallocation preconditions
still apply. Equality is counting-wrapper identity, not upstream equivalence.
The live-byte budget and cumulative-byte counter overflow reject before forwarding.
Block-free snapshots are null with a reason when that route has no successful
explicit block/free pair (hard-cap failure and the container-only standard pool).

`experiments`: full `trace_bytes[]`, configuration/trace description, alignment,
object/slot sizes, measurement/metric definitions, unsupported shapes,
`backends[]`, `fragmentation`. Backends named `new_delete_resource`, `bump_arena`,
`fixed_pool`, `pmr_monotonic`, `pmr_pool`; each has `warmup_ns`, `samples[]`.
Each sample has `repetition`, `raw`, `objects`. Each phase contains
`timings_ns: {setup,allocate,deallocate,construct,destroy,release_or_reset,teardown}`,
fractional `allocation_operations_per_second`, counts, checksums, fixture/timing
statuses, errors, `storage_bytes`, frontend/upstream counters. Unsupported timers
are JSON null (e.g. arena individual free, raw object construction). Invalid
elapsed produces a null rate and explicit error, never division by zero.
The custom raw arena/fixed-pool phases have `frontend: null` plus a reason:
`allocations/deallocations` count their dispatch operations, not fictitious PMR
requests. All actual PMR/host paths have concrete frontend counter objects.

Storage counters describe the all-live point unless their name says otherwise.
Definitions are embedded in `experiments.metric_definitions`. In particular,
resource-acquired bytes are explicit upstream requests, not host size classes or
RSS. PMR cursor, slack and internal metadata and host hidden bytes are **unknown**
null with a reason. Caller byte-buffer capacity and typed fixed-pool metadata are
separate. `retained_not_live_after_deallocate` includes unused buffer capacity and
retained upstream chunks with zero client-live payload, not just dead allocations;
dead workload payload has a separate metric. PMR pool internal reuse is unknown.
The fixed pool has a 64-byte payload and alignment 16 for this equivalent trace;
arbitrary larger requests are explicitly unsupported, never truncated.

Raw and object trials have fresh resources, identical simultaneous-live byte
requests, payload checksums, counts and destruction obligations. Object payload
requests reserve at least `sizeof(Object)` and can contain unused tail storage.
Raw touch/checksum is outside allocation/deallocation timing. Allocation pointers
escape into an observed table and all payload bytes are read after all writes.
No forced speedup or equal reclamation claim: arena has no individual free;
monotonic deallocation is only a request and retains dead blocks until release;
fixed pool validates releases in O(capacity); standard pool mechanics are opaque.
PMR frontend/upstream counter overhead is included, not subtracted; custom raw
allocators are not wrapped in PMR. These are measured tool paths, not isolated
universal allocator instruction costs.

`fragmentation` is a 64-byte free-interval **model**, not a libc heap. Four 16-byte
blocks, holes [0,16), [32,48): total free 32, largest 16, request 24 fails. A
contiguous 24-byte hole succeeds. Freeing [16,32) gives total 48/largest 16 before
explicit coalescing, then one [0,48) hole and request 24 succeeds. No host heap or
general malloc implementation is inferred.

## Lifetime policies / limits

Buffers/upstreams precede resources, resources precede containers. Containers and
objects are destroyed before collective release. Keeping a cleared vector and
releasing its resource is deliberately **not executed**: capacity still refers to
invalidated storage. Monotonic per-block deallocation is matched but not treated
as cursor reclamation. Fresh resources are used for independent trials rather
than relying on implementation-specific post-release behavior. Copy construction
without an explicit allocator selects the current default; explicit copy/move
construction uses the chosen destination; ordinary move construction uses source;
move assignment retains destination. No global default/new overrides.
An explicit move to an unequal resource cannot generally steal source storage;
moved-from container contents are unspecified and are not asserted here.

Upstream budgets: PMR demo 1 MiB live; timing phases 16 MiB live. Pool options are
hints and metadata can allocate upstream during construction. No fixed chunk-count,
growth-factor, per-free upstream-return or universal performance assertion.

## Actual evidence

Initial discovery: branch `extension/memory-v1`, base
`7d0759ca9535ea10d056400a9db435212c4d67c8`; concurrent coordinator/core edits left
untouched. Native `uname -a`: Darwin 25.6.0 arm64; Apple clang 21.0.0
`clang-2100.1.1.101`, target `arm64-apple-darwin25.6.0` (commands exit 0).
Initial strict syntax check **failed** (exit 1) on cross-allocator `pmr::string ==
std::string`, with tests **not run** for that check. Corrected to independent size
and character fixtures. The subsequent strict five-file syntax check passed.

Final evidence directory (approved temporary location, no repository binary/log
files):
`/private/var/folders/14/mvcf30lj2bz5nmw28f8nr1ch0000gn/T/opencode/memory-experiment-final-awv0vdl3/`.
`verification.json` holds every exact argv, exit, log path, compiler identity,
source/test SHA-256 and binary SHA-256. Its SHA-256 is
`b961e3c2328dd7cf7110142009455c9765b40cd95cc018f2faa6ee707618827c`.
All commands ran from the public worktree root. No source drift during this run.
The manifest covers the three core headers as well as these seven implementation/
test files; canonical sorted compact JSON manifest SHA-256:
`8c704fbf2708dfa9f91c9de8dd2061dd6136179f85a4cca9ab58d1127673d825`.
The seven owned source/test entries alone have the corresponding manifest SHA-256
`582af59dd5315ceac7adeeca863d339df3e2fab6d8606615f373e58a00be89a8`.
Test identities:

| File | SHA-256 |
| --- | --- |
| `tests/pmr.cpp` | `3d4ec83629646a1850bcd0a7d7085598fded68fb41770fbe85b84fa766e8cc28` |
| `tests/costs.cpp` | `0360b8c833677a30b863dc725f6028922749a1d287eeec0079cf0f2c26d28cf5` |

For reproduction, let `M=projects/runtime-inspector/memory`, `B` be a fresh
approved temporary directory and `F` one of the flags below. Exact compilation
shape for each of `T=pmr,costs`:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I"$M/include" $F "$M/src/pmr.cpp" "$M/src/experiments.cpp" "$M/tests/$T.cpp" -o "$B/runtime-memory-$T"
"$B/runtime-memory-$T"
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I"$M/include" $F "$M/src/pmr.cpp" "$M/src/experiments.cpp" "$M/src/main.cpp" -o "$B/runtime-memory"
```

| Configuration / F | Builds | Focused executable tests |
| --- | --- | --- |
| dev: `-O0 -g` | 3/3 passed, exit 0 | 2/2 passed, exit 0 |
| release: `-O3 -DNDEBUG` | 3/3 passed, exit 0 | 2/2 passed, exit 0 |
| ASan: `-O1 -g -fsanitize=address -fno-omit-frame-pointer` | 3/3 passed, exit 0 | 2/2 passed, exit 0 |
| UBSan: `-O1 -g -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all` | 3/3 passed, exit 0 | 2/2 passed, exit 0 |

Sanitizer binaries were used only for correctness, not performance evidence.
Separate optimized **uninstrumented** driver invocations all passed, exit 0,
with independent Python JSON/layout/counter/payload/destructor/model validation:

```sh
"$B/release-runtime-memory"
"$B/release-runtime-memory" --size 1 --seed 0 --repetitions 1
"$B/release-runtime-memory" --size 128 --seed 42 --repetitions 7
"$B/release-runtime-memory" --size 2048 --seed 18446744073709551615 --repetitions 20
```

Raw JSON: `release-driver-{0,1,2,3}.json`. Twelve invalid-CLI cases passed: each
returned exit 2, diagnostic stderr, empty stdout (zero/oversized/negative/invalid/
duplicate size, overflowing/negative/plus-prefixed seed, zero/oversized repetitions,
missing value and unknown option). Valid maximum bounds retained all raw samples.

### Controlled observation (not a universal speedup)

Question: for the same bulk lifetime, what storage survives client deallocation,
and what allocation cost does this instrumented tool path observe? In the
size=128/seed=42/repetitions=7 run, all backends requested **5149 payload bytes**.
Medians of seven raw allocation samples (nanoseconds, setup/touch/reclamation
excluded): new-delete resource **2541**, arena **1333**, fixed pool **417**,
PMR monotonic **792**, PMR pool **2709**. These tiny platform-specific measurements
include common dispatch and unequal resource/counter/validation mechanics; no
speedup guarantee, subtraction of guessed overhead or causal universal claim.

Arena consumed **6095** = 5149 payload + **946** alignment slack. Fixed pool
occupied **8192** live slot bytes, **3043** rounding, and **2160** explicitly counted
control/reserved typed-metadata bytes. Arena/fixed/monotonic caller byte buffers
were each **16896** bytes (including unused capacity). Monotonic needed **zero**
counted upstream bytes for this trace; its internal cursor remains unknown.
The standard pool acquired a measured peak **8648** upstream bytes, but this
libc++ run returned them by the end of client deallocation: observed remaining
upstream **0**, not a promise of retention or immediate return on other libraries.
Every resource's final upstream live bytes was zero after destruction. The model
independently confirmed total-free **32** versus largest-block **16** failure for
request **24**, and adjacent-hole merging **48/16 -> 48/48**.

Existing project regressions, CTest integration, publication and durable copying of
temporary evidence are coordinator-owned, **not run by this slice**. Linux
extension checks and CLion GUI demonstration: **not run** by this slice. No commits
were made. No learner competence is asserted.

Next action: coordinator integrates this schema into the existing Inspector report,
runs combined regressions/CI on final source identities, and preserves/publishes
the selected evidence before the private delta.

## Coordinator Linux follow-up (supersedes original trace alignment)

Actual GCC13.3/libstdc++ CI40597511941 built after the CTAD-test fix, then the cost
trace's alignment check caught misaligned standard-pool blocks before any typed
object construction. This matches known libstdc++ PR118681: size classes such as
24/40/56 need not satisfy a requested 16-byte alignment for every block. Core
allocator/PMR-route tests passed; cost/driver/report checks failed and the dependent
report-artifact check was not run. Actual failed evidence is retained in root
evidence/memory-v1/ci/40597511941/, not called a pass.

The equivalent cost trace now uses the counted object's **natural alignment**
(8 on supported ARM64/x86-64); its variable byte sizes, seeds, simultaneous-live
payloads and identical request sizes across all five backends are preserved.
This avoids that known library shape without silently rounding only one backend's
requests or weakening pointer checks. It is a deliberately bounded cost experiment,
not verification of every standard-library pool alignment. Custom arena/pool
over-alignment remains covered through 256; PMR standard deallocation preconditions
remain unchanged. Earlier 16-byte-aligned timing observations above are historical;
new root versioned manifests/investigations identify the corrected implementation.

Reference: [GCC PR118681 fix](https://github.com/gcc-mirror/gcc/commit/ac2fb60a67d6d1de6446c25c5623b8a1389f4770).
