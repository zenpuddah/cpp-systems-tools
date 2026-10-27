# M01–M03 public core — 2026-10-27

Implemented on `extension/memory-v1` in `cpp-systems-tools-memory`, based on
`7d0759ca9535ea10d056400a9db435212c4d67c8` (also the unchanged `public-v1`).
These are uncommitted extension files; this delegated slice makes no commits.
No original checkout, workbook, learner state, CMake, report or experiment file
was changed by the core agent. Coordinator/other-agent changes in the shared
worktree were preserved. Implementation evidence is not learner assessment.

## Executed plan and interfaces

1. `include/memory_lab/layout.hpp`: header-only `MemberExtent`, `PaddingExtent`,
   `LayoutMap`, `layout_map` and `fixture_layouts`, matching `MEMORY_API.md`.
   Validate extents before building the byte map; use actual standard-layout
   `sizeof`, `alignof`, `offsetof` and observed two-element array address stride.
2. `include/memory_lab/arena.hpp`: header-only non-owning `BumpArena`; checked
   raw allocation, payload/padding accounting and collective reset.
3. `include/memory_lab/pool.hpp`: header-only non-owning `FixedPool` with separate
   typed caller metadata, checked stride/capacity, free list and equality-only
   release validation. Both allocator classes delete copying and moving.
4. `tests/core.cpp`: standalone selector executable (`layout|arena|pool`), using
   release-enabled `CHECK`, independent fixture extents, deterministic traces,
   invalid-request state snapshots and explicit object construction/destruction.
   Compile directly to avoid depending on parallel, incomplete experiment work.

## Policies and accounting

### M01: layout space, not padding values

The map supports ordinary non-overlapping member extents, not inheritance,
bit-fields, packed/overlapping members or `[[no_unique_address]]`. Object size
must be nonzero, a multiple of its power-of-two alignment, and equal to array
stride. Each member has nonzero size, power-of-two alignment no greater than
object alignment, aligned offset and size, and checked in-object bounds. Bad
extents/overlap throw `std::invalid_argument`; extent arithmetic is checked before
addition. Input order need not be declaration order. Names are owned strings.
An empty extent list explicitly describes all bytes as unassigned tail space.

`byte_owner` is a *computed layout map*: member index, -1 internal padding or -2
tail padding. No object padding is read, serialized or compared. Original and
reordered fixtures have identical named `char`, `double`, `uint32_t` fields;
standard-layout assertions gate all fixture uses of `offsetof`.

Observed macOS ARM64 / Apple Clang 21 layout (not universal ABI assertions):

| Fixture | sizeof / array stride | alignof | Internal bytes | Tail bytes |
| --- | ---: | ---: | ---: | ---: |
| original | 24 | 8 | 7 | 4 |
| reordered | 16 | 8 | 0 | 3 |
| alignas(64) | 64 | 64 | 7 | 44 |

In this observation the original's `char` ends at offset 1 and its `double`
starts at offset 8: the seven-byte internal gap satisfies member alignment. Its
last member ends at offset 20, and four tail bytes round size to 24 so successive
array elements satisfy object alignment. `alignas(64)` increases *object*
alignment and the array stride/tail here, not the natural scalar member
alignments. Allocation alignment must satisfy the constructed object's
alignment; arbitrary raw bytes have no such typed-object requirement. A 64-byte
alignment observation does not discover a machine's cache-line size or prove
absence of false sharing. Reordering need not save space on every ABI.

A memory image contains padding and ABI-specific representations; it is not a
portable wire format. Encode explicit fields, byte order and lengths instead.

### M02: arena storage versus object lifetime

Supported allocation alignment is power-of-two 1 through 256. Zero payload,
invalid/unsupported alignment, huge sizes and exhaustion return null with all
counters unchanged. `std::align` is used only on a nonempty in-view candidate;
success consumes **both** padding and payload. No hidden heap/upstream allocation
occurs. `consumed = payload_bytes + padding_bytes`, and
`remaining = capacity - consumed`; initial alignment loss is charged only for
successful requests. Failure never commits alignment padding alone.

The caller owns and keeps the byte span alive. There is no individual free.
`reset()` runs no destructors and invalidates every old handle. Tests destroy
constructed objects before reset, then obtain fresh handles rather than
dereferencing stale ones. A throwing constructor leaves its raw reservation
consumed. A tested throwing outer constructor also allocates and successfully
constructs a nested object: that nested object survives the exception, is not
overwritten by later allocation, and is explicitly destroyed before reset.
There is no typed helper or rewind across nested allocations.

