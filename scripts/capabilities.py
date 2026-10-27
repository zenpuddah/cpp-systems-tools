#!/usr/bin/env python3
"""Read-only capability probes. Presence is not proof of a successful profiling run."""
import datetime
import json
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def probe(argv):
    if shutil.which(argv[0]) is None:
        return {'command': argv, 'status': 'unavailable', 'reason': 'executable not on PATH'}
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=15)
        return {'command': argv, 'status': 'passed' if p.returncode == 0 else 'failed',
                'exit_code': p.returncode, 'output': (p.stdout + p.stderr)[:16000]}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {'command': argv, 'status': 'unavailable', 'reason': str(exc)}


def main():
    clion = {'status': 'not run', 'installed_version': None}
    plist = Path('/Applications/CLion.app/Contents/Info.plist')
    if plist.is_file():
        with plist.open('rb') as stream:
            clion['installed_version'] = plistlib.load(stream).get('CFBundleShortVersionString')
    else:
        clion['reason'] = 'CLion not found at native macOS application path; no GUI demonstration'
    result = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'platform': platform.platform(), 'isa': platform.machine(),
              'probes': [probe(a) for a in [
                  ['c++', '--version'], ['cmake', '--version'], ['python3', '--version'],
                  ['ninja', '--version'], ['perf', 'stat', 'true'], ['perf', 'record', '-o', str(ROOT / 'reports/perf.data'), '--', 'true'],
                  ['perf', 'report', '-i', str(ROOT / 'reports/perf.data'), '--stdio'], ['strace', '-c', 'true'], ['lldb', '--version'],
                  ['lldb', '--batch', '-o', 'run', '--', '/usr/bin/true'],
                  ['xctrace', 'list', 'templates'], ['docker', 'version'], ['podman', 'version']]],
              'linux_native_tests': {'status': 'unavailable' if platform.system() != 'Linux' else 'not run'},
              'clion_interactive': clion}
    out = ROOT / 'evidence/capabilities.json'
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + '\n')
    print(out)


if __name__ == '__main__':
    (ROOT / 'reports').mkdir(exist_ok=True)
    main()
