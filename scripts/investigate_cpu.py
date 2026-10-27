#!/usr/bin/env python3
"""Record the release workload investigation plus independent integer oracle checks."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
from verify import identity

ROOT = Path(__file__).resolve().parents[1]


def reference(size, seed):
    mask = (1 << 64) - 1
    return sum(((((i ^ seed) * 0x9e3779b97f4a7c15 + seed) & mask) >> 17) for i in range(size)) & mask


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--size', type=int, default=200000)
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--repetitions', type=int, default=7)
    ns = ap.parse_args()
    if not 0 <= ns.size <= 10000000 or not 1 <= ns.threads <= 32 or not 0 <= ns.seed < 1 << 64 or not 1 <= ns.repetitions <= 30:
        ap.error('size/threads/seed/repetitions outside workload bounds')
    binary = ROOT / 'build/release/projects/runtime-inspector/runtime-workloads'
    argv = [str(binary), '--size', str(ns.size), '--threads', str(ns.threads), '--seed', str(ns.seed), '--repetitions', str(ns.repetitions)]
    p = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        print(p.stderr, file=sys.stderr)
        return p.returncode
    data = json.loads(p.stdout)
    expected = reference(ns.size, ns.seed)
    valid = data['expected'] == expected and all(s['checksum'] == expected for s in data['samples'])
    medians = {variant: statistics.median(s['seconds'] for s in data['samples'] if s['variant'] == variant)
               for variant in {s['variant'] for s in data['samples']}}
    report = {'question': 'Does private accumulation reduce observed cost compared with a shared atomic?',
              'baseline': 'shared', 'candidates': ['spaced', 'local'], 'diagnostic_control': 'adjacent',
              'hypothesis': 'Removing per-item shared atomic updates may reduce contention; spacing alone may not explain it.',
              'changes': 'Two existing workload alternatives, no performance-motivated source modification.',
              'platform': platform.platform(), 'isa': platform.machine(),
              'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'source_manifest': identity(),
              'dirty': subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True),
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'command': argv, 'exit_code': p.returncode,
              'compiler': subprocess.check_output(['c++', '--version'], text=True),
              'compile_commands': json.loads((ROOT / 'build/release/compile_commands.json').read_text()),
              'independent_python_expected': expected, 'correctness': 'passed' if valid else 'failed',
              'raw': data, 'median_seconds': medians,
              'conclusion': 'Observed timing comparison only. No cache/false-sharing causal claim without counters/tracing; no required speedup.'}
    out = ROOT / 'evidence/cpu-investigation.json'
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(out)
    print(json.dumps({'correctness': report['correctness'], 'median_seconds': medians}, indent=2))
    return int(not valid)


if __name__ == '__main__':
    raise SystemExit(main())
