#!/usr/bin/env python3
"""Retain the extended Inspector's optimized investigation, never sanitizer timings."""
import datetime
import json
from pathlib import Path
import platform
import shutil
import sys

from verify import ROOT, command, identity


def main():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    output = ROOT / 'reports' / f'memory-investigation-{stamp}'
    evidence = ROOT / 'evidence/memory-v1/investigation' / stamp
    before = identity()
    result = command([sys.executable, 'projects/runtime-inspector/report.py', '--output', str(output),
                      '--size', '100003', '--threads', '3', '--seed', '42', '--repetitions', '7',
                      '--memory-size', '128', '--memory-repetitions', '7'], timeout=180)
    evidence.mkdir(parents=True)
    if output.is_dir():
        for path in output.iterdir():
            if path.suffix in {'.txt', '.json', '.md'} and path.is_file():
                shutil.copyfile(path, evidence / path.name)
    after = identity()
    record = {'schema': 1, 'platform': platform.platform(), 'machine': platform.machine(),
              'source': before, 'source_after': after, 'source_drift': before != after,
              'command': result, 'scope': 'optimized uninstrumented Inspector build; full artifacts remain in reports, technical text/JSON retained here',
              'status': 'passed' if result['status'] == 'passed' and before == after else 'failed'}
    (evidence / 'measurement.json').write_text(json.dumps(record, indent=2) + '\n')
    print(record['status'], evidence)
    return int(record['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
