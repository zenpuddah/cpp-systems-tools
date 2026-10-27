#!/usr/bin/env python3
"""Inspect only the bundled specimen; standard library, bounded subprocesses."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent


def source_identity() -> str:
    digest = hashlib.sha256()
    for folder in ("specimen", "src", "include", "tests"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                digest.update(str(path.relative_to(ROOT)).encode() + b"\0" + path.read_bytes())
    for name in ("CMakeLists.txt", "report.py"):
        digest.update(name.encode() + b"\0" + (ROOT / name).read_bytes())
    return digest.hexdigest()


class Inspector:
    def __init__(self, output: Path):
        self.output = output
        self.commands = []
        self.errors = []

    def require_build(self):
        build_failed = any(command["status"] != "passed" and
                           (command["name"] in {"compiler", "compiler-target", "archive", "shared-library"}
                            or command["name"].startswith(("build-", "link-")))
                           for command in self.commands)
        if build_failed:
            # Never execute or inspect a stale artifact from an earlier run
            # when this run failed to build its required inputs.
            failure = {"schema": 1, "status": "failed", "source_sha256": source_identity(),
                       "platform": platform.platform(), "machine": platform.machine(),
                       "commands": self.commands, "errors": self.errors,
                       "dependent_inspection_and_tests": "not run"}
            (self.output / "report.json").write_text(json.dumps(failure, indent=2) + "\n")
            (self.output / "report.md").write_text("# Runtime inspector report — failed build\n\n"
                + "\n".join(self.errors) + "\n\nDependent inspection/tests: **not run**. See report.json and command logs.\n")
            raise RuntimeError("build failed; dependent inspections/tests not run; see report.json")

    def run(self, name, command, *, timeout=30, required=False, expect_failure=False):
        start = time.monotonic()
        try:
            with subprocess.Popen([str(v) for v in command], cwd=self.output,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, errors="replace", start_new_session=True) as proc:
                try:
                    stdout, stderr = proc.communicate(timeout=timeout)
                    success = proc.returncode != 0 if expect_failure else proc.returncode == 0
                    status = "passed" if success else "failed"
                except subprocess.TimeoutExpired:
                    # Kill the owned process group, including compiler/debugger
                    # children, not just the launcher that exceeded its deadline.
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    stdout, stderr = proc.communicate(timeout=2)
                    stderr += f"\ndeadline exceeded ({timeout}s); owned process group killed"
                    status = "failed"
                text = stdout + "\n--- stderr ---\n" + stderr
                code = proc.returncode
        except FileNotFoundError as exc:
            text, status, code = str(exc), "unavailable", None
        except subprocess.TimeoutExpired as exc:
            text, status, code = f"deadline exceeded ({timeout}s): {exc}", "failed", None
        # Tool output is finite for this tiny specimen; bound retained logs too.
        truncated = len(text) > 2_000_000
        (self.output / f"{name}.txt").write_text(text[:2_000_000], encoding="utf-8")
        record = {"name": name, "command": [str(v) for v in command], "status": status,
                  "exit_code": code, "seconds": time.monotonic()-start,
                  "expected_failure": expect_failure, "log": f"{name}.txt", "truncated": truncated}
        self.commands.append(record)
        if required and status != "passed":
            self.errors.append(f"{name}: {status}; see {name}.txt")
        return record


def inspect(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    inspector = Inspector(output)
    cxx = shlex.split(args.cxx)
    system = platform.system()
    if system not in ("Linux", "Darwin"):
        raise RuntimeError("native inspection supports Linux and macOS only; PE/DLL is documented comparison")
    native = "Mach-O / macOS" if system == "Darwin" else "ELF / Linux"
    flags = ["-std=c++20", "-O0", "-g", "-fPIC", "-fno-omit-frame-pointer"]
    workload_flags = ["-std=c++20", "-O2", "-g", "-pthread", "-fno-omit-frame-pointer"]
    git = inspector.run("git-revision", ["git", "-C", ROOT, "rev-parse", "HEAD"])
    inspector.run("git-dirty", ["git", "-C", ROOT, "status", "--short"])
    inspector.run("compiler", [*cxx, "--version"], required=True)
    inspector.run("compiler-target", [*cxx, "-dumpmachine"], required=True)
    inspector.require_build()
    if system == "Darwin":
        inspector.run("machine-details", ["sysctl", "machdep.cpu.brand_string", "hw.model", "hw.pagesize"])
    else:
        inspector.run("machine-details", ["lscpu"])
    for unit in ("main", "operations"):
        source = ROOT / "specimen" / f"{unit}.cpp"
        for suffix, option in (("ii", "-E"), ("s", "-S"), ("o", "-c")):
            inspector.run(f"build-{unit}-{suffix}", [*cxx, *flags, option, source, "-o", output / f"{unit}.{suffix}"], required=True)
    inspector.require_build()
    main, operations = output / "main.o", output / "operations.o"
    exe = output / "runtime-specimen"
    inspector.run("link-specimen", [*cxx, main, operations, "-o", exe], required=True)
    inspector.require_build()
    inspector.run("deliberate-link-failure", [*cxx, main, "-o", output / "must-not-link"], required=True, expect_failure=True)
    failure = (output / "deliberate-link-failure.txt").read_text()
    if "specimen_transform" not in failure or "specimen_global" not in failure:
        inspector.errors.append("deliberate link failure did not diagnose both omitted-TU symbols")
    archive = output / "libruntime-example.a"
    inspector.run("archive", ["ar", "rcs", archive, operations], required=True)
    inspector.require_build()
    inspector.run("archive-members", ["ar", "t", archive], required=True)
    inspector.run("link-static-client", [*cxx, main, archive, "-o", output / "static-client"], required=True)
    shared = output / ("libruntime-example.dylib" if system == "Darwin" else "libruntime-example.so")
    shared_options = (["-dynamiclib", "-Wl,-install_name,@rpath/" + shared.name] if system == "Darwin" else ["-shared"])
    inspector.run("shared-library", [*cxx, *shared_options, operations, "-o", shared], required=True)
    rpath = "-Wl,-rpath,@loader_path" if system == "Darwin" else "-Wl,-rpath,$ORIGIN"
    inspector.run("link-shared-client", [*cxx, main, shared, rpath, "-o", output / "shared-client"], required=True)
    inspector.require_build()
    for name in ("runtime-specimen", "static-client", "shared-client"):
        inspector.run(f"run-{name}", [output / name, "42"], required=True)
    inspector.run("file-format", ["file", exe, main, archive, shared], required=True)
    inspector.run("symbols-defined", ["nm", "-g", operations], required=True)
    inspector.run("symbols-undefined", ["nm", "-u", main], required=True)
    if system == "Darwin":
        inspector.run("sections-segments", ["otool", "-l", exe], required=True)
        inspector.run("dependencies", ["otool", "-L", output / "shared-client"], required=True)
        inspector.run("relocations", ["otool", "-rv", main], required=True)
        inspector.run("disassembly", ["otool", "-tvV", exe], required=True)
    else:
        inspector.run("sections-segments", ["readelf", "-h", "-S", "-l", exe], required=True)
        inspector.run("dependencies", ["readelf", "-d", output / "shared-client"], required=True)
        inspector.run("relocations", ["readelf", "-r", main], required=True)
        inspector.run("disassembly", ["objdump", "-drC", exe], required=True)
    disassembly = (output / "disassembly.txt").read_text().splitlines()
    excerpts = []
    for index, line in enumerate(disassembly):
        if "specimen_transform" in line:
            excerpts.extend(disassembly[max(0, index-3):index+18])
    (output / "call-trace.txt").write_text("\n".join(excerpts[:80]) + "\n")
    observations = {}
    try:
        observations = json.loads((output / "run-runtime-specimen.txt").read_text().splitlines()[0])
        for client in ("static-client", "shared-client"):
            candidate = json.loads((output / f"run-{client}.txt").read_text().splitlines()[0])
            if candidate["result"] != observations["result"]:
                inspector.errors.append(f"{client} result mismatch")
    except (ValueError, IndexError, OSError):
        inspector.errors.append("specimen did not emit valid observation JSON")
    # Keep child alive only while reading its maps; even errors always terminate
    # and reap it. A file avoids blocking readline on a failed/slow child.
    live_path = output / "specimen-live.json"
    with live_path.open("w") as live:
        child = None
        try:
            child = subprocess.Popen([exe, "42", "--hold"], stdout=live, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 4
            while live_path.stat().st_size == 0 and child.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            if live_path.stat().st_size == 0:
                raise RuntimeError("specimen maps startup deadline")
            if system == "Linux":
                maps = Path(f"/proc/{child.pid}/maps").read_text()
                (output / "process-maps.txt").write_text(maps)
                inspector.commands.append({"name": "process-maps", "command": ["read", f"/proc/{child.pid}/maps"],
                                           "status": "passed", "exit_code": 0, "log": "process-maps.txt"})
            else:
                inspector.run("process-maps", ["vmmap", str(child.pid)], timeout=8)
                if shutil.which("sample"):
                    inspector.run("sampling-capability", ["sample", str(child.pid), "1", "-file", output / "sample-profile.txt"], timeout=5)
        except (OSError, RuntimeError) as exc:
            inspector.errors.append(f"process maps: {exc}")
        finally:
            if child is not None:
                if child.poll() is None:
                    child.terminate()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=2)

    # Compile timings separately, explicitly optimized and NOT instrumented.
    workloads = output / "runtime-workloads"
    inspector.run("build-workloads", [*cxx, *workload_flags, "-I", ROOT / "include",
                                      ROOT / "src/workloads.cpp", ROOT / "src/main.cpp", "-o", workloads], required=True)
    inspector.require_build()
    inspector.run("workloads", [workloads, "--size", str(args.size), "--threads", str(args.threads),
                                 "--seed", str(args.seed), "--repetitions", str(args.repetitions)], required=True)
    workload = None
    summaries = {}
    try:
        workload = json.loads((output / "workloads.txt").read_text().splitlines()[0])
        mask = (1 << 64)-1
        expected = sum(((((i ^ args.seed)*0x9e3779b97f4a7c15 + args.seed) & mask) >> 17)
                       for i in range(args.size)) & mask
        if workload["expected"] != expected or any(s["checksum"] != expected for s in workload["samples"]):
            raise ValueError("independent checksum mismatch")
        (output / "workloads.json").write_text(json.dumps(workload, indent=2) + "\n")
        for mode in ("aos", "soa", "shared", "adjacent", "spaced", "local"):
            samples = sorted(s["seconds"] for s in workload["samples"] if s["variant"] == mode)
            summaries[mode] = {"samples": len(samples), "median_seconds": statistics.median(samples),
                               "min_seconds": min(samples), "max_seconds": max(samples)}
    except (OSError, ValueError, KeyError, IndexError) as exc:
        inspector.errors.append(f"workload validation: {exc}")

    capabilities = {name: shutil.which(name) for name in ("perf", "strace", "lldb", "gdb", "vmmap", "sample")}
    # Probes distinguish installed binaries from usable kernel/ptrace access.
    if capabilities["perf"]:
        inspector.run("perf-stat", ["perf", "stat", "-e", "task-clock,cycles,instructions", exe, "42"], timeout=10)
        inspector.run("perf-record", ["perf", "record", "-o", output / "perf.data", "--", exe, "42"], timeout=10)
        inspector.run("perf-report", ["perf", "report", "--stdio", "-i", output / "perf.data"], timeout=10)
    if capabilities["strace"]:
        inspector.run("strace", ["strace", "-c", str(exe), "42"], timeout=10)
    if capabilities["lldb"]:
        inspector.run("debugger-capability", ["lldb", "--batch", "-o", "target create " + shlex.quote(str(exe)),
                                               "-o", "breakpoint set -n specimen_transform", "-o", "run 42", "-o", "bt"], timeout=12)
    elif capabilities["gdb"]:
        inspector.run("debugger-capability", ["gdb", "--batch", "-ex", "break specimen_transform", "-ex", "run 42", "-ex", "bt", exe], timeout=12)
    model = {"label": "toy page translator MODEL, not observed physical addresses", "page_bytes": 4096,
             "virtual_address": 0x12345, "virtual_page": 0x12, "offset": 0x345,
             "invented_page_table": {"18": 128}, "invented_physical_address": 0x80345}
    (output / "translator-model.json").write_text(json.dumps(model, indent=2) + "\n")
    meta = {"schema": 1, "utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "native_format": native, "machine": platform.machine(), "platform": platform.platform(),
            "cpu_count": os.cpu_count(), "compiler": cxx, "specimen_flags": flags,
            "workload_flags": workload_flags, "source_sha256": source_identity(),
            "git_revision_status": git["status"], "commands": inspector.commands,
            "capabilities": capabilities, "observations": observations, "workload_summary": summaries,
            "errors": inspector.errors, "translator_model": model}
    (output / "report.json").write_text(json.dumps(meta, indent=2) + "\n")
    write_markdown(output, meta)
    return 1 if inspector.errors else 0


def write_markdown(output, meta):
    obs = meta["observations"]
    abi = ("AArch64: first uint64 arguments x0/x1, return x0; `bl` calls and `ret` returns."
           if meta["machine"] in ("arm64", "aarch64") else
           "x86-64 System V: first uint64 arguments rdi/rsi, return rax; call/ret."
           if meta["machine"] in ("x86_64", "AMD64") else
           "ABI annotation unavailable for this ISA; inspect assembly directly.")
    lines = ["# Runtime inspector report", "", f"Native observation: **{meta['native_format']}**, {meta['machine']}.",
             f"Source/test identity SHA-256: `{meta['source_sha256']}`; Git revision and dirty state in linked logs.",
             "", "## Observed build → link → load → memory", "",
             "- `main.ii` / `operations.ii`: real expanded includes/macros; `.s`: compiler assembly; `.o`: relocatable objects.",
             "- Missing operations.o deliberately fails: specimen_transform and specimen_global are undefined. See deliberate-link-failure.txt.",
             "- Static archive members are selected at link time; shared-client resolves a runtime dependency via its local rpath.",
             "- symbols-*.txt / relocations.txt show actual symbol references and relocation records before final address resolution.",
             "- sections-segments.txt and dependencies.txt show native structure and dynamic dependencies; process-maps.txt shows one live specimen.",
             f"- Observed object size/alignment/offsets: `{obs.get('layout', {})}`.",
             f"- Vector observation: `{obs.get('vector', {})}`. Capacity was exceeded; all old element pointers/references/iterators are invalidated, regardless of address coincidence.",
             "- Addresses in run-runtime-specimen.txt are virtual, ASLR-dependent, not physical locations. Fault counters are cumulative since startup, not per-line causality.",
             "", "## Actual call / ABI trace", "", abi,
             "At -O0, main passes input and salt=5 to specimen_transform; the second TU multiplies by 33 and adds salt and global=17, returning 1408 for input=42.",
             "Find specimen_transform in main.s, operations.s and disassembly.txt: object-level unresolved call relocation becomes a linked branch/call. Function prologue/spills and exact instruction selection are observations, not C++ guarantees.", ""]
    if meta["native_format"].startswith("Mach-O"):
        lines += ["Mach-O is not ELF: otool load commands describe __TEXT/__DATA segments and their sections plus dylib loads; dyld handles loading/fixups. No Linux ELF verification is implied."]
    else:
        lines += ["ELF section headers describe link/debug organization; program headers describe mapped PT_LOAD segments and permissions (not one mapping per section). The kernel loads the image/interpreter; the dynamic loader resolves dependencies/relocations before runtime initialization and main."]
    lines += ["", "## CPU/concurrency question", "",
              "Question: on this machine/input, does private accumulation reduce elapsed aggregation time versus a contended atomic, without changing the modulo-2^64 checksum?",
              "Baseline shared; candidate 1 spaced per-thread atomics; candidate 2 local accumulation plus final reduction. Adjacent is a diagnostic control; AoS/SoA is a separate layout comparison. No product changes are selected automatically.",
              "The hypothesis is reduced shared-write coordination. Timing alone cannot establish cache-line/false-sharing causes; unavailable perf counters remain unavailable. Setup/thread creation/join are excluded; gate/reduction costs are included for aggregation. One warm-up per variant, rotating measurement order.",
              "", "| Variant | Samples | Median seconds | Min | Max |", "|---|---:|---:|---:|---:|"]
    for name, summary in meta["workload_summary"].items():
        lines.append(f"| {name} | {summary['samples']} | {summary['median_seconds']:.9g} | {summary['min_seconds']:.9g} | {summary['max_seconds']:.9g} |")
    summaries = meta["workload_summary"]
    if all(mode in summaries for mode in ("shared", "spaced", "local")):
        baseline = summaries["shared"]["median_seconds"]
        ratios = {mode: baseline/summaries[mode]["median_seconds"] if summaries[mode]["median_seconds"] > 0 else None
                  for mode in ("spaced", "local")}
        lines += ["", f"Observed median ratios (baseline / candidate): {ratios}. None means below clock resolution. These are sample-specific ratios, not general speedup guarantees.",
                  "Conclusion: retain all variants as an experiment; no server/design change is warranted by these samples. Repeat on the target workload and use supported counters before assigning a hardware cause."]
    lines += ["", "## Models and limits", "",
              "translator-model.json invents a page table/frame: VPN 0x12 + offset 0x345 maps to frame 0x80, physical 0x80345. It is not a physical-address measurement. See README/architecture for MMU/TLB, segmentation, faults, COW and PE/DLL comparison. PGM remains unresolved.",
              "No intentional protection failure is executed. No Linux/Windows results or universal race-freedom claims follow from macOS results.",
              "", "## Commands and capability outcomes", "", "| Step | Status | Exit | Evidence |", "|---|---|---:|---|"]
    for command in meta["commands"]:
        lines.append(f"| {command['name']} | {command['status']} | {command.get('exit_code')} | [{command['log']}]({command['log']}) |")
    for name, path in meta["capabilities"].items():
        lines.append(f"- {name}: {'installed at ' + path if path else 'unavailable (not on PATH)'}")
    lines += ["", "Errors: " + ("; ".join(meta["errors"]) or "none"), "",
              "Raw samples/checksums: workloads.json. Exact commands, compiler flags, OS/ISA, identities and statuses: report.json. Timing runs are separate from sanitizer tests."]
    lines += ["", "## Actual linked call excerpt", "", "```text",
              (output / "call-trace.txt").read_text().rstrip(), "```",
              "The optional sample profile is a capability probe of the held specimen (normally sleeping), not evidence of a CPU-workload bottleneck."]
    (output / "report.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--size", type=int, default=200000)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    if not (0 <= args.size <= 10000000 and 1 <= args.threads <= 32 and
            0 <= args.seed < (1 << 64) and 1 <= args.repetitions <= 30):
        parser.error("workload outside bounded input limits")
    try:
        status = inspect(args)
    except (OSError, RuntimeError) as exc:
        print(f"runtime-inspector: {exc}", file=sys.stderr)
        return 1
    print(f"{'passed' if status == 0 else 'failed'}: {args.output.resolve() / 'report.md'}")
    return status


if __name__ == "__main__":
    sys.exit(main())
