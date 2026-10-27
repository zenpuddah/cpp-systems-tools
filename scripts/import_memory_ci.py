#!/usr/bin/env python3
"""Import immutable memory-v1 CI evidence without touching original releases."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PRESETS = ('dev', 'release', 'asan', 'ubsan', 'tsan')


def validate(data, preset, revision):
    if data.get('extension') != 'memory-v1' or data.get('machine') not in ('x86_64', 'aarch64'):
        raise ValueError('not recognized Linux memory extension evidence')
    if not data.get('platform', '').startswith('Linux-'):
        raise ValueError('not actual Linux evidence')
    if data.get('revision', {}).get('output', '').strip() != revision:
        raise ValueError('artifact revision does not match the workflow')
    if data.get('source_drift') is not False or data.get('source') != data.get('source_after'):
        raise ValueError('artifact source drift or missing identity')
    steps = data.get('presets', {}).get(preset)
    if not steps or len(steps) != 3:
        raise ValueError('missing configure/build/test results')
    passed = all(s.get('status') == 'passed' and s.get('exit_code') == 0 for s in steps)
    tests = steps[-1].get('tests', {})
    passed = passed and tests.get('status') == 'passed' and tests.get('count', 0) > 0
    return 'passed' if passed and data.get('status') == 'passed' else 'failed'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_id', type=int)
    ns = ap.parse_args()
    workflow = json.loads(subprocess.check_output(
        ['gh', 'run', 'view', str(ns.run_id), '--json', 'conclusion,headSha,url,jobs,status'], cwd=ROOT, text=True))
    if workflow['status'] != 'completed':
        ap.error('workflow is not completed')
    target = ROOT / 'reports' / f'memory-ci-{ns.run_id}'
    target.mkdir(parents=True, exist_ok=True)
    subprocess.run(['gh', 'run', 'download', str(ns.run_id), '--dir', str(target)], cwd=ROOT, check=True)
    evidence = ROOT / 'evidence/memory-v1/ci' / str(ns.run_id)
    if evidence.exists():
        ap.error('run already imported; existing immutable evidence will not be overwritten')
    evidence.mkdir(parents=True)
    result = {'schema': 1, 'workflow': workflow, 'presets': {}}
    for preset in PRESETS:
        artifact = target / f'linux-{preset}-evidence' / 'evidence'
        records = list((artifact / 'memory-v1/linux').glob('*/verification.json'))
        if len(records) != 1:
            result['presets'][preset] = {'status': 'unavailable', 'reason': 'expected one extension artifact'}
            continue
        record = records[0]
        data = json.loads(record.read_text())
        try:
            status = validate(data, preset, workflow['headSha'])
        except ValueError as exc:
            result['presets'][preset] = {'status': 'failed', 'reason': str(exc)}
            continue
        destination = evidence / preset
        shutil.copytree(record.parent, destination)
        capabilities = artifact / 'capabilities.json'
        if capabilities.is_file():
            shutil.copyfile(capabilities, destination / 'capabilities.json')
        result['presets'][preset] = {'status': status, 'source_sha256': data['source']['sha256'],
                                      'record': str((destination / 'verification.json').relative_to(ROOT))}
    identities = {p.get('source_sha256') for p in result['presets'].values()}
    result['status'] = ('passed' if workflow['conclusion'] == 'success' and len(identities) == 1 and
                        None not in identities and all(p['status'] == 'passed' for p in result['presets'].values()) else 'failed')
    (evidence / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(workflow['url'], result['status'], evidence)
    return int(result['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
