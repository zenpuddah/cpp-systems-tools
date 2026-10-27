#!/usr/bin/env python3
"""Record targeted server checks and exact project identities, without commits."""
import argparse
import hashlib
import json
import pathlib
import platform
import subprocess
import sys
import time


def digest(entries):
    return hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()


def manifest(project, root):
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(project.rglob("*"))
        if path.is_file()
        and "evidence" not in path.relative_to(project).parts
        and "__pycache__" not in path.parts
        and path.name != "progress.md"
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("presets", nargs="*", default=["dev", "release", "asan", "ubsan"])
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--label", choices=["verification", "resilience"], default="verification")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 30:
        parser.error("repeat must be in 1..30")
    project = pathlib.Path(__file__).resolve().parents[1]
    root = project.parents[1]
    evidence = project / "evidence"
    evidence.mkdir(exist_ok=True)
    before = manifest(project, root)
    source = {p: h for p, h in before.items() if "/src/" in p or "/include/" in p or p.endswith("CMakeLists.txt")}
    assessment = {p: h for p, h in before.items() if "/tests/" in p}
    configuration = {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest()
        for name in ("CMakeLists.txt", "CMakePresets.json")
    }
    report = {
        "platform": platform.platform(), "machine": platform.machine(), "python": sys.version,
        "manifest": before, "source_sha256": digest(source),
        "assessment_sha256": digest(assessment), "manifest_sha256": digest(before),
        "root_build_configuration": configuration, "runs": [],
    }
    for name, command in [("compiler", ["clang++", "--version"]), ("cmake", ["cmake", "--version"])]:
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=10)
        report[name] = {"command": command, "exit": result.returncode, "output": result.stdout + result.stderr}
    for preset in args.presets:
        run = {"preset": preset, "stages": []}
        report["runs"].append(run)
        stages = [
            ("configure", ["cmake", "--preset", preset]),
            ("build", ["cmake", "--build", "--preset", preset, "--target", "tcp-kv", "tcp-kv-unit"]),
            ("test", ["ctest", "--preset", preset, "-R", "^tcp-kv-", "--output-on-failure",
                      "--repeat", f"until-fail:{args.repeat}"]),
        ]
        okay = True
        for stage, command in stages:
            entry = {"stage": stage, "command": command, "state": "not run"}
            run["stages"].append(entry)
            if not okay:
                continue
            start = time.monotonic()
            try:
                result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                        timeout=max(120, args.repeat * 10))
                output = result.stdout + result.stderr
                entry.update(exit=result.returncode, state="passed" if result.returncode == 0 else "failed")
            except subprocess.TimeoutExpired as error:
                output = str(error)
                entry.update(exit=None, state="failed", reason="timeout")
            entry["elapsed_seconds"] = time.monotonic() - start
            log = evidence / f"{args.label}-{preset}-{stage}.log"
            log.write_text(output)
            entry["log"] = str(log.relative_to(project))
            okay = entry["state"] == "passed"
            print(preset, stage, entry["state"], entry.get("exit"), flush=True)
            if not okay:
                print(output, flush=True)
        commands = root / "build" / preset / "compile_commands.json"
        if commands.exists():
            run["compile_commands"] = [
                command for command in json.loads(commands.read_text())
                if str(project) in command["file"]
            ]
    report["source_drift"] = manifest(project, root) != before
    (evidence / f"{args.label}.json").write_text(json.dumps(report, indent=2) + "\n")
    print("source", report["source_sha256"])
    print("assessment", report["assessment_sha256"])
    return 0 if not report["source_drift"] and all(
        stage["state"] == "passed" for run in report["runs"] for stage in run["stages"]
    ) else 1


if __name__ == "__main__":
    sys.exit(main())
