# Runtime inspector implementation progress

Updated 2026-10-27. **A4–A5 implemented and verified locally on macOS.** Changes are confined to `projects/runtime-inspector/**`; no root edits or commits were made by this delegated implementation. Repository main has no commit yet (`git rev-parse HEAD` exit 128); no revision was invented. Publication/integrated release belongs to the coordinator. These are implementation checks, not independent learner-competence evidence.

## Delivered interfaces

- CMake targets: `runtime-specimen`, `runtime-example-static`, `runtime-example-shared`, `runtime-workloads`, `runtime-tests` (plus internal `runtime-workload-lib`). C++20; root supplies Threads, Python, sanitizers and CTest.
- One run-to-report command, from repository root:

  ```sh
  python3 projects/runtime-inspector/report.py --output reports/runtime
  ```

  Creates real two-TU preprocess/assembly/object artifacts, symbols, relocations, controlled missing-TU link failure, working archive/shared clients, native dependencies/disassembly/maps, observed layout/vector/addresses, ABI excerpt, labeled page-translation model and validated workload samples. Optional debugger/profiling limitations are explicit; required build failures retain diagnostics and stop dependent inspections/tests rather than execute stale artifacts. Every subprocess has a deadline and owned-process cleanup.
- `runtime-workloads --size N --threads N --seed N --repetitions N` emits JSON for AoS/SoA/shared/adjacent/spaced/local variants. Setup/warmup are outside retained timings; aggregation includes gates/reduction but excludes thread creation/joins. Defined uint64 arithmetic and checksums; independent Python fixtures include empty/uneven/maximal-seed/sum-overflow cases. Zero-duration observations retain zero with a null rate.
- `runtime-tests core|queue`; CTest `runtime-core`, `runtime-queue`, `runtime-workloads-oracle`, `runtime-report`. First three are labeled `concurrency`; report is `inspection`. Throwing checks stay active in Release. Queue tests prove capacity, owned-item lifetime, exact once, reject/drain, idempotent close, blocked-producer/consumer wakeups and joined shutdown for empty/full/uneven cases.
- `README.md` and `architecture.md`: public purpose/quick start/output/architecture/results/limits, language lifetime/ownership/layout, loader/sections/segments, MMU/TLB/segmentation/paging/faults/COW, Windows PE/DLL documented comparison and unresolved `PGM`.

## Actual environment and identities

Native macOS 26.6.2, arm64, **Apple M5 Pro / Mac17,9**, native page size **16,384 bytes**, Apple Clang 21.0.0 (`clang-2100.1.1.101`), target `arm64-apple-darwin25.6.0`; Python 3.14.8, CMake 4.4.3/Ninja. Native format is **Mach-O**, not ELF. The translator's 4096-byte page size is an invented model parameter, not this machine's observed page size.

- Complete configure/build/test matrix source/test SHA-256: `8abe903eb5c44a2d739d3a0bbc107f47fa4818f5cdb912ac4eed8d8b7c856b4b`, recorded in `evidence/verification/results.json`.
- Final report/inspection source/test SHA-256: **`4b17a0084a2f31d787bc3dc8f9711cfbed307533b04984009f6b662fe5a030c7`**, in `evidence/native/report.json` and `evidence/verification/final-report-check.json`. Only `report.py` changed after the full matrix (build-failure classification and native machine-details probe); the report test and native report were rerun afterward. No C++ or assessment-test changes occurred after the passing matrix.
- Per-file immutable source/assessment identities are in `evidence/verification/final-report-check.json` (`file_sha256`), including the independent Python workload oracle (`5803e883f4aa7b4ad73d8bcc4085df83594a2027616156fb9d91d4c3914f5079`), C++ tests (`99eaa6e7a99cc5ad092e6e87806c5a35e2a4a1367a951d5849a626af7ffd87b0`) and report assessment (`1cfc454730ac71ec5ce0bdb7d3581690758ca0f5c68e70d76d58f4ffedacc8dc`). Documentation/evidence files are intentionally outside this implementation identity.

## Verification results (all commands from repository root)

For each preset below, actual configure command was `cmake --preset <preset>`; build command was:

```sh
cmake --build --preset <preset> --target runtime-specimen runtime-example-static runtime-example-shared runtime-workloads runtime-tests
```

Every configure/build completed with exit **0**. Exact argv and separate logs are in `evidence/verification/results.json` and `<preset>-{configure,build,test}.txt`.

