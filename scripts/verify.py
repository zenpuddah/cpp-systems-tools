#!/usr/bin/env python3
"""Retain reproducible build/test evidence without calling a failed build a test run."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def identity():
    files = {}
    for p in sorted(ROOT.rglob('*')):
        if not p.is_file() or any(x in {'.git', 'build', 'reports', '__pycache__', 'evidence'} for x in p.relative_to(ROOT).parts):
            continue
        if p.name == 'progress.md':
            continue
        files[p.relative_to(ROOT).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {'sha256': digest, 'files': files}


def command(args, timeout=240):
    started = time.monotonic()
    try:
        p = subprocess.Popen(args, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, start_new_session=True)
        try:
            output, _ = p.communicate(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
            os.killpg(p.pid, signal.SIGKILL)
            output, _ = p.communicate()
            return {'command': args, 'exit_code': p.returncode, 'status': 'failed',
                    'seconds': time.monotonic() - started, 'output': output,
                    'reason': 'deadline' if isinstance(exc, subprocess.TimeoutExpired) else 'cancelled'}
        return {'command': args, 'exit_code': p.returncode, 'status': 'passed' if p.returncode == 0 else 'failed',
                'seconds': time.monotonic() - started, 'output': output}
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {'command': args, 'exit_code': None, 'status': 'failed', 'seconds': time.monotonic() - started,
                'output': str(exc)}


def test_results(path):
    if not path.is_file():
        return {'status': 'unavailable', 'reason': 'CTest did not produce a JUnit result'}
    try:
        suite = ET.parse(path).getroot()
        cases = []
        for case in suite.iter('testcase'):
            state = 'failed' if case.find('failure') is not None or case.find('error') is not None else 'passed'
            if case.find('skipped') is not None or case.get('status') in {'notrun', 'disabled'}:
                state = 'not run'
            cases.append({'name': case.get('name'), 'status': state, 'seconds': case.get('time')})
        return {'status': 'passed' if cases and all(c['status'] == 'passed' for c in cases) else 'failed',
                'count': len(cases), 'passed': sum(c['status'] == 'passed' for c in cases), 'cases': cases}
    except ET.ParseError as exc:
        return {'status': 'failed', 'reason': str(exc)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('presets', nargs='*', default=['dev', 'release', 'asan', 'ubsan', 'tsan'])
    ns = ap.parse_args()
    output = ROOT / 'evidence'
    output.mkdir(exist_ok=True)
    report = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'platform': platform.platform(), 'machine': platform.machine(), 'python': platform.python_version(),
              'source': identity(), 'git': command(['git', 'rev-parse', 'HEAD']),
              'dirty': command(['git', 'status', '--porcelain']), 'compiler': command(['c++', '--version']),
              'cmake': command(['cmake', '--version']), 'presets': {}}
    for preset in ns.presets:
        steps = []
        junit = ROOT / 'build' / preset / 'ctest-results.xml'
        for argv in [['cmake', '--preset', preset], ['cmake', '--build', '--preset', preset, '-j', '4'],
                     ['ctest', '--preset', preset, '--output-on-failure', '--no-tests=error', '--output-junit', str(junit)]]:
            if steps and steps[-1]['status'] != 'passed':
                steps.append({'command': argv, 'status': 'not run', 'exit_code': None})
                continue
            print(' '.join(argv), flush=True)
            if argv[0] == 'ctest' and junit.exists():
                junit.unlink()  # Only our generated build result, never a source/user file.
            steps.append(command(argv))
            if argv[0] == 'ctest':
                steps[-1]['tests'] = test_results(junit)
                if steps[-1]['tests']['status'] != 'passed':
                    steps[-1]['status'] = 'failed'
            print(steps[-1]['output'], flush=True)
        report['presets'][preset] = steps
        path = output / 'verification.json'
        tmp = path.with_suffix('.tmp')
        tmp.write_text(json.dumps(report, indent=2) + '\n')
        os.replace(tmp, path)
    report['source_after'] = identity()
    report['source_drift'] = report['source'] != report['source_after']
    (output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(any(s['status'] != 'passed' for steps in report['presets'].values() for s in steps))


if __name__ == '__main__':
    raise SystemExit(main())
