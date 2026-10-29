#!/usr/bin/env python3
"""Protect the host when an isolated guest test runs low on memory."""
import argparse
from collections import deque
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

from test_isolation import require_test_container

SCRIPTS = {
    'linux': '/work/iiVirtualizer/tests/os_boot.py',
    'x86': '/work/iiVirtualizer/tests/windows_x86_boot.py',
    'arm': '/work/iiVirtualizer/tests/windows_arm_boot.py',
}
ROOT = Path(__file__).resolve().parents[1]


def memory_percent(text):
    match = re.search(r'System-wide memory free percentage:\s*(\d+)%', text)
    if not match or int(match.group(1)) > 100:
        raise ValueError('Cannot determine host memory pressure')
    return int(match.group(1))


def low_memory(samples, threshold):
    return len(samples) >= 3 and all(value < threshold for value in list(samples)[-3:])


def swapout_bytes(text):
    page_size = re.search(r'page size of\s+(\d+)\s+bytes', text)
    pages = re.search(r'^\s*Swapouts:\s*(\d+)\.?\s*$', text, re.MULTILINE)
    if not page_size or not pages or int(page_size.group(1)) == 0:
        raise ValueError('Cannot determine host swap activity')
    return int(page_size.group(1)) * int(pages.group(1))


def swapout_spike(previous, current, threshold_mib):
    # A reboot can reset the cumulative counter; establish a new baseline.
    return (previous is not None and current >= previous and
            current - previous >= threshold_mib * 1024**2)


def sustained_swap_pressure(deltas, free_percent, threshold_mib):
    return free_percent < 20 and sum(list(deltas)[-3:]) >= threshold_mib * 1024**2


def matches_command(raw, script, vm_id):
    argv = raw.rstrip(b'\0').split(b'\0')
    if len(argv) < 5 or not Path(os.fsdecode(argv[0])).name.startswith('python3'):
        return False
    if argv[1:3] != [b'-B', os.fsencode(script)]:
        return False
    return any(argv[i:i+2] == [b'--id', os.fsencode(vm_id)] for i in range(3, len(argv)-1))


def candidates(proc, script, vm_id):
    found = []
    for entry in Path(proc).iterdir():
        if not entry.name.isdecimal() or int(entry.name) <= 1:
            continue
        try:
            if matches_command((entry / 'cmdline').read_bytes(), script, vm_id):
                found.append(int(entry.name))
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return found


