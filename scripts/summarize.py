#!/usr/bin/env python3
"""Print actual evidence status without promoting unavailable/unrun checks to passes."""
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]


def main():
    path = ROOT / 'evidence/verification.json'
    if not path.exists():
        print('Product verification: not run')
        return 1
    data = json.loads(path.read_text())
    print(f"Platform: {data['platform']} ({data['machine']})")
    print(f"Source manifest: {data['source']['sha256']}")
    for preset, steps in data['presets'].items():
        configure, build, test = steps
        counts = test.get('tests', {})
        print(f"{preset}: configure={configure['status']}, build={build['status']}, tests={test['status']} "
              f"({counts.get('passed', 'unknown')}/{counts.get('count', 'not run')})")
    path = ROOT / 'evidence/server-investigation.json'
    if path.exists():
        investigation = json.loads(path.read_text())
        print(f"Server measurement errors: {len(investigation['errors'])}")
        for batch in (1, 16):
            samples = [r for r in investigation['raw'] if r['batch'] == batch]
            if samples:
                rate = statistics.median(r['throughput_requests_per_second'] for r in samples)
                print(f'  batch {batch}: median {rate:.1f} requests/s; {len(samples)} repetitions')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
