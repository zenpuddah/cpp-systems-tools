# Verification evidence

Final A1–A3 records:

- `verification.json`: exact project file hashes, separate source/assessment
  identity, root CMake/preset hashes as actually used, native platform/toolchain,
  real compiler commands, and configure/build/CTest commands with exit codes.
- `verification-<preset>-{configure,build,test}.log`: final targeted dev, release,
  ASan and UBSan runs. Each test log reports two registered CTest entries passed.
- `resilience.json` and `resilience-dev-*.log`: the same source and assessment,
  with each registered test repeated successfully 20 times.

Both final records observed no source drift. Logical source and assessment hashes
exclude generated evidence and `progress.md`; the final progress ledger was added
after the tests and does not change the tested source identity. No tests were
rerun merely because the agent/server session restarted.

Unprefixed `<preset>-*.log` files are earlier preliminary passing checks, before
the final additional boundary/compaction tests. They are retained as superseded
development evidence, not mapped to the final assessment identity. Earlier failed
build/fixture results and their fixes are explicitly recorded in `../progress.md`.

Reproduce from the repository root:

```sh
python3 projects/tcp-kv/tests/verify.py
python3 projects/tcp-kv/tests/verify.py dev --repeat 20 --label resilience
```

This script reruns checks only when explicitly invoked and writes project-local
evidence; it does not stage, commit or push anything. These are correctness and
sanitizer results, not benchmarks or independent learner assessments. Linux, IDE
interaction, and the A6 performance investigation were not run by this task.