def interrupt_owned(proc, script, vm_id):
    found = candidates(proc, script, vm_id)
    if not found:
        return False
    if len(found) != 1:
        raise RuntimeError('More than one matching test process; refusing to signal')
    pid = found[0]
    fd = os.pidfd_open(pid)
    try:
        # A pidfd prevents a reused numeric PID from targeting a different process.
        if not matches_command((Path(proc) / str(pid) / 'cmdline').read_bytes(), script, vm_id):
            raise RuntimeError('Test process identity changed; refusing to signal')
        signal.pidfd_send_signal(fd, signal.SIGTERM)
    finally:
        os.close(fd)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    for action in ('monitor', 'signal'):
        command = sub.add_parser(action)
        command.add_argument('--kind', choices=sorted(SCRIPTS), required=True)
        command.add_argument('--vm-id', required=True)
        if action == 'monitor':
            command.add_argument('--ssh-config', type=Path, required=True)
            command.add_argument('--container', required=True)
            command.add_argument('--duration', type=int, required=True)
            command.add_argument('--threshold', type=int, default=12)
            command.add_argument('--max-swapout-mib', type=int, default=256,
                                 help='Stop on this much host swap-out in one sample interval')
            command.add_argument('--stop-file', type=Path,
                                 help='Fresh task-owned file whose creation stops this monitor')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,63}', args.vm_id):
        raise ValueError('Invalid owned VM identifier')
    if args.action == 'signal':
        require_test_container('tcg')
        if interrupt_owned('/proc', SCRIPTS[args.kind], args.vm_id):
            print('IIV_WATCHDOG_SIGNAL_SENT', flush=True)
            return 0
        print('IIV_WATCHDOG_NO_MATCH', flush=True)
        return 3
    if sys.platform != 'darwin' or not re.fullmatch(r'iiv-test-[0-9a-f]{12}', args.container):
        raise ValueError('Monitor requires macOS and a dedicated test container')
    config = args.ssh_config.resolve(strict=True)
    if (ROOT.parents[1] not in config.parents or not 1 <= args.duration <= 43200 or
            not 5 <= args.threshold <= 20 or not 1 <= args.max_swapout_mib <= 8192):
        raise ValueError('Invalid monitor configuration')
    stop_file = None
    if args.stop_file is not None:
        stop_file = args.stop_file.resolve(strict=False)
        if (ROOT / 'resources').resolve(strict=True) not in stop_file.parents or stop_file.exists():
            raise ValueError('Monitor stop file must be a fresh path beneath resources/')
    remote = shlex.join(['sudo', '/usr/local/bin/nerdctl', 'exec', args.container,
                         'python3', '-B', '/work/iiVirtualizer/tools/vm_test_watchdog.py',
                         'signal', '--kind', args.kind, '--vm-id', args.vm_id])
    samples = deque(maxlen=3)
    deadline = time.monotonic() + args.duration
    unavailable = 0
    previous_swap = None
    swap_deltas = deque(maxlen=3)
    stop_pending = False
    def request_stop():
        try:
            result = subprocess.run(['/usr/bin/ssh', '-F', str(config), 'lima-iivc', remote],
                                    capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            print('IIV_WATCHDOG_RETRY', type(error).__name__, flush=True)
            return -1
        print(result.stdout.strip(), result.stderr.strip(), flush=True)
        if result.returncode == 0:
            return 1
        if result.returncode == 3:
            return 0
        return -1
    print('IIV_WATCHDOG_STARTED', args.kind, args.vm_id, flush=True)
    while time.monotonic() < deadline:
        if stop_file is not None and stop_file.is_file():
            print('IIV_WATCHDOG_STOPPED', flush=True)
            return 0
        try:
            report = subprocess.run(['/usr/bin/memory_pressure', '-Q'], capture_output=True, text=True,
                                    timeout=10, check=True)
            vm_stats = subprocess.run(['/usr/bin/vm_stat'], capture_output=True, text=True,
                                      timeout=10, check=True)
            current_swap = swapout_bytes(vm_stats.stdout)
            free = memory_percent(report.stdout)
            swap_delta = None if previous_swap is None or current_swap < previous_swap else current_swap - previous_swap
            if swap_delta is None:
                swap_deltas.clear()
            else:
                swap_deltas.append(swap_delta)
            rapid_swap = (swapout_spike(previous_swap, current_swap, args.max_swapout_mib) or
                          sustained_swap_pressure(swap_deltas, free, args.max_swapout_mib))
            previous_swap = current_swap
            samples.append(free)
            unavailable = 0
        except (OSError, ValueError, subprocess.SubprocessError):
            samples.clear()
            previous_swap = None
            swap_deltas.clear()
            unavailable += 1
            rapid_swap = False
            swap_delta = None
        if (low_memory(samples, args.threshold) or rapid_swap or unavailable >= 3) and not stop_pending:
            print('IIV_WATCHDOG_STOP_REQUEST', list(samples), unavailable, swap_delta, flush=True)
            stop_pending = True
        if stop_pending:
            if stop_file is not None and stop_file.is_file():
                print('IIV_WATCHDOG_STOPPED', flush=True)
                return 0
            if request_stop() == 1:
                return 0
        time.sleep(10)
    if stop_file is not None and stop_file.is_file():
        print('IIV_WATCHDOG_STOPPED', flush=True)
        return 0
    print('IIV_WATCHDOG_EXPIRED', flush=True)
    for attempt in range(3):
        if request_stop() >= 0:
            return 0
        if attempt < 2:
            time.sleep(10)
    return 1


if __name__ == '__main__':
    sys.exit(main())
