#!/usr/bin/env python3
"""Same GET workload, serial versus batched issuance; no server service-time claims."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import selectors
import socket
import statistics
import subprocess
import time
from verify import identity

ROOT = Path(__file__).resolve().parents[1]


def percentile(xs, q):
    return sorted(xs)[min(len(xs) - 1, int((len(xs) - 1) * q))]


def trial(binary, batch, count):
    p = subprocess.Popen([str(binary), '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        with selectors.DefaultSelector() as sel:
            sel.register(p.stdout, selectors.EVENT_READ)
            if not sel.select(5):
                raise TimeoutError('READY deadline')
            ready = p.stdout.readline().decode().strip().split()
        if len(ready) != 2 or ready[0] != 'READY':
            raise RuntimeError(f'bad handshake {ready}')
        with socket.create_connection(('127.0.0.1', int(ready[1])), timeout=3) as sock:
            sock.settimeout(3)
            stream = sock.makefile('rb')
            sock.sendall(b'SET bench value\n')
            if stream.readline() != b'OK\n':
                raise RuntimeError('setup failed')
            for _ in range(32):
                sock.sendall(b'GET bench\n')
                if stream.readline() != b'VALUE value\n':
                    raise RuntimeError('warmup failed')
            samples = []
            start = time.monotonic_ns()
            checksum = hashlib.sha256()
            for first in range(0, count, batch):
                n = min(batch, count - first)
                intended = time.monotonic_ns()  # closed-loop: no externally paced offered load
                issue = time.monotonic_ns()
                sock.sendall(b'GET bench\n' * n)
                for index in range(n):
                    answer = stream.readline()
                    complete = time.monotonic_ns()
                    if answer != b'VALUE value\n':
                        raise RuntimeError(f'wrong response {answer!r}')
                    checksum.update(answer)
                    samples.append({'request': first + index, 'intended_ns': intended - start,
                                    'issue_ns': issue - start, 'complete_ns': complete - start,
                                    'observed_rtt_ns': complete - issue,
                                    'client_schedule_delay_ns': issue - intended})
            end = time.monotonic_ns()
            elapsed = (end - start) / 1e9
            if elapsed <= 0:
                raise RuntimeError('nonpositive elapsed')
            rtts = [x['observed_rtt_ns'] for x in samples]
            return {'batch': batch, 'requests': count, 'elapsed_seconds': elapsed,
                    'throughput_requests_per_second': count / elapsed, 'checksum': checksum.hexdigest(),
                    'p50_rtt_ns': statistics.median(rtts), 'p95_rtt_ns': percentile(rtts, .95),
                    'p99_rtt_ns': percentile(rtts, .99), 'samples': samples}
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=2)
        if p.stdout:
            p.stdout.close()
        if p.stderr:
            p.stderr.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', type=Path, default=ROOT / 'build/release/projects/tcp-kv/tcp-kv')
    ap.add_argument('--requests', type=int, default=256)
    ap.add_argument('--repetitions', type=int, default=5)
    ns = ap.parse_args()
    if not 1 <= ns.requests <= 100000 or not 1 <= ns.repetitions <= 20:
        ap.error('bounded requests/repetitions required')
    revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True, capture_output=True)
    report = {'question': 'Does batched client issuance change observed loopback throughput and RTT?',
              'hypothesis': 'Batching may amortize client/server syscall scheduling, but may raise within-batch RTT.',
              'change': 'One client workload change: batch 1 versus 16. No server implementation change.',
              'platform': platform.platform(), 'isa': platform.machine(), 'revision': revision.stdout.strip(),
              'source_manifest': identity(),
              'dirty': subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True),
              'binary_sha256': hashlib.sha256(ns.binary.read_bytes()).hexdigest(),
              'compile_commands': json.loads((ROOT / 'build/release/compile_commands.json').read_text()),
              'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'seed': 0,
              'boundaries': '32 warmup GETs and SET/setup excluded; send, receive and client checksum included.',
              'latency': 'Closed-loop observed round trip per request from batch issue; service time unavailable.',
              'raw': [], 'errors': []}
    for repetition in range(ns.repetitions):
        for batch in ([1, 16] if repetition % 2 == 0 else [16, 1]):
            try:
                report['raw'].append({'repetition': repetition, **trial(ns.binary.resolve(), batch, ns.requests)})
            except Exception as exc:
                report['errors'].append({'repetition': repetition, 'batch': batch, 'error': str(exc)})
    report['conclusion'] = 'Observational workload comparison only; no causal cache/syscall claim or required speedup. See raw distributions.'
    out = ROOT / 'evidence/server-investigation.json'
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(out)
    for batch in (1, 16):
        runs = [r for r in report['raw'] if r['batch'] == batch]
        if runs:
            print(f"batch={batch}: median throughput={statistics.median(r['throughput_requests_per_second'] for r in runs):.1f} req/s, runs={len(runs)}")
    return int(bool(report['errors']))


if __name__ == '__main__':
    raise SystemExit(main())
