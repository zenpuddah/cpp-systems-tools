# Implementation progress

## A0 — discovery and plan (2026-10-27)

Independent local repository initialized on main; no prior target directories/remotes existed. Hosting authentication checked: zenpuddah, requested repository names absent. Native macOS 26.6.2 / Darwin 25.6.0 / ARM64, Apple Clang 21.0.0, CMake 4.4.3, Python 3.14.8, Ninja installed. CLion 2026.2.1 installed; interactive workflow not run. perf/strace/container runtimes absent; LLDB available. Git identity exists, no signing override introduced. Source baseline: workspace 0c44a68a490035999aa3525e45f4601c883407f2. Discovery commands exited 0 except expected missing-repository queries. Product tests: not run yet. PLAN.md fixes files/interfaces/checks and delegation ownership.

Next action: hand over the verified public revision, pin it and derive the independent private workbook without copying private data back into this repository.

## Evidence tooling slice

Root configure/build/test presets parse with `cmake --list-presets` (exit 0). Latest `python3 tests/test_scripts.py`: seven checks passed (exit 0), including failure/deadline/unavailable status, actual JUnit counts/empty-suite rejection, deterministic source manifest, public allowlist and percentile boundaries. Capability output is `evidence/capabilities.json`; LLDB launch failed due to macOS attach policy and xctrace failed because full Xcode is absent. GitHub repository created and queried: https://github.com/zenpuddah/cpp-systems-tools, PUBLIC (`evidence/hosting.json`), not yet a verified published release. Full integrated product tests and Linux CI not run yet. No private repository created yet.

The newly created remote initially used SSH from the gh preference. `git ls-remote origin` failed (128: host key verification); no SSH trust/security setting was changed. Switched this new repository's remote to HTTPS; read-only `git ls-remote origin` passed (0, empty remote). Publishing will use the existing authenticated gh HTTPS credential helper for that invocation, without changing global credentials.

## Restart recovery

User reported a server restart. Existing uncommitted tools files preserved; workspace discovery commit remains 20d9437. Resumed the same exclusively owned component sessions, not replacement implementations. Server `projects/tcp-kv/evidence/verification.json` retains source/test manifests and passed dev/release/ASan/UBSan configure/build plus two CTest cases (unit/integration) per preset; source drift false for that recorded run. Inspector sources/report/tests present, final evidence pending. No finished check was repeated solely because of the restart. Main integration measurement wrappers prepared; combined preset gate remains next after component finalization.

## A1–A6 — verified public handoff candidate

Product commit `377205a5280dcad79ff3ecbd5efc6c5624f6382b`, main, pushed to checked PUBLIC origin. Both component ledgers contain completed slices and immutable source/test identities. Coordinator reviewed parser/store, FD ownership, bounded turn loop, queue closure/wakeup and workload timing/publication. Integrated `python3 scripts/verify.py dev release asan ubsan tsan` exited 0: dev/release/ASan/UBSan **7/7 each**, TSan concurrency **3/3**, native macOS ARM64 Clang 21. Full source manifest `64f3a1d2e1e5d9e30840636438b827f7a6087ba22a3134e265b3c63e93ea5363`; drift false.

Ubuntu 24.04 x86-64 GCC 13.3 workflow **40573727465** passed all five presets with the same counts. `python3 scripts/import_ci.py 40573727465` exited 0, retaining actual platform, compiler, commands, exits and test identities in `evidence/linux-ci.json` / `evidence/linux/`. Reference Linux execution is no longer unverified. Underlying CI host ISA/counters not established beyond runner metadata.

`python3 scripts/investigate_server.py --requests 256 --repetitions 5` and `python3 scripts/investigate_cpu.py --size 200000 --threads 4 --seed 42 --repetitions 7` exited 0. Zero server errors and independently matched CPU checksums; raw samples/flags/binary identities retained. Separate post-measurement release CTest passed **7/7**, exit 0. Public allowlist audit passed. See docs/RESULTS.md for actual observations and limits, not a manufactured optimization claim. No public code changes were selected from timings. IDE interaction remains not run; no learner assessment performed. Release evidence/docs are committed separately from the tested product source.
