#!/usr/bin/env python3
"""Require Windows x86 identity and successful execution of its native tool."""
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from windows_x86_boot import parse_build_event, startup_proof

job = '0123456789abcdef0123456789abcdef-win'
started = {'kind': 'started', 'job': job, 'os': 'windows', 'architecture': 'x86_64', 'workspace': 'C:/iiv/work'}
step = {'kind': 'step', 'job': job, 'index': 0, 'executable': 'C:/iiv/tools/clang/bin/x86_64-w64-mingw32-clang++.exe'}
finished = {'kind': 'exit', 'job': job, 'index': 0, 'code': 0, 'reason': 0}
events = [started, step, finished]
assert startup_proof(events, job)
for bad in [dict(started, architecture='aarch64'), dict(started, os='linux'), dict(started, job='wrong')]:
    assert not startup_proof([bad, step, finished], job)
for bad in [dict(started, os=None), dict(started, workspace=[]), dict(step, executable=None)]:
    candidate = [bad, step, finished] if bad.get('kind') == 'started' else [started, bad, finished]
    assert not startup_proof(candidate, job)
assert not startup_proof([started, step, dict(finished, code=1)], job)
assert not startup_proof([started, finished], job)
assert not startup_proof([step, started, finished], job)
assert parse_build_event(b'IIVB1 ' + json.dumps(started).encode(), job) == started
assert parse_build_event(b'firmware output IIVB1 ' + json.dumps(started).encode(), job) == started
assert parse_build_event(b'not an event', job) is None
assert parse_build_event(b'IIVB1 {broken', job) is None
assert parse_build_event(b'IIVB1 ' + json.dumps(dict(started, job='wrong')).encode(), job) is None
if sys.platform != 'win32':
    child = r'''
import os, signal, sys
sys.path.insert(0, sys.argv[1])
from windows_x86_boot import cleanup_stop_requested
requested = cleanup_stop_requested()
assert not requested()
os.kill(os.getpid(), signal.SIGTERM)
assert requested()
print('CLEANUP_STOP_REQUESTED')
'''
    result = subprocess.run([sys.executable, '-B', '-c', child, str(Path(__file__).resolve().parent)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and 'CLEANUP_STOP_REQUESTED' in result.stdout, result.stderr
print('Windows x86 startup and native tool execution evidence contracts passed')
