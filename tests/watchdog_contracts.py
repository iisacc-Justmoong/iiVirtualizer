#!/usr/bin/env python3
"""Validate the memory watchdog's thresholds and process identity rules."""
from pathlib import Path
import select
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from vm_test_watchdog import (SCRIPTS, candidates, low_memory, matches_command, memory_percent, swapout_bytes,
                              swapout_spike, sustained_swap_pressure)

assert memory_percent('System-wide memory free percentage: 12%\n') == 12
for invalid in ('', 'memory 3%', 'System-wide memory free percentage: 101%'):
    try:
        memory_percent(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid memory pressure accepted')
assert not low_memory([5, 5], 12)
assert not low_memory([11, 12, 11], 12)
assert low_memory([11, 10, 9], 12)
vm_stat = 'Mach Virtual Memory Statistics: (page size of 16384 bytes)\nSwapouts: 814540.\n'
assert swapout_bytes(vm_stat) == 814540 * 16384
for invalid in ('', 'Swapouts: 123.', 'page size of 0 bytes\nSwapouts: 3.',
                'page size of 4096 bytes\nSwapouts: unavailable'):
    try:
        swapout_bytes(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid swap statistics accepted')
assert not swapout_spike(None, 1024**3, 256)
assert not swapout_spike(1024**3, 1024**3 + 255 * 1024**2, 256)
assert swapout_spike(1024**3, 1024**3 + 256 * 1024**2, 256)
assert not swapout_spike(1024**3, 0, 256)
assert sustained_swap_pressure([100*1024**2, 80*1024**2, 76*1024**2], 19, 256)
assert not sustained_swap_pressure([100*1024**2, 80*1024**2, 75*1024**2], 19, 256)
assert not sustained_swap_pressure([256*1024**2], 20, 256)
script = '/work/iiVirtualizer/tests/windows_x86_boot.py'
def cmd(executable='python3', arguments=('-B', script, '--id', 'test-vm')):
    return b'\0'.join([executable.encode(), *(str(value).encode() for value in arguments)]) + b'\0'
assert matches_command(cmd(), script, 'test-vm')
assert not matches_command(cmd(arguments=('-B', script, '--id', 'other')), script, 'test-vm')
assert not matches_command(cmd(arguments=('-c', script, '--id', 'test-vm')), script, 'test-vm')
assert not matches_command(cmd(executable='unrelated', arguments=('-B', script, '--id', 'test-vm')), script, 'test-vm')
linux_script = '/work/iiVirtualizer/tests/os_boot.py'
assert SCRIPTS['linux'] == linux_script
assert matches_command(cmd(arguments=('-B', linux_script, '--id', 'test-vm')), linux_script, 'test-vm')
assert not matches_command(cmd(arguments=('-B', linux_script, '--id', 'test-vm')), script, 'test-vm')
with tempfile.TemporaryDirectory(dir=ROOT / 'resources') as temp:
    proc = Path(temp)
    for pid, raw in [('101', cmd()), ('102', cmd(arguments=('-B', script, '--id', 'other'))),
                     ('1', cmd()), ('not-a-pid', cmd())]:
        (proc / pid).mkdir()
        (proc / pid / 'cmdline').write_bytes(raw)
    assert candidates(proc, script, 'test-vm') == [101]
with tempfile.TemporaryDirectory(dir=ROOT / 'resources') as temp:
    # A one-interval swap spike must remain actionable if the first SSH request
    # fails and the next memory sample has already recovered.
    config = Path(temp) / 'ssh.config'
    config.write_text('')
    simulate = r'''
import contextlib, importlib.util, io, sys, types
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]).resolve().parent))
spec = importlib.util.spec_from_file_location('watchdog_retry_contract', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.sys.platform = 'darwin'
calls = {'stats': 0, 'ssh': 0, 'clock': 0}
def fake_run(command, **_kwargs):
    if command[0].endswith('memory_pressure'):
        return types.SimpleNamespace(stdout='System-wide memory free percentage: 40%', stderr='', returncode=0)
    if command[0].endswith('vm_stat'):
        calls['stats'] += 1
        pages = 0 if calls['stats'] == 1 else 65536
        return types.SimpleNamespace(stdout=f'page size of 4096 bytes\nSwapouts: {pages}.\n', stderr='', returncode=0)
    assert command[0].endswith('ssh')
    calls['ssh'] += 1
    return types.SimpleNamespace(stdout='IIV_WATCHDOG_SIGNAL_SENT' if calls['ssh'] > 1 else '',
                                 stderr='', returncode=0 if calls['ssh'] > 1 else 255)
def fake_clock():
    calls['clock'] += 1
    return calls['clock']
module.subprocess.run = fake_run
module.time.monotonic = fake_clock
module.time.sleep = lambda _duration: None
sys.argv = ['vm_test_watchdog.py', 'monitor', '--kind', 'x86', '--vm-id', 'watchdog-fixture',
            '--container', 'iiv-test-0123456789ab', '--ssh-config', sys.argv[2], '--duration', '60']
output = io.StringIO()
with contextlib.redirect_stdout(output):
    code = module.main()
assert code == 0 and calls['ssh'] == 2
assert 'IIV_WATCHDOG_STOP_REQUEST' in output.getvalue()
assert 'IIV_WATCHDOG_EXPIRED' not in output.getvalue()
'''
    retry = subprocess.run([sys.executable, '-B', '-c', simulate,
                            str(ROOT / 'tools/vm_test_watchdog.py'), str(config)],
                           capture_output=True, text=True, timeout=30)
    assert retry.returncode == 0, retry.stderr
if sys.platform == 'darwin':
    actual_stats = subprocess.run(['/usr/bin/vm_stat'], capture_output=True, text=True,
                                  timeout=10, check=True)
    assert swapout_bytes(actual_stats.stdout) >= 0
    with tempfile.TemporaryDirectory(dir=ROOT / 'resources') as temp:
        config = Path(temp) / 'ssh.config'
        config.write_text('')
        stop = Path(temp) / 'stop'
        command = [sys.executable, '-B', str(ROOT / 'tools/vm_test_watchdog.py'), 'monitor',
                   '--kind', 'x86', '--vm-id', 'watchdog-fixture', '--container', 'iiv-test-0123456789ab',
                   '--ssh-config', str(config), '--duration', '60', '--stop-file', str(stop)]
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            ready, _, _ = select.select([child.stdout], [], [], 60)
            if not ready or not child.stdout.readline().startswith('IIV_WATCHDOG_STARTED'):
                raise RuntimeError('Watchdog monitor did not start')
            stop.touch()
            out, err = child.communicate(timeout=30)
            assert child.returncode == 0 and 'IIV_WATCHDOG_STOPPED' in out, err
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=10)
print('Guest test watchdog memory and ownership contracts passed')
