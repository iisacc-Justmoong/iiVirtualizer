#!/usr/bin/env python3
"""Start and monitor this project's dedicated Lima test host on macOS."""
import argparse
from collections import deque
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from vm_test_watchdog import memory_percent, swapout_bytes

ROOT = Path(__file__).resolve().parents[1]
MIN_FREE_PERCENT = 25


def validate_min_free_percent(value):
    if not isinstance(value, int) or not MIN_FREE_PERCENT <= value <= 99:
        raise ValueError('The free-memory threshold must be between 25 and 99 percent')
    return value


def emergency_memory(samples, recent_swap, min_free_percent=MIN_FREE_PERCENT):
    return bool(samples) and (samples[-1] < min_free_percent or recent_swap >= 256 * 1024**2)


def signal_own_start(child, signum):
    if child.poll() is not None:
        return False
    if os.getpgid(child.pid) != child.pid:
        raise RuntimeError('Refusing to signal a process group not owned by this guard')
    os.killpg(child.pid, signum)
    return True


def sample_host():
    report = subprocess.run(['/usr/bin/memory_pressure', '-Q'], capture_output=True,
                            text=True, timeout=10, check=True)
    stats = subprocess.run(['/usr/bin/vm_stat'], capture_output=True,
                           text=True, timeout=10, check=True)
    return memory_percent(report.stdout), swapout_bytes(stats.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duration', type=int, default=43200)
    parser.add_argument('--start-log', type=Path, required=True)
    parser.add_argument('--min-free-percent', type=int, default=MIN_FREE_PERCENT)
    args = parser.parse_args()
    if sys.platform != 'darwin' or not 1 <= args.duration <= 43200:
        raise ValueError('A macOS host and a duration of at most twelve hours are required')
    validate_min_free_percent(args.min_free_percent)
    resources = (ROOT / 'resources').resolve(strict=True)
    lima_home = (resources / 'ct').resolve(strict=True)
    instance = (lima_home / 'iivc').resolve(strict=True)
    lima = (resources / 'platform-guests/container-runtime/lima-2.2.1/bin/limactl').resolve(strict=True)
    tmpdir = (resources / 'platform-guests/container-runtime/tmp').resolve(strict=True)
    if instance.parent != lima_home or not lima.is_file() or not tmpdir.is_dir():
        raise ValueError('The dedicated host resources are not available')
    log = args.start_log.resolve(strict=False)
    if (ROOT / 'build').resolve(strict=True) not in log.parents or log.exists():
        raise ValueError('Choose a fresh start log beneath build/')
    initial_free, previous_swap = sample_host()
    if initial_free < args.min_free_percent:
        raise RuntimeError('Host memory is below the startup threshold')
    environment = os.environ.copy()
    environment['LIMA_HOME'] = str(lima_home)
    environment['TMPDIR'] = str(tmpdir)
    stop = [False]
    def request_stop(_signum, _frame):
        stop[0] = True
    for name in ('SIGINT', 'SIGTERM', 'SIGHUP'):
        signal.signal(getattr(signal, name), request_stop)
    samples = deque([initial_free], maxlen=3)
    swap_deltas = deque(maxlen=20)
    unavailable = 0
    deadline = time.monotonic() + args.duration
    with log.open('xb') as stream:
        child = subprocess.Popen([str(lima), 'start', '--foreground', '--timeout', '20m', '--tty=false', 'iivc'],
                                 env=environment, stdout=stream, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        print('IIV_HOST_GUARD_STARTED', child.pid, flush=True)
        while child.poll() is None:
            if stop[0] or time.monotonic() >= deadline:
                print('IIV_HOST_GUARD_STOP_REQUEST', 'signal-or-deadline', flush=True)
                break
            try:
                free, current_swap = sample_host()
                if current_swap < previous_swap:
                    swap_deltas.clear()
                delta = max(0, current_swap - previous_swap)
                swap_deltas.append(delta)
                previous_swap = current_swap
                samples.append(free)
                unavailable = 0
                if emergency_memory(samples, sum(swap_deltas), args.min_free_percent):
                    print('IIV_HOST_GUARD_STOP_REQUEST', list(samples), sum(swap_deltas), flush=True)
                    break
            except (OSError, ValueError, subprocess.SubprocessError):
                samples.clear()
                unavailable += 1
                if unavailable >= 3:
                    print('IIV_HOST_GUARD_STOP_REQUEST', 'sampling-unavailable', flush=True)
                    break
            time.sleep(3)
        else:
            print('IIV_HOST_GUARD_HOST_EXITED', child.returncode, flush=True)
            # A failed foreground start can leave a host agent behind. Ask the
            # same dedicated instance to stop before the guard itself exits.
            try:
                subprocess.run([str(lima), 'stop', 'iivc'], env=environment,
                               stdout=stream, stderr=subprocess.STDOUT, timeout=90)
            except (OSError, subprocess.TimeoutExpired):
                print('IIV_HOST_GUARD_STOP_UNCONFIRMED', flush=True)
            return child.returncode
        # Only the dedicated instance and the child process group created by
        # this guard may be stopped; unrelated host workloads are untouched.
        while child.poll() is None:
            try:
                subprocess.run([str(lima), 'stop', 'iivc'], env=environment,
                               stdout=stream, stderr=subprocess.STDOUT, timeout=90)
            except (OSError, subprocess.TimeoutExpired):
                pass
            if child.poll() is None:
                signal_own_start(child, signal.SIGINT)
                try:
                    child.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    print('IIV_HOST_GUARD_STOP_RETRY', flush=True)
                    time.sleep(10)
        print('IIV_HOST_GUARD_START_EXITED', child.returncode, flush=True)
    return 1


if __name__ == '__main__':
    sys.exit(main())
