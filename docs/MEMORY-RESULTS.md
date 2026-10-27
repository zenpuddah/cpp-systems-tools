# Memory-v1 verified result

Product/tested revision: **350a0e979dce3ef188b38466dbc7e0ce69d84ae1**. Subsequent release receipts add documentation/evidence only. Original `public-v1` remains 7d0759ca9535ea10d056400a9db435212c4d67c8.

| Actual environment | dev | release | ASan | UBSan | TSan concurrency |
|---|---:|---:|---:|---:|---:|
| macOS26.6.2 ARM64, AppleClang21 | 15/15 | 15/15 | 15/15 | 15/15 | 3/3 |
| Ubuntu24.04 x86-64, GCC13.3/libstdc++ | 15/15 | 15/15 | 15/15 | 15/15 | 3/3 |

All configure/build/test steps exited0. Exact commands, OS/compiler, full source/test file hashes, compile commands and JUnit:
- `evidence/memory-v1/native/20261027T225153.085866Z/verification.json`, source SHA256 `7919c541c97eefc63ae650a54a17a1978b57146706eea992a5fae9756d31a109`, drift false.
- Actual successful [Linux workflow40597984542](https://github.com/zenpuddah/cpp-systems-tools/actions/runs/40597984542), immutable imported artifacts `evidence/memory-v1/ci/40597984542/`; no source drift.

Primary corrected optimized investigation, run separately after correctness checks: `evidence/memory-v1/investigation/20261027T225302.460015Z/report.md`, with `memory.json`, `report.json`, `measurement.json` and raw command logs. Seven repetitions, size128/seed42; all five routes agree on payload/checksums/destructor obligations. The report declares exact timing boundaries, natural object alignment, observed counters and unknown internals. No universal speedup or host-heap reconstruction is claimed. Original server/numerical/queue sources/tests and release evidence are unchanged.

Two earlier Linux failures are preserved:40597197071 was a GCC test-initialization build failure (dependent tests not run);40597511941 caught known libstdc++ PR118681 pool misalignment before object construction. Corrected cost experiment uses natural object alignment, preserves identical request sizes across all backends and keeps pointer checks. Separate custom allocator tests cover alignments through256. Earlier native timing samples are historical, not relabeled as the corrected experiment.

Reproduce: `python3 scripts/verify_memory.py dev release asan ubsan tsan` and, separately, `python3 scripts/measure_memory.py`. [Guide and policies](MEMORY.md). Native ASan leak detection is unavailable (`detect_leaks=0` recorded); Linux sanitizer results are actual runner results, not native emulation. CLion interactive demonstration remains **not run**. Implementation checks are not independent learner assessment.
