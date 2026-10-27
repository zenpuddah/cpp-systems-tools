# TCP server implementation progress

Updated 2026-10-27. **A1–A3 complete and verified locally.** Only
`projects/tcp-kv/**` source/documentation/evidence was created or edited; no root
files, Git index, commits, remotes or unrelated projects were modified. Root
CMake/presets supplied by the coordinator were used once available; no temporary
wrapper was needed. Discovery found no existing server implementation or nested
AGENTS instructions. The tools repository was on unborn `main`, with no remotes;
publication is not claimed.

## Delivered interface

- Targets `tcp-kv`, `tcp-kv-unit` and internal static library `tcp-kv-core`.
- Executable: `build/<preset>/projects/tcp-kv/tcp-kv [--port 0..65535]`.
  Default 5555, IPv4 127.0.0.1 only, startup `READY <actual-port>`.
- Registered tests: `tcp-kv-unit` and `tcp-kv-integration` (20-/90-second outer
  CTest timeouts, respectively; process/socket operations also have deadlines).
- `include/tcp_kv/protocol.hpp`: pure `parse(frame)` and bounded `Store`.
- `include/tcp_kv/fd.hpp`: move-only descriptor ownership.
- `include/tcp_kv/connection.hpp`: injectable `Transport`, bounded `Connection`
  turns, inspectable turn statistics, and steady-clock deadlines.
- Exact error precedence/recovery, limits and EOF/signal policy in `README.md`;
  reactor and state diagrams in `docs/architecture.md`.

## Completed slices and review

| Slice | State | Evidence |
| --- | --- | --- |
| A1 parser/store | passed | ASCII/grammar/error precedence, 64/1,024/4,096-byte boundaries, all byte values, 1,024 keys and legal replacement at capacity |
| A2 ownership/I/O | passed | FD move/destruction, every split point of representative frames, exact injected short-write offsets and subsequent append compaction, EINTR/EAGAIN/fatal read/write outcomes, half-close/reset/reconnect |
| A3 resilience | passed | 16-command/16-KiB/32-call budgets, exact 8-KiB input and 64-KiB output boundaries and overflow, 32 clients, buffered work without new arrivals, slow-reader/fair-client progress, EOF/tail/discard recovery, injected exact two-second deadlines and process shutdown |

Self-review checked consumed-prefix accounting, the last permitted buffer byte,
overlong-line discard without a second EOF error, command-budget resume after
EOF, output offset preservation when a new reply is appended, explicit accepted
socket O_NONBLOCK/FD_CLOEXEC, SIGPIPE suppression, accept-attempt bounds, readiness
interest, earlier EOF versus shutdown deadlines, and deadline-aware poll timeouts.
The owning thread is the only owner of the store and connection state; no worker
pool or private/learner material is included.

## Exact verification identity

The source and tests were uncommitted delegated work, identified by content rather
than an invented commit. `evidence/verification.json` contains per-file SHA-256,
the actual root build configuration hashes, compiler commands/flags, platform,
commands, exit codes, durations and log paths. `evidence/resilience.json` records
the repeated checks with the **same** source and assessment identities.

- Source SHA-256: `6ed0d50571bdcd7959b0666b110a92576095bfb3289049a88983f2ad6fd131aa`.
- Test/assessment SHA-256: `fa274467b9405b3ca92614370eec30e26d31a11ea70d3ba6320aa56b325964ea`.
- Both reports recorded `source_drift: false`.
- Environment: native macOS 26.6.2, Darwin 25.6.0, ARM64/Mach-O; Apple Clang
  21.0.0 (`clang-2100.1.1.101`), CMake 4.4.3, Python 3.14.8, Ninja.
- Release compiler commands include `-O3 -DNDEBUG`; the custom test checks remain
  active and the direct release unit invocation passed all six unit groups.

Commands were executed from the tools repository root:

```sh
python3 projects/tcp-kv/tests/verify.py
python3 projects/tcp-kv/tests/verify.py dev --repeat 20 --label resilience
./build/release/projects/tcp-kv/tcp-kv-unit
```

The verification script executes these commands for each preset P:

```sh
cmake --preset P
cmake --build --preset P --target tcp-kv tcp-kv-unit
ctest --preset P -R '^tcp-kv-' --output-on-failure --repeat until-fail:1
```

| Configuration/check | Configure | Build | Tests | Evidence |
| --- | --- | --- | --- | --- |
| dev | passed, exit 0 | passed, exit 0 | 2/2 CTest entries passed, exit 0 | `evidence/verification-dev-*.log` |
| release | passed, exit 0 | passed, exit 0 | 2/2 passed, exit 0 | `evidence/verification-release-*.log` |
| ASan | passed, exit 0 | passed, exit 0 | 2/2 passed, exit 0 | `evidence/verification-asan-*.log` |
| UBSan | passed, exit 0 | passed, exit 0 | 2/2 passed, exit 0 | `evidence/verification-ubsan-*.log` |
| dev repeated | passed, exit 0 | passed, exit 0 | each of 2 entries passed 20 times; exit 0 | `evidence/resilience-dev-*.log` |
| release unit direct | n/a | existing verified build | 6/6 groups passed, exit 0 | execution transcript; unit code uses checks independent of NDEBUG |

The two registered entries contain six C++ unit groups and eight Python process
scenarios. The repeated command used `--repeat until-fail:20`, not an empty suite.
All sanitizer checks here are actual native macOS runs, not Linux extrapolations.

### Earlier failures retained as development history

1. Initial `cmake --preset dev` passed; initial target build failed, exit 1:
   macOS `sigemptyset` is a macro and rejected the leading `::`. Dependent tests
   were **not run**. Corrected the portable invocation; also removed a self-move
   test warning without dropping the self-move check.
2. The subsequent build passed. Initial targeted CTest exited 8: integration
   passed, unit failed one parser fixture (5/6 groups). Its NUL-byte string length
   accidentally included the C terminator after LF. Corrected 9 to 8 bytes and
   improved case diagnostics. Final matrix/repeated checks above supersede this
   failure. These first diagnostics are in the execution transcript, not falsely
   presented as saved raw log files.
3. Earlier unprefixed `evidence/{dev,release,asan,ubsan}-*.log` files are preliminary
   passing checks before the final exact-boundary/compaction test additions;
   use the `verification-*` and `resilience-*` records for the final identity.

## Limits and remaining phase boundary

- Linux: unavailable/not run in this native macOS environment; no Linux pass
  claimed. Linux POSIX route is documented by the repository coordinator.
- TSan: not run for this single-threaded server; concurrent queue verification
  belongs to the Inspector/coordinator.
- CLion interaction: not run by this delegated task; diagrams/docs are not an IDE
  demonstration.
- Server performance investigation: not run here; A6 coordinator-owned release
  work. No throughput/speedup/service-time claim is made from correctness timing.
- Remote publication and full combined-project suite: coordinator-owned pending
  release gates; targeted server results are complete and not a claim about
  unrelated targets.
- This is implementation verification, not evidence of independent learner skill.

## One next action

Coordinator: integrate this unchanged server candidate into A6 combined release
verification and the bounded server performance investigation; retain the exact
source/test identities above and update the root/workspace ledgers.
