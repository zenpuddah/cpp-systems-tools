#!/usr/bin/env python3
"""Versioned extension evidence; never overwrite public-v1 verification files."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil

from verify import ROOT, command, identity, test_results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('presets', nargs='*', default=['dev', 'release', 'asan', 'ubsan', 'tsan'])
    ns = ap.parse_args()
    allowed = {'dev', 'release', 'asan', 'ubsan', 'tsan'}
    if not ns.presets or any(p not in allowed for p in ns.presets):
        ap.error('expected one or more known build presets')
    os.environ.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    os.environ.setdefault('ASAN_OPTIONS', 'halt_on_error=1' + (':detect_leaks=0' if platform.system() == 'Darwin' else ''))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    family = 'linux' if platform.system() == 'Linux' else 'native'
    base = ROOT / 'evidence/memory-v1' / family
    evidence = base / stamp
    evidence.mkdir(parents=True)
    before = identity()
    data = {'schema': 1, 'extension': 'memory-v1', 'source': before,
            'platform': platform.platform(), 'machine': platform.machine(),
            'compiler': command(['c++', '--version']), 'cmake': command(['cmake', '--version']),
            'python': platform.python_version(), 'revision': command(['git', 'rev-parse', 'HEAD']),
            'dirty': command(['git', 'status', '--porcelain']), 'presets': {},
            'sanitizer_environment': {k: os.environ.get(k) for k in ('ASAN_OPTIONS', 'UBSAN_OPTIONS', 'TSAN_OPTIONS')}}
    for preset in ns.presets:
        junit = ROOT / 'build' / preset / 'memory-ctest.xml'
        if junit.exists():
            junit.unlink()
        steps = []
        data['presets'][preset] = steps
        for argv in [['cmake', '--preset', preset], ['cmake', '--build', '--preset', preset, '-j', '4'],
                     ['ctest', '--preset', preset, '--output-on-failure', '--no-tests=error', '--output-junit', str(junit)]]:
            if steps and steps[-1]['status'] != 'passed':
                steps.append({'command': argv, 'status': 'not run', 'exit_code': None})
                continue
            print(' '.join(argv), flush=True)
            result = command(argv, timeout=360)
            steps.append(result)
            print(result['output'], flush=True)
            if argv[0] == 'ctest':
                result['tests'] = test_results(junit)
                if result['tests']['status'] != 'passed':
                    result['status'] = 'failed'
                if junit.exists():
                    shutil.copyfile(junit, evidence / f'{preset}-tests.xml')
        flags = ROOT / 'build' / preset / 'compile_commands.json'
        if flags.exists():
            shutil.copyfile(flags, evidence / f'{preset}-compile-commands.json')
        (evidence / 'verification.json').write_text(json.dumps(data, indent=2) + '\n')
    data['source_after'] = identity()
    data['source_drift'] = before != data['source_after']
    data['status'] = 'passed' if not data['source_drift'] and all(s['status'] == 'passed' for v in data['presets'].values() for s in v) else 'failed'
    (evidence / 'verification.json').write_text(json.dumps(data, indent=2) + '\n')
    tmp = base / 'latest.tmp'
    tmp.write_text(json.dumps({'record': str((evidence / 'verification.json').relative_to(ROOT)),
                               'status': data['status'], 'source_sha256': before['sha256']}, indent=2) + '\n')
    os.replace(tmp, base / 'latest.json')
    print(evidence / 'verification.json')
    return int(data['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
