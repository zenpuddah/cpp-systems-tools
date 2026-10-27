import json
from pathlib import Path
import platform
import subprocess
import sys

script, destination, compiler = sys.argv[1:]
result = subprocess.run([sys.executable, script, "--output", destination, "--cxx", compiler,
                         "--size", "101", "--threads", "3", "--repetitions", "1"],
                        capture_output=True, text=True, timeout=110)
assert result.returncode == 0, result.stdout + result.stderr
output = Path(destination)
data = json.loads((output / "report.json").read_text())
assert not data["errors"]
assert data["native_format"] == ("Mach-O / macOS" if platform.system() == "Darwin" else "ELF / Linux")
for filename in ["main.ii", "operations.ii", "main.s", "operations.s", "main.o", "operations.o",
                 "libruntime-example.a", "symbols-defined.txt", "symbols-undefined.txt", "relocations.txt",
                 "dependencies.txt", "disassembly.txt", "sections-segments.txt", "translator-model.json",
                 "workloads.json", "report.md", "process-maps.txt"]:
    assert (output / filename).stat().st_size > 0, filename
failure = next(c for c in data["commands"] if c["name"] == "deliberate-link-failure")
assert failure["status"] == "passed" and failure["exit_code"] != 0
assert data["observations"]["result"] == 1408
layout = data["observations"]["layout"]
assert layout["value_offset"] % 8 == 0 and layout["size"] % layout["alignment"] == 0
assert data["observations"]["vector"]["new_capacity"] > data["observations"]["vector"]["old_capacity"]
print("passed: report workflow, native labels, artifacts, diagnosed link failure, layout, checksums")
# A failed compiler probe must not inspect stale successful artifacts, and
# must still produce a useful report rather than cascading missing-file errors.
failed_output = output / "failed-build"
failed = subprocess.run([sys.executable, script, "--output", str(failed_output),
                         "--cxx", "/definitely/missing/runtime-inspector-cxx"],
                        capture_output=True, text=True, timeout=10)
assert failed.returncode != 0
failure_data = json.loads((failed_output / "report.json").read_text())
assert failure_data["dependent_inspection_and_tests"] == "not run"
assert not any(c["name"].startswith("run-") for c in failure_data["commands"])
print("passed: failed compiler retains diagnostics and does not run dependent tests")