### M03: bounded free list and separate checked release

Storage and metadata are separate, non-overlapping valid caller spans. Typed
`SlotState` elements must already exist and are exclusively loaned to the pool;
callers must not modify them while active. Payload is nonzero and alignment is
power-of-two 1 through 256. Round-up is checked before addition, then
`stride = round_up(payload, alignment)`. Invalid configurations do not write
metadata and cannot allocate/release. Valid zero-capacity configurations are
allowed. Only metadata elements below capacity are initialized/used.

For a valid pool:

- `capacity = min(metadata.size(), (backing_capacity - alignment_padding) / stride)`.
- `alignment_padding + capacity * stride + tail_bytes = backing_capacity`.
- If the first aligned address would be outside the byte view, the entire view
  is front slack and capacity is zero; no out-of-view pointer is formed.
- `metadata_reserved_bytes = metadata.size() * sizeof(SlotState)` and
  `metadata_used_bytes = capacity * sizeof(SlotState)` are **separate caller
  storage**, including each typed metadata element's implementation padding.
- `live_count + free_count = capacity`; per-live-slot internal rounding is
  `stride - payload_size`. Free slots and tail slack are not live payload and
  should not simply be called external fragmentation.

Allocate and free-list link insertion/removal are O(1). The checked public
`deallocate` additionally scans **O(capacity)** valid slot starts, comparing only
pointer equality before reading trusted metadata. It never orders/subtracts
unrelated pointers or dereferences a supplied pointer. Null, foreign, interior,
padding, one-past and already-free starts are rejected without changing metadata
or counters. Repeated release/reuse tests also verify the hidden head/link state.
This is not a generation/provenance handle scheme: an old pointer numerically
equal to a legitimately reused slot start is indistinguishable from the current
handle. Tests do not misrepresent reuse as stale-handle detection.

The caller constructs/destroys objects and destroys each live object before
returning its slot. Failed construction has no complete object to destroy; the
caller explicitly returns that raw reservation. The pool does not infer object
lifetimes or run destructors. Both allocators are single-threaded views, not
owners. If variable-size individual deallocation is required, this fixed-slot
interface is insufficient: select an existing general allocator/PMR resource,
or separately design size tracking, splitting/coalescing and fragmentation
policies; no general heap is implemented here.

## Deterministic coverage

- Layout: independent same-field fixtures; every member/padding byte accounted
  for; actual array stride/alignment; alignas observation; unsorted extents and
  owned names; bad object/member alignment, zero/crossing/overflow/overlap extents.
- Arena: empty, exact fit, one byte over fresh/remaining capacity, unaligned view,
  padding-induced failure, maximum/unsupported/huge alignment and size requests;
  256 mixed allocations across all supported alignments with nonoverlap, marker
  preservation and exact counter checks; reset/reuse; real alignas(256) object;
  counted ordinary, throwing and nested-throwing construction/destruction.
- Pool: invalid configuration leaves every metadata field intact; empty and
  zero-capacity pools; exact fit/one-byte-short/one-byte-extra; huge payload and
  stride overflow; metadata-limited and backing-limited capacity; all supported
  alignments; full exhaustion; invalid-release snapshots; eight varied complete
  release/LIFO-reuse cycles; 1,000 mixed operations preserving other live payloads;
  final recovery of every slot; counted and alignas(256) object lifetimes.
- No `assert`, intentional stale dereference, global-new override, invalid
  standard-resource free, or constructor-failure rewind is used.

## Actual verification evidence

Native environment: macOS **26.6.2** (25G83), Darwin **25.6.0**, **arm64**,
Mach-O; Apple Clang **21.0.0** (`clang-2100.1.1.101`), target
`arm64-apple-darwin25.6.0`, CommandLineTools compiler. C++20/libc++.
Branch/head queries, `c++ --version`, `uname -a`, `sw_vers`, `git remote -v` and
`git diff --check` all exited 0. `git diff --check` does not inspect untracked
files; the core files were also compiled with warnings as errors.

Final raw logs, binaries, exact command arrays, environment, hashes and
source-drift check are in this approved temporary directory:

