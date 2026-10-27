# Public tools

Follow ../cpp-systems-workspace/specs/TECHNICAL-CONTRACT.md. Public technical code/evidence only; no learner/author records. C++20, CMake >=3.25, Python stdlib. Preserve configured Git policies. Record actual checks, not intended passes.

Delegated ownership: server agent owns only projects/tcp-kv/**; inspector agent owns only projects/runtime-inspector/**. Coordinator owns root build, docs, scripts, evidence, progress and integration. Do not commit shared work from child agents.

Memory extension ownership supersedes the original task roles only for this delta: core agent owns memory/include/memory_lab/{layout,arena,pool}.hpp, memory/tests/core.cpp and memory/CORE-NOTES.md; experiment agent owns memory/include/memory_lab/{pmr,experiments}.hpp, memory/src/{pmr,experiments,main}.cpp, memory/tests/{pmr,costs}.cpp and memory/EXPERIMENT-NOTES.md. Paths are relative to projects/runtime-inspector. Coordinator owns all CMake/report integration, remaining docs/evidence/progress and publication. Read MEMORY_PLAN.md/MEMORY_API.md and the authoritative sibling specs/MEMORY-ALLOCATION.md. No original server/workload/queue edits or private records here.
