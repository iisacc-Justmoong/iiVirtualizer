#!/usr/bin/env python3
"""Verify the watchdog signals only its owned fixture in an isolated container."""
from pathlib import Path
import select
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from test_isolation import require_test_container
from vm_test_watchdog import interrupt_owned

try:
    require_test_container('tcg')
except RuntimeError as error:
    print(str(error), file=sys.stderr)
    sys.exit(77)

with tempfile.TemporaryDirectory(dir=ROOT / 'resources') as temp:
    fixture = Path(temp) / 'fixture.py'
    fixture.write_text('import signal,sys,time\nsignal.signal(signal.SIGTERM, lambda s,f: sys.exit(42))\n'
                       'print("ready", flush=True)\nwhile True: time.sleep(1)\n')
    processes = []
    try:
        for vm_id in ('watchdog-first', 'watchdog-other'):
            child = subprocess.Popen([sys.executable, '-B', str(fixture), '--id', vm_id], stdout=subprocess.PIPE)
            processes.append(child)
            ready, _, _ = select.select([child.stdout], [], [], 60)
            if not ready or child.stdout.readline().strip() != b'ready':
                raise RuntimeError('Watchdog fixture did not start')
        assert interrupt_owned('/proc', str(fixture), 'watchdog-first')
        assert processes[0].wait(timeout=30) == 42
        assert processes[1].poll() is None
        assert not interrupt_owned('/proc', str(fixture), 'missing')
        print('IIV_WATCHDOG_OWNED_PROCESS_ONLY')
    finally:
        for child in processes:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
