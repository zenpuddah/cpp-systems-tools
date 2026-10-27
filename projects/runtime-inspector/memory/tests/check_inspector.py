"""Verify the actual extended Inspector workflow, not a generated placeholder."""
import hashlib
import json
from pathlib import Path
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


output = Path(sys.argv[1])
meta = json.loads((output / 'report.json').read_text())
raw = json.loads((output / 'memory.json').read_text())
require(not meta['errors'], 'Inspector reported an error')
require(meta['memory'] == raw and bool(raw), 'memory document missing or mismatched')
require(not meta['source_drift'], 'source drift invalidates the measured identity')
require(meta['source_before_sha256'] == meta['source_sha256'], 'source identity mismatch')
require(meta['memory_binary_sha256'] == hashlib.sha256((output / 'runtime-memory').read_bytes()).hexdigest(), 'measured binary identity mismatch')
require('-std=c++20' in meta['memory_flags'] and '-O2' in meta['memory_flags'], 'actual memory build flags absent')
require(not any('sanitize=' in flag for flag in meta['memory_flags']), 'timings must not come from sanitizer builds')
commands = {c['name']: c for c in meta['commands']}
for name in ('build-memory', 'memory', 'validate-memory'):
    require(commands[name]['status'] == 'passed' and commands[name]['exit_code'] == 0, name + ' did not pass')
require(not any('sanitize=' in flag for flag in commands['build-memory']['command']), 'instrumented compiler invocation used for timings')
require('Layout, caller-buffer allocation and PMR' in (output / 'report.md').read_text(), 'human memory section absent')
print('passed: extended Inspector workflow, memory artifacts, binary/source identity and declared flags')
