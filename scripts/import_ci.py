#!/usr/bin/env python3
"""Import actual completed Linux workflow evidence, never a workflow definition as a pass."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_id', type=int)
    ns = ap.parse_args()
    raw = subprocess.check_output(['gh', 'run', 'view', str(ns.run_id), '--json',
                                   'conclusion,headSha,url,jobs,status'], cwd=ROOT, text=True)
    workflow = json.loads(raw)
    if workflow['status'] != 'completed':
        ap.error('workflow is not completed; no result imported')
    target = ROOT / 'reports' / f'linux-ci-{ns.run_id}'
    target.mkdir(parents=True, exist_ok=True)
    subprocess.run(['gh', 'run', 'download', str(ns.run_id), '--dir', str(target)], cwd=ROOT, check=True)
    evidence = ROOT / 'evidence' / 'linux'
    evidence.mkdir(parents=True, exist_ok=True)
    result = {'workflow': workflow, 'presets': {}}
    for preset in ('dev', 'release', 'asan', 'ubsan', 'tsan'):
        directory = target / f'linux-{preset}-evidence' / 'evidence'
        verification = directory / 'verification.json'
        if not verification.is_file():
            result['presets'][preset] = {'status': 'unavailable', 'reason': 'artifact missing'}
            continue
        data = json.loads(verification.read_text())
        dest = evidence / f'{preset}.json'
        shutil.copyfile(verification, dest)
        result['presets'][preset] = {'steps': data['presets'].get(preset),
                                      'platform': data['platform'], 'machine': data['machine'],
                                      'source_sha256': data['source']['sha256'], 'evidence': str(dest.relative_to(ROOT))}
        capabilities = directory / 'capabilities.json'
        if capabilities.is_file():
            shutil.copyfile(capabilities, evidence / f'{preset}-capabilities.json')
    (ROOT / 'evidence/linux-ci.json').write_text(json.dumps(result, indent=2) + '\n')
    print(workflow['url'], workflow['conclusion'])


if __name__ == '__main__':
    main()