| Check | Actual command | Result |
|---|---|---|
| Debug | `ctest --preset dev -R '^runtime-' --output-on-failure` | **passed**, 4/4, exit 0 |
| Release | `ctest --preset release -R '^runtime-' --output-on-failure` | **passed**, 4/4, exit 0 |
| AddressSanitizer | `ctest --preset asan -R '^runtime-' --output-on-failure` | **passed**, 4/4, exit 0 |
| UndefinedBehaviorSanitizer | `ctest --preset ubsan -R '^runtime-' --output-on-failure` | **passed**, 4/4, exit 0 |
| ThreadSanitizer, separate | `ctest --preset tsan -R '^runtime-' --output-on-failure -L concurrency` | **passed**, 3/3, exit 0 |
| Final Python-only report review | `ctest --preset release -R '^runtime-report$' --output-on-failure` | **passed**, 1/1, exit 0 |
| Native report / optimized measurements | `python3 projects/runtime-inspector/report.py --output projects/runtime-inspector/evidence/native --size 200000 --threads 4 --seed 42 --repetitions 7` | **passed**, exit 0; no required errors |

Sanitizer runs instrument the CMake core/queue/oracle executables. The report test deliberately compiles its separately inspected native artifacts without sanitizer flags; it is not a sanitizer pass for those freshly compiled artifacts. Retained performance samples come from a separate optimized, uninstrumented run, after verification, not from the sanitizer tests. No intentional memory-protection crash demonstration is present.

### Failures found and preserved

1. Initial release run: 2/4 failed, exit 8, because empty/tiny traversal duration can be zero at native clock resolution. Corrected the numerical policy to retain zero and output `below_clock_resolution` plus `items_per_second=null`; added explicit zero-rate serialization coverage. Queue and inspection already passed. This was not a workload-result mismatch.
2. First TSan matrix: 1/3 passed, exit 8; TSan warned about plain local-result publication across the library barrier. No suppression or sanitizer disabling was used. Changed to one release/acquire atomic publication per worker, retaining private non-atomic arithmetic inside the hot loop; all three concurrent tests then passed. Original failing source/test identity and output remain in `evidence/verification/first-pass.json` and `tsan-initial-failure.txt` (also the original `tsan.txt`). This record does not claim whether the original warning was a library-instrumentation limitation or establish universal race freedom.
3. An early optional LLDB probe exceeded its 12-second deadline. Final debugger probe completed successfully (exit 0), producing an actual specimen_transform breakpoint/backtrace in `evidence/native/debugger-capability.txt`. Optional tool deadlines remain enforced, now with owned process-group cleanup.

## Native inspection / bounded CPU investigation

Retained `evidence/native/report.md` / `report.json` / `workloads.json` include actual commands, flags, environment, seed, repetitions, all 42 samples/checksums and errors. Compiler flags: specimen `-std=c++20 -O0 -g -fPIC -fno-omit-frame-pointer`; workloads `-std=c++20 -O2 -g -pthread -fno-omit-frame-pointer`. Inspected artifact binaries/preprocessed sources remain local and ignored; text logs, raw samples, the explicit model and report are retained for public evidence. Rerun the report to regenerate all artifacts.

Question: does private aggregation reduce observed elapsed time against the shared-atomic baseline on this input/machine? Two candidates only: spaced atomics and local accumulation; adjacent slots are a diagnostic control. All variants match the independently calculated reference sum. Final seven-sample medians (seconds): AoS **0.000212583**, SoA **0.000029791**, shared **0.003609084**, adjacent **0.003321292**, spaced **0.000511208**, local **0.000040375**. Min/max/raw data are retained. Timing order rotates, but frequency, placement and background activity are uncontrolled; this is a small observation, not a general speedup or causal cache-line proof. No product/server optimization selected; retain readable variants and remeasure on the intended target if needed.

`nm`, `otool`, `ar`, `file`, `vmmap`, and `sample` probes **passed**; deliberate omitted-TU link returned expected exit **1** and diagnosed both missing symbols. LLDB final breakpoint/backtrace **passed**. `perf`, `strace` and `gdb` are **unavailable** on PATH. Sampling is a probe of the held specimen (normally sleeping), not CPU-hotspot evidence. Linux ELF/perf/strace and Windows PE/DLL execution are **not run**; their reference route/comparison is **documented only**. CLion interactive demonstration is **not run**. No physical-address measurement, hardware-counter attribution, Linux result or independent learner ability is claimed.

## One next action

Coordinator: include the inspector in the integrated public A6 verification/release and record the resulting repository commit/publication state in the root ledger. No remaining A4–A5 implementation blocker; Linux reference execution remains an explicit platform-verification limitation.
