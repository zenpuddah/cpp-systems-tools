# Runtime inspector report

Native observation: **Mach-O / macOS**, arm64.
Source/test identity SHA-256: `af79ff550e1c660e8f9bb1b9d8d229d27808f0a47d8818c4b0a17ed71c45e496`; Git revision and dirty state in linked logs.

## Observed build → link → load → memory

- `main.ii` / `operations.ii`: real expanded includes/macros; `.s`: compiler assembly; `.o`: relocatable objects.
- Missing operations.o deliberately fails: specimen_transform and specimen_global are undefined. See deliberate-link-failure.txt.
- Static archive members are selected at link time; shared-client resolves a runtime dependency via its local rpath.
- symbols-*.txt / relocations.txt show actual symbol references and relocation records before final address resolution.
- sections-segments.txt and dependencies.txt show native structure and dynamic dependencies; process-maps.txt shows one live specimen.
- Observed object size/alignment/offsets: `{'size': 24, 'alignment': 8, 'tag_offset': 0, 'value_offset': 8, 'count_offset': 16}`.
- Vector observation: `{'old_capacity': 2, 'new_capacity': 4, 'old_address': 4317551264, 'new_address': 4317551280}`. Capacity was exceeded; all old element pointers/references/iterators are invalidated, regardless of address coincidence.
- Addresses in run-runtime-specimen.txt are virtual, ASLR-dependent, not physical locations. Fault counters are cumulative since startup, not per-line causality.

## Actual call / ABI trace

AArch64: first uint64 arguments x0/x1, return x0; `bl` calls and `ret` returns.
At -O0, main passes input and salt=5 to specimen_transform; the second TU multiplies by 33 and adds salt and global=17, returning 1408 for input=42.
Find specimen_transform in main.s, operations.s and disassembly.txt: object-level unresolved call relocation becomes a linked branch/call. Function prologue/spills and exact instruction selection are observations, not C++ guarantees.

Mach-O is not ELF: otool load commands describe __TEXT/__DATA segments and their sections plus dylib loads; dyld handles loading/fixups. No Linux ELF verification is implied.

## CPU/concurrency question

Question: on this machine/input, does private accumulation reduce elapsed aggregation time versus a contended atomic, without changing the modulo-2^64 checksum?
Baseline shared; candidate 1 spaced per-thread atomics; candidate 2 local accumulation plus final reduction. Adjacent is a diagnostic control; AoS/SoA is a separate layout comparison. No product changes are selected automatically.
The hypothesis is reduced shared-write coordination. Timing alone cannot establish cache-line/false-sharing causes; unavailable perf counters remain unavailable. Setup/thread creation/join are excluded; gate/reduction costs are included for aggregation. One warm-up per variant, rotating measurement order.

| Variant | Samples | Median seconds | Min | Max |
|---|---:|---:|---:|---:|
| aos | 7 | 0.000236458 | 0.000125208 | 0.000667791 |
| soa | 7 | 2.7584e-05 | 1.7042e-05 | 4.0291e-05 |
| shared | 7 | 0.0017915 | 0.001166875 | 0.002742833 |
| adjacent | 7 | 0.001541458 | 0.001335833 | 0.001752208 |
| spaced | 7 | 0.000137333 | 0.000110042 | 0.000356791 |
| local | 7 | 3.1083e-05 | 2.8959e-05 | 0.000121917 |

Observed median ratios (baseline / candidate): {'spaced': 13.044934575083918, 'local': 57.63600682044847}. None means below clock resolution. These are sample-specific ratios, not general speedup guarantees.
Conclusion: retain all variants as an experiment; no server/design change is warranted by these samples. Repeat on the target workload and use supported counters before assigning a hardware cause.

## Layout, caller-buffer allocation and PMR

