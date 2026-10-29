#!/usr/bin/env python3
"""Boot an owned Windows x86 guest and verify its native build worker starts."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_os_guests import generated_path
from test_isolation import TestInterrupted, install_interrupt_handlers, protect_cleanup_from_interrupts, require_test_container


def parse_build_event(line, job):
    marker = line.find(b'IIVB1 ')
    if marker < 0 or len(line) > 65536:
        return None
    try:
        event = json.loads(line[marker + 6:])
    except (ValueError, UnicodeDecodeError):
        return None
    return event if isinstance(event, dict) and event.get('job') == job else None


def startup_proof(events, job):
    state = 0
    for event in events:
        if event.get('job') != job:
            continue
        if event.get('kind') == 'started':
            os_name = event.get('os')
            workspace = event.get('workspace')
            if (state != 0 or not isinstance(os_name, str) or os_name.lower() != 'windows' or
                    event.get('architecture') != 'x86_64' or not isinstance(workspace, str) or
                    not workspace.lower().startswith('c:/')):
                return False
            state = 1
        elif event.get('kind') == 'step' and event.get('index') == 0:
            name = event.get('executable')
            if not isinstance(name, str):
                return False
            name = name.replace('\\', '/').lower()
            if state != 1 or not name.endswith('/x86_64-w64-mingw32-clang++.exe'):
                return False
            state = 2
        elif event.get('kind') == 'exit' and event.get('index') == 0:
            if state != 2 or event.get('code') != 0 or event.get('reason') != 0:
                return False
            return True
        elif event.get('kind') in ('failed', 'error'):
            return False
    return False


def qmp_powerdown(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as channel:
        channel.settimeout(30)
        channel.connect(str(path))
        stream = channel.makefile('rwb', buffering=0)
        greeting = json.loads(stream.readline())
        if 'QMP' not in greeting:
            raise RuntimeError('Missing QMP greeting')
        for identifier, command in [('cap', 'qmp_capabilities'), ('power', 'system_powerdown')]:
            stream.write((json.dumps({'execute': command, 'id': identifier}) + '\n').encode())
            while True:
                reply = json.loads(stream.readline())
                if reply.get('id') == identifier:
                    if 'return' not in reply:
                        raise RuntimeError('QMP power request failed')
                    break


def cleanup_stop_requested():
    """Record a stop request without interrupting owned-VM cleanup."""
    requested = [False]
    def record_stop(_signum, _frame):
        requested[0] = True
    protect_cleanup_from_interrupts(record_stop)
    return lambda: requested[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime', 'qemu', 'root', 'output', 'job', 'receipt', 'source-receipt'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--id', required=True)
    parser.add_argument('--timeout', type=int, default=7200)
    args = parser.parse_args()
    require_test_container('tcg')
    if sorted(os.listdir('/sys/class/net')) != ['lo']:
        raise RuntimeError('The VM test container must have no network')
    resources = (ROOT / 'resources').resolve(strict=True)
    root = args.root.resolve(strict=False)
    if resources not in root.parents:
        raise ValueError('Windows VM instances must remain beneath resources/')
    output = generated_path(args.output)
    if output.exists() or args.timeout < 1:
        raise ValueError('Choose a fresh output and positive timeout')
    job = json.loads(args.job.read_text())['job']
    if not re.fullmatch(r'[0-9a-f]{32}-win', job):
        raise ValueError('Invalid Windows job identity')
    receipt = json.loads(args.receipt.read_text())
    source = json.loads(args.source_receipt.read_text())
    if (not receipt.get('complete') or not source.get('complete') or
            not isinstance(receipt.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', receipt['sha256']) or
            receipt['sha256'] != source.get('sha256') or not isinstance(receipt.get('written'), int) or
            receipt['written'] <= 0 or receipt['written'] != source.get('data_sent')):
        raise ValueError('Windows disk transfer receipts do not agree')
    output.mkdir(parents=True)
    commands = []
    def run(command, *options):
        argv = [str(args.runtime.resolve()), command, args.id, '--root', str(root),
                '--qemu-x86_64', str(args.qemu.resolve()), '--startup-timeout', '180',
                '--control-timeout', '60', *map(str, options)]
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=1800)
        commands.append({'argv': argv, 'returncode': completed.returncode,
                         'stdout': completed.stdout, 'stderr': completed.stderr})
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if completed.returncode:
            raise RuntimeError('VM command failed: ' + completed.stderr)
        return json.loads(completed.stdout)
    vm = run('describe')
    if (vm['state'] != 'stopped' or vm['architecture'] != 'x86_64' or vm['accelerator'] != 'tcg' or
            vm['network'] or vm['boot_mode'] != 'disk' or receipt.get('logical_size') != vm['disk_mib'] * 1024**2):
        raise ValueError('The stopped Windows VM does not match the isolated disk receipt')
    limit = Path('/sys/fs/cgroup/memory.max').read_text().strip()
    if limit != 'max' and int(limit) < (vm['memory_mib'] + 512) * 1024**2:
        raise RuntimeError('Container memory must leave 512 MiB beyond guest RAM')
    plan = run('plan')
    if plan[plan.index('-accel') + 1] != 'tcg' or plan[plan.index('-nic') + 1] != 'none':
        raise ValueError('Windows x86 VM plan is not isolated TCG')
    events = []
    proof = False
    forced = False
    install_interrupt_handlers()
    try:
        vm = run('start')
        begun = time.monotonic()
        buffer = bytearray()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as serial, (output / 'serial.log').open('wb') as log:
            serial.settimeout(2)
            serial.connect(str(Path(vm['directory']) / 'agent.sock'))
            while time.monotonic() - begun < args.timeout and not proof:
                try:
                    chunk = serial.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    raise RuntimeError('Windows agent serial channel closed before startup proof')
                log.write(chunk)
                log.flush()
                buffer.extend(chunk)
                while b'\n' in buffer:
                    line, _, rest = buffer.partition(b'\n')
                    buffer = bytearray(rest)
                    event = parse_build_event(line.rstrip(b'\r'), job)
                    if event:
                        events.append(event)
                        (output / 'events.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in events))
                        if event.get('kind') in ('failed', 'error'):
                            raise RuntimeError('Windows native worker failed: ' + str(event))
                        proof = startup_proof(events, job)
                        if proof:
                            break
                if len(buffer) > 65536 or log.tell() > 8 * 1024 * 1024:
                    raise RuntimeError('Windows serial output exceeded its bound')
        if not proof:
            raise TimeoutError('Windows x86 native worker startup timed out')
        lifecycle = [run('pause'), run('resume'), run('describe')]
        if [item['state'] for item in lifecycle] != ['paused', 'running', 'running']:
            raise RuntimeError('Windows x86 VM lifecycle mismatch')
    finally:
        interrupted = isinstance(sys.exc_info()[1], (TestInterrupted, KeyboardInterrupt))
        stop_requested = cleanup_stop_requested()
        state = run('describe')
        if state['state'] == 'paused' and not stop_requested():
            state = run('resume')
        if state['state'] in ('running', 'paused'):
            try:
                if state['state'] == 'running' and not stop_requested():
                    qmp_powerdown(Path(state['directory']) / 'qmp.sock')
                    until = time.monotonic() + (30 if interrupted else 300)
                    while time.monotonic() < until and not stop_requested():
                        state = run('describe')
                        if state['state'] == 'stopped':
                            break
                        time.sleep(5)
            except (OSError, ValueError, RuntimeError):
                pass
            protect_cleanup_from_interrupts()
            if state['state'] != 'stopped':
                forced = True
                state = run('stop', '--force')
        else:
            protect_cleanup_from_interrupts()
        (output / 'stopped.json').write_text(json.dumps(state, indent=2) + '\n')
        if state['state'] != 'stopped':
            raise RuntimeError('Owned Windows VM did not stop')
    result = {'passed': True, 'guest': 'windows', 'architecture': 'x86_64', 'accelerator': 'tcg',
              'recorded_at': datetime.now(timezone.utc).isoformat(), 'job': job, 'uuid': vm['uuid'],
              'proof': 'installed Windows native worker started and compiler version command exited successfully',
              'disk_transfer_sha256': receipt['sha256'], 'network': False, 'forced_stop': forced, 'lifecycle': lifecycle}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    removed = run('remove')
    (output / 'removed.json').write_text(json.dumps(removed, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
