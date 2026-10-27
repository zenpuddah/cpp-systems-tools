# Verified public release — 2026-10-27

Product/source commit tested: `377205a5280dcad79ff3ecbd5efc6c5624f6382b`. Subsequent release-evidence/documentation commits do not change product sources or assessment tests. Exact source file manifest SHA-256 for the integrated native run: `64f3a1d2e1e5d9e30840636438b827f7a6087ba22a3134e265b3c63e93ea5363`; source drift **false**. Repository visibility checked **PUBLIC**: https://github.com/zenpuddah/cpp-systems-tools.

| Actual environment | dev | release | ASan | UBSan | TSan concurrency |
|---|---|---|---|---|---|
| macOS 26.6.2 ARM64, Apple Clang 21 | passed 7/7 | passed 7/7 | passed 7/7 | passed 7/7 | passed 3/3 |
| Ubuntu 24.04 x86-64, GCC 13.3 | passed 7/7 | passed 7/7 | passed 7/7 | passed 7/7 | passed 3/3 |

Commands: `python3 scripts/verify.py dev release asan ubsan tsan`; CI runs the same script separately per preset. All configure/build/test exits **0**. Actual Linux workflow: https://github.com/zenpuddah/cpp-systems-tools/actions/runs/40573727465, head revision above, conclusion **success**. Linux guest kernels differ by runner; exact platform/compiler per preset are retained, not normalized into one invented machine. These GitHub runners are Linux VMs; host ISA/counters beyond runner metadata were not established. Native Mac observations are Mach-O; Linux report tests exercise ELF.

Evidence: `evidence/verification.json`, `evidence/linux-ci.json`, `evidence/linux/{dev,release,asan,ubsan,tsan}.json`, per-preset capability probes and original component evidence. The seven CTest entries include seven internal evidence-automation checks, server unit groups/integration, runtime core/queue/independent workload oracle and native report workflow. The report compiles its inspected artifacts separately without sanitizer flags; sanitizer claims apply to instrumented CMake executables, not those fresh artifacts.

## Measurements and correctness recheck

Release binary, seed 0, five repetitions of 256 GETs per client configuration: serial median **25,657.4 requests/s**, batch-16 median **540,892.3 requests/s**. All replies checked, zero errors. Raw request issue/completion times, RTT distributions/sample counts, checksums and exact binary/compile/source identities: `evidence/server-investigation.json`. This is a closed-loop workload comparison; batch latency includes position within the batch. No server service-time measurement or causal syscall explanation exists. No server source optimization selected.

Release CPU workload, seed 42, 200,000 items, four threads, seven repetitions/variant; all 42 checksums match the independent Python integer oracle. Median seconds: shared **0.002127833**, spaced **0.000109792**, local **0.000029416**; adjacent control **0.001347333**, AoS **0.000114541**, SoA **0.000014375**. `evidence/cpu-investigation.json` retains raw data, timing boundaries, flags and identities. These observations are not universal speedups or proof of cache-line causality. Two existing alternatives inspected; no performance-driven source modification. Setup/thread startup excluded as documented.

After measurements, `ctest --preset release --output-on-failure` passed **7/7**, exit **0**. See `evidence/post-measurement.json`. No sanitizer timing was used as performance evidence.

## Limits

Native perf/strace absent; full Xcode/xctrace unavailable. System-binary LLDB probe failed macOS attach policy, while the inspector's owned specimen breakpoint/backtrace succeeded (separate exact evidence). Counters remain unavailable/not inferred. CI reports its own capability errors. Node-20 action deprecation warnings are nonfatal (GitHub ran the actions on Node 24); no warning was counted as a product test.

CLion 2026.2.1 installed and current official integration docs checked, but no interactive IDE workflow has been demonstrated. That private-phase gate remains pending. Product implementation results are not evidence of independent learner competence.