M01–M05 extend this Inspector, not the server. `memory.json` retains actual standard-layout member/padding maps, arena/pool policies, PMR front-end/upstream counters, bounded allocation samples and a labeled free-interval model.
Member alignment, allocation alignment, object padding and cache-line spacing are different quantities. Padding is not semantic initialized data; never serialize a struct memory image or use padding in memcmp equality.
Arena reset/monotonic release reclaim storage collectively, not object lifetimes. Destroy dependent objects/containers before release; clear alone is insufficient. Fixed-slot checked releases are not standard-PMR invalid-deallocation tests.
Known payload, backing capacity, metadata/rounding/padding and retained storage are separate from unobservable host allocator internals or PMR cursors. Unknowns must not be inferred from RSS. Resource choices have no guaranteed speedup.
[Full measured memory report and raw samples](memory.json). See memory.txt and build-memory.txt for the actual optimized uninstrumented run/flags.

### Measured layout (active ABI)

| Fixture | Size / stride | Alignment | Internal / tail padding | Members: offset + size |
|---|---:|---:|---:|---|
| original | 24 / 24 | 8 | 7 / 4 | label: 0 + 1, value: 8 + 8, count: 16 + 4 |
| reordered | 16 / 16 | 8 | 0 / 3 | label: 12 + 1, value: 0 + 8, count: 8 + 4 |
| alignas(64) | 64 / 64 | 64 | 7 / 44 | label: 0 + 1, value: 8 + 8, count: 16 + 4 |

Computed byte maps: member initials; `.` internal padding; `T` tail padding. No padding values are read.

- original: `l.......vvvvvvvvccccTTTT`
- reordered: `vvvvvvvvcccclTTT`
- alignas(64): `l.......vvvvvvvvccccTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT`
Observed gap: 7 bytes at offset 1 precede value at aligned offset 8 (member alignment 8).
Observed original tail: 4 bytes; size/stride 24 is a multiple of object alignment 8, keeping successive array elements aligned. Neither this padding total nor reordering savings is universal.

Caller-buffer arena demonstration: consumed 94 = payload 38 + alignment padding 56; remaining 929 / capacity 1023. Reset consumption 0.
Fixed pool demonstration: 4 slots, payload 24, stride 32; front gap 15, tail 112, separate metadata used/reserved 64/64 bytes. Final live/free 0/4.

### Observed PMR resource routes

| Route | Frontend successes / failures | Upstream successes / failures | Upstream peak bytes | Before release bytes | Final upstream bytes |
|---|---:|---:|---:|---:|---:|
| monotonic_fallback | 4 / 0 | 2 / 0 | 5184 | 5184 | 0 |
| monotonic_hardcap | 3 / 1 | 0 / 1 | 0 | 0 | 0 |
| pool | 9 / 0 | 8 / 0 | 1312 | 632 | 0 |

Hard-cap bad_alloc observed: True. Copy/move resource-selection fixtures: {'copy_uses_default': True, 'explicit_copy_uses_destination': True, 'move_uses_source': True, 'explicit_move_uses_destination': True, 'move_assignment_keeps_destination': True, 'fixture_ok': True}. All counted objects were destroyed before release.

### Controlled allocation question

For the same simultaneous-live payload and bulk lifetime, what does each instrumented allocation path cost, and what storage remains before collective reclamation? No causal or universal speedup claim follows.
Size 128; seed 42; repetitions 7; runtime trace and all raw samples retained in memory.json. Times below are medians in **nanoseconds**; setup/warmup, payload touch/checksum and teardown are separate. PMR counters/dispatch and checked pool-free validation costs are included, not subtracted.

| Backend | Raw allocation | Raw free | Reset/release | Object construction | Object destruction |
|---|---:|---:|---:|---:|---:|
| new_delete_resource | 3084 | 3500 | unsupported | 208 | 209 |
| bump_arena | 1417 | unsupported | 42 | 167 | 167 |
| fixed_pool | 500 | 5584 | unsupported | 167 | 167 |
| pmr_monotonic | 958 | 541 | 41 | 167 | 167 |
| pmr_pool | 3084 | 16709 | 42 | 167 | 167 |

