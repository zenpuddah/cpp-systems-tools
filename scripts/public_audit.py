#!/usr/bin/env python3
"""Fail closed on files outside this public product's explicit content allowlist."""
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOP = {'AGENTS.md', '.gitignore', 'PLAN.md', 'CMakeLists.txt', 'CMakePresets.json', 'README.md', 'progress.md'}
DIRS = {'docs', 'scripts', 'tests', 'evidence'}
SUFFIXES = {'.md', '.py', '.cpp', '.hpp', '.h', '.json', '.txt', '.yml', '.yaml', '.cmake'}
FORBIDDEN = {'.env', '.git', 'author', 'solutions', 'attempts', 'current-context.md', 'current-context.json', 'reference.lock.json', 'learn.py'}


def allowed(name):
    p = Path(name)
    if p.is_absolute() or '..' in p.parts or any(x in FORBIDDEN for x in p.parts):
        return False
    if len(p.parts) == 1:
        return name in TOP
    if p.parts[:2] == ('.github', 'workflows'):
        return p.suffix in {'.yml', '.yaml'}
    if p.parts[0] == 'projects':
        return len(p.parts) >= 3 and p.parts[1] in {'tcp-kv', 'runtime-inspector'} and (
            p.suffix in SUFFIXES or p.name == '.gitignore' or ('evidence' in p.parts and p.suffix == '.log'))
    return p.parts[0] in DIRS and p.suffix in SUFFIXES


def main():
    files = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
    rejected = []
    for name in sorted(set(filter(None, files))):
        p = ROOT / name
        if not allowed(name) or p.is_symlink():
            rejected.append(name)
        elif p.is_file() and b'/Users/' in p.read_bytes() and b'/Library/' in p.read_bytes():
            # Compiler SDK paths are technical evidence; private Library documents are not.
            data = p.read_text(errors='replace')
            if re.search(r'/Users/[^/\s]+/Library/(?:Mobile Documents|Containers|CloudStorage)/', data):
                rejected.append(name + ': private Library path')
    print(json.dumps({'status': 'failed' if rejected else 'passed', 'rejected': rejected,
                      'policy': 'technical product files only; explicit paths/extensions; no symlinks/private history'}, indent=2))
    return int(bool(rejected))


if __name__ == '__main__':
    raise SystemExit(main())
