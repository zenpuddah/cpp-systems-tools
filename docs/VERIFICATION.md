# Reproducing verification

```sh
python3 scripts/capabilities.py
python3 scripts/verify.py dev release asan ubsan tsan
```

Each preset has configure, build and test entries. `tsan` filters the real tests labeled `concurrency`; it is not combined with ASan. Tests use explicit runtime checks, not disabled Release `assert`. Normal/sanitizer suites contain no intentional protection-fault demo. A source-file SHA256 manifest accompanies each run; source drift is reported. Raw CTest details are in `build/<preset>/Testing/Temporary/`.

## Observed host

macOS 26.6.2 / ARM64, Apple Clang 21.0.0, CMake 4.4.3, Python 3.14.8. `evidence/capabilities.json` contains exact outputs: perf/strace/container runtimes absent; LLDB executable/version available but launch of the system `/usr/bin/true` denied by macOS attach policy; xctrace requires full Xcode (only Command Line Tools active). These are capability failures/unavailable checks, not evidence about tool correctness. CLion 2026.2.1 installed; no interactive demonstration yet.

## Linux reference route

Use Ubuntu 24.04 x86-64 or ARM64 with `build-essential clang cmake ninja-build python3 binutils strace linux-tools-generic gdb` (distribution/kernel packages may vary). On a Mac, use an existing Linux VM, recording **both** host and guest ISA; no VM or paid infrastructure was installed here. Run the same presets inside the guest. Do not compare VM timings to native timings as an ISA-only difference. `perf` may require guest PMU support and administrator policy; record permission errors verbatim rather than changing host security policy.

The public GitHub Actions workflow runs the preset matrix on Ubuntu, uploads evidence even on failure, and is a reproducible Linux route. Workflow definition alone is **documented only**, until actual workflow results are recorded in progress.md. See that ledger and checked workflow/artifact identities for status.

## Evidence boundaries

`passed`, `failed`, `unavailable`, `not run`, `documented only` are distinct. Compiler success alone is not a test pass. TSan success does not prove race freedom. Source/build/test flags, raw samples and errors are preserved; no benchmark is inferred from sanitizer execution. Reports are observations of this specimen/toolchain, not portable ABI guarantees.