Storage below is the **first raw sample**, bytes. Consumed is observed arena cursor/live-slot span or host client-visible payload, never inferred host footprint. Retained includes unused caller capacity; metadata is separate. `unknown` is not zero.

| Backend | Requested payload | Backing capacity | Acquired upstream peak | Consumed | Alignment gaps | Slot rounding | Custom metadata | Retained after client lifetime |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| new_delete_resource | 5149 | 0 | 5149 | 5149 | unknown | 0 | unknown | 0 |
| bump_arena | 5149 | 16896 | 0 | 6095 | 946 | 0 | 40 | 16896 |
| fixed_pool | 5149 | 16896 | 0 | 8192 | 0 | 3043 | 2160 | 16896 |
| pmr_monotonic | 5149 | 16896 | 0 | unknown | unknown | unknown | unknown | 16896 |
| pmr_pool | 5149 | 0 | 8648 | unknown | unknown | unknown | unknown | 0 |

Free-interval **model**, not libc heap: total free 32, largest hole 16, request 24 fails despite sufficient sum. Adjacent-hole coalescing changes total/largest 48/16 → 48/48; the request then succeeds.
Conclusion: arena/monotonic allocations retain dead payload until collective reclamation; fixed-slot rounding is internal loss; free fixed slots are not automatically external fragmentation. Standard-pool retention and host internals remain library-specific/unknown. Choose by lifetime, bounds and supported shape, not this small timing ranking.

## Models and limits

translator-model.json invents a page table/frame: VPN 0x12 + offset 0x345 maps to frame 0x80, physical 0x80345. It is not a physical-address measurement. See README/architecture for MMU/TLB, segmentation, faults, COW and PE/DLL comparison. PGM remains unresolved.
No intentional protection failure is executed. No Linux/Windows results or universal race-freedom claims follow from macOS results.

## Commands and capability outcomes

| Step | Status | Exit | Evidence |
|---|---|---:|---|
| git-revision | passed | 0 | [git-revision.txt](git-revision.txt) |
| git-dirty | passed | 0 | [git-dirty.txt](git-dirty.txt) |
| compiler | passed | 0 | [compiler.txt](compiler.txt) |
| compiler-target | passed | 0 | [compiler-target.txt](compiler-target.txt) |
| machine-details | passed | 0 | [machine-details.txt](machine-details.txt) |
| build-main-ii | passed | 0 | [build-main-ii.txt](build-main-ii.txt) |
| build-main-s | passed | 0 | [build-main-s.txt](build-main-s.txt) |
| build-main-o | passed | 0 | [build-main-o.txt](build-main-o.txt) |
| build-operations-ii | passed | 0 | [build-operations-ii.txt](build-operations-ii.txt) |
| build-operations-s | passed | 0 | [build-operations-s.txt](build-operations-s.txt) |
| build-operations-o | passed | 0 | [build-operations-o.txt](build-operations-o.txt) |
| link-specimen | passed | 0 | [link-specimen.txt](link-specimen.txt) |
| deliberate-link-failure | passed | 1 | [deliberate-link-failure.txt](deliberate-link-failure.txt) |
| archive | passed | 0 | [archive.txt](archive.txt) |
| archive-members | passed | 0 | [archive-members.txt](archive-members.txt) |
| link-static-client | passed | 0 | [link-static-client.txt](link-static-client.txt) |
| shared-library | passed | 0 | [shared-library.txt](shared-library.txt) |
| link-shared-client | passed | 0 | [link-shared-client.txt](link-shared-client.txt) |
| run-runtime-specimen | passed | 0 | [run-runtime-specimen.txt](run-runtime-specimen.txt) |
| run-static-client | passed | 0 | [run-static-client.txt](run-static-client.txt) |
| run-shared-client | passed | 0 | [run-shared-client.txt](run-shared-client.txt) |
| file-format | passed | 0 | [file-format.txt](file-format.txt) |
| symbols-defined | passed | 0 | [symbols-defined.txt](symbols-defined.txt) |
| symbols-undefined | passed | 0 | [symbols-undefined.txt](symbols-undefined.txt) |
| sections-segments | passed | 0 | [sections-segments.txt](sections-segments.txt) |
| dependencies | passed | 0 | [dependencies.txt](dependencies.txt) |
| relocations | passed | 0 | [relocations.txt](relocations.txt) |
| disassembly | passed | 0 | [disassembly.txt](disassembly.txt) |
| process-maps | passed | 0 | [process-maps.txt](process-maps.txt) |
| sampling-capability | passed | 0 | [sampling-capability.txt](sampling-capability.txt) |
| build-workloads | passed | 0 | [build-workloads.txt](build-workloads.txt) |
| workloads | passed | 0 | [workloads.txt](workloads.txt) |
| build-memory | passed | 0 | [build-memory.txt](build-memory.txt) |
| memory | passed | 0 | [memory.txt](memory.txt) |
| validate-memory | passed | 0 | [validate-memory.txt](validate-memory.txt) |
| debugger-capability | passed | 0 | [debugger-capability.txt](debugger-capability.txt) |
- perf: unavailable (not on PATH)
- strace: unavailable (not on PATH)
- lldb: installed at /usr/bin/lldb
- gdb: unavailable (not on PATH)
- vmmap: installed at /usr/bin/vmmap
- sample: installed at /usr/bin/sample

