# Runtime inspector report

Native observation: **Mach-O / macOS**, arm64.
Source/test identity SHA-256: `4b17a0084a2f31d787bc3dc8f9711cfbed307533b04984009f6b662fe5a030c7`; Git revision and dirty state in linked logs.

## Observed build → link → load → memory

- `main.ii` / `operations.ii`: real expanded includes/macros; `.s`: compiler assembly; `.o`: relocatable objects.
- Missing operations.o deliberately fails: specimen_transform and specimen_global are undefined. See deliberate-link-failure.txt.
- Static archive members are selected at link time; shared-client resolves a runtime dependency via its local rpath.
- symbols-*.txt / relocations.txt show actual symbol references and relocation records before final address resolution.
- sections-segments.txt and dependencies.txt show native structure and dynamic dependencies; process-maps.txt shows one live specimen.
- Observed object size/alignment/offsets: `{'size': 24, 'alignment': 8, 'tag_offset': 0, 'value_offset': 8, 'count_offset': 16}`.
- Vector observation: `{'old_capacity': 2, 'new_capacity': 4, 'old_address': 4343863424, 'new_address': 4343863440}`. Capacity was exceeded; all old element pointers/references/iterators are invalidated, regardless of address coincidence.
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
| aos | 7 | 0.000212583 | 0.000186542 | 0.000289834 |
| soa | 7 | 2.9791e-05 | 2.7708e-05 | 3.2e-05 |
| shared | 7 | 0.003609084 | 0.002656833 | 0.004273916 |
| adjacent | 7 | 0.003321292 | 0.002306209 | 0.003686834 |
| spaced | 7 | 0.000511208 | 0.000497208 | 0.000634125 |
| local | 7 | 4.0375e-05 | 3.5583e-05 | 6.8125e-05 |

Observed median ratios (baseline / candidate): {'spaced': 7.059912990407036, 'local': 89.38907739938081}. None means below clock resolution. These are sample-specific ratios, not general speedup guarantees.
Conclusion: retain all variants as an experiment; no server/design change is warranted by these samples. Repeat on the target workload and use supported counters before assigning a hardware cause.

## Models and limits

translator-model.json invents a page table/frame: VPN 0x12 + offset 0x345 maps to frame 0x80, physical 0x80345. It is not a physical-address measurement. See README/architecture for MMU/TLB, segmentation, faults, COW and PE/DLL comparison. PGM remains unresolved.
No intentional protection failure is executed. No Linux/Windows results or universal race-freedom claims follow from macOS results.

## Commands and capability outcomes

| Step | Status | Exit | Evidence |
|---|---|---:|---|
| git-revision | failed | 128 | [git-revision.txt](git-revision.txt) |
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