`/private/var/folders/14/mvcf30lj2bz5nmw28f8nr1ch0000gn/T/opencode/memory-core-final-kpha8s2m/`

Read `verification.json`; per-mode `build.log` and `{layout,arena,pool}.log`
contain full output and exits. No source drift during this run.

| Mode | Additional compiler flags | Build | layout / arena / pool |
| --- | --- | --- | --- |
| dev | `-O0 -g` | passed, exit 0 | passed 3/3, each exit 0 |
| release | `-O3 -DNDEBUG` | passed, exit 0 | passed 3/3, each exit 0 |
| ASan | `-O1 -g -fno-omit-frame-pointer -fsanitize=address` | passed, exit 0 | passed 3/3, each exit 0 |
| UBSan | `-O1 -g -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all` | passed, exit 0 | passed 3/3, each exit 0 |

All builds also used `-std=c++20 -Wall -Wextra -Wpedantic -Werror`.
ASan runs used `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`; UBSan runs used
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

Exact command shape, from the repository root, with `$E` equal to the evidence
directory above, `$mode` a table row and `$FLAGS` its additional flags:

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror $FLAGS \
  -I projects/runtime-inspector/memory/include \
  projects/runtime-inspector/memory/tests/core.cpp \
  -o "$E/$mode/runtime-memory-core"
"$E/$mode/runtime-memory-core" layout
"$E/$mode/runtime-memory-core" arena
"$E/$mode/runtime-memory-core" pool
```

Each of the three headers also passed an independent single-include
`c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I projects/runtime-inspector/memory/include "$E/<name>-standalone.cpp"`
check, exit 0; the tiny sources/logs are in `$E`. Missing and unknown selector
checks returned the expected usage exit **2** on the release binary.

Initial/superseded run evidence is preserved at
`/private/var/folders/14/mvcf30lj2bz5nmw28f8nr1ch0000gn/T/opencode/memory-core-j2ucosax/`.
Dev/release/UBSan passed there, but `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`
aborted all three ASan invocations with signal **6** (subprocess return -6):
`AddressSanitizer: detect_leaks is not supported on this platform.` Those test
bodies were **not run**; that leak-detection mode is **unavailable**, not a pass.
The final ASan results above use this platform's supported no-leak-check mode.
Self-review then parenthesized the checked stride addition to avoid intermediate
unsigned wrap and added explicit over-aligned object lifetime coverage before
the final complete rerun.

### Exact final source/test identity (SHA-256)

Paths below are relative to `projects/runtime-inspector/memory/`:

| Path | SHA-256 |
| --- | --- |
| `include/memory_lab/layout.hpp` | `be08008d9dce7f7ec6f97926fddd63ee65bf998e4973b37c0ef2784394cfdd45` |
| `include/memory_lab/arena.hpp` | `345d84bc6a7a174cb941f8817dca840f24460647091947e858657af7648ce7d0` |
| `include/memory_lab/pool.hpp` | `086c9e14b224172741b4d04104da5ff43302817106ad514b48390fe0f66bd6a2` |
| `tests/core.cpp` | `c50082f138890b6253ffc9ce2df03cd0257d8228d731f2ba71b825c8bab20ff1` |

The test file hash is the public M01–M03 check identity for this slice, not an
immutable private assessment or independently demonstrated learner competence.

## Remaining / next action

Full CMake/report integration and original-product regression suites are **not
run by this slice**; the coordinator owns those gates. Linux/other ABI checks
are **not run** for this new code. ASan leak detection is **unavailable** on this
native target; no timing/benchmark or new CLion demonstration is claimed.
CLion's required interactive gate remains pending. Temporary evidence must be
retained/copied by the coordinator before any external temp cleanup.

**Next action:** coordinator consumes the fixed headers in M04–M05, runs the
registered `memory-layout`, `memory-arena`, `memory-pool` tests and relevant
regressions through integrated dev/release/ASan/UBSan, then records/publishes the
extension identity without moving old release tags.

Coordinator integration follow-up: first actual GCC13.3 Linux CI rejected nine
parenthesized CTAD-span test initializations accepted by native Clang. Those builds
failed and dependent tests were not run (workflow40597197071). Test declarations
were changed to unambiguous braces, without changing allocator implementations.
The source/test hashes above identify the earlier direct slice, not this corrected
test revision; final root versioned matrix manifests are authoritative for it.