Errors: none

Raw samples/checksums: workloads.json. Exact commands, compiler flags, OS/ISA, identities and statuses: report.json. Timing runs are separate from sanitizer tests.

## Actual linked call excerpt

```text
000000010000064c	str	x9, [x8, #0x50]
0000000100000650	ldr	x0, [x8, #0x58]
0000000100000654	mov	x1, #0x5
0000000100000658	bl	_specimen_transform
000000010000065c	ldr	x9, [sp, #0x170]
0000000100000660	mov	x8, x0
0000000100000664	sub	x0, x29, #0x40
0000000100000668	str	x8, [x9, #0x40]
000000010000066c	sub	x8, x29, #0x38
0000000100000670	bl	__ZNSt3__111make_uniqueB9nqe210106IyJyELi0EEENS_10unique_ptrIT_NS_14default_deleteIS2_EEEEDpOT0_
0000000100000674	sub	x0, x29, #0x58
0000000100000678	str	x0, [sp, #0x160]
000000010000067c	bl	__ZNSt3__16vectorIyNS_9allocatorIyEEEC1B9nqe210106Ev
0000000100000680	ldr	x0, [sp, #0x160]
0000000100000684	mov	x1, #0x2
0000000100000688	bl	__ZNSt3__16vectorIyNS_9allocatorIyEEE7reserveEm
000000010000068c	b	0x100000690
0000000100000690	sub	x0, x29, #0x58
0000000100000694	sub	x1, x29, #0x28
0000000100000698	bl	__ZNSt3__16vectorIyNS_9allocatorIyEEE9push_backB9nqe210106ERKy
000000010000069c	b	0x1000006a0
__ZNSt3__123__libcpp_numeric_limitsIxLb1EE3maxB9nqe210106Ev:
00000001000036a4	mov	x0, #0x7fffffffffffffff
00000001000036a8	ret
_specimen_transform:
00000001000036ac	sub	sp, sp, #0x10
00000001000036b0	str	x0, [sp, #0x8]
00000001000036b4	str	x1, [sp]
00000001000036b8	ldr	x8, [sp, #0x8]
00000001000036bc	mov	x9, #0x21
00000001000036c0	mul	x8, x8, x9
00000001000036c4	ldr	x9, [sp]
00000001000036c8	add	x8, x8, x9
00000001000036cc	adrp	x9, 5 ; 0x100008000
00000001000036d0	ldr	x9, [x9]
00000001000036d4	add	x0, x8, x9
00000001000036d8	add	sp, sp, #0x10
00000001000036dc	ret

--- stderr ---
```
The optional sample profile is a capability probe of the held specimen (normally sleeping), not evidence of a CPU-workload bottleneck.
