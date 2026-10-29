#!/usr/bin/env python3
"""Test host-guard thresholds and ownership of a signalled process group."""
import os
from pathlib import Path
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from container_host_guard import emergency_memory, signal_own_start, validate_min_free_percent

assert emergency_memory([40, 22], 0)
assert emergency_memory([24], 0)
assert not emergency_memory([25], 0)
assert emergency_memory([39], 0, 40)
assert not emergency_memory([40], 0, 40)
assert validate_min_free_percent(40) == 40
for invalid in (24, 100):
    try:
        validate_min_free_percent(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Unsafe memory threshold was accepted')
assert emergency_memory([40], 256 * 1024**2)
assert not emergency_memory([40], 255 * 1024**2)
if os.name == 'posix':
    command = [sys.executable, '-B', '-c', 'import time; time.sleep(60)']
    own = subprocess.Popen(command, start_new_session=True)
    other = subprocess.Popen(command, start_new_session=True)
    shared = subprocess.Popen(command)
    try:
        try:
            signal_own_start(shared, signal.SIGTERM)
        except RuntimeError:
            pass
        else:
            raise AssertionError('Shared process group was not rejected')
        assert shared.poll() is None
        assert signal_own_start(own, signal.SIGTERM)
        assert own.wait(timeout=10) != 0
        assert other.poll() is None
        assert not signal_own_start(own, signal.SIGTERM)
    finally:
        if own.poll() is None:
            own.terminate()
            own.wait(timeout=10)
        if other.poll() is None:
            other.terminate()
            other.wait(timeout=10)
        if shared.poll() is None:
            shared.terminate()
            shared.wait(timeout=10)
print('Host guard thresholds and owned-process contracts passed')
