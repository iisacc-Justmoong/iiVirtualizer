#!/usr/bin/env python3
"""Boot a prepared foreign guest through iiVirtualizer and retain real evidence."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_os_guests import digest, generated_path
from test_isolation import install_interrupt_handlers, protect_cleanup_from_interrupts, require_test_container


def boot_report(console, guest, architecture='aarch64'):
    prefix = 'IIV_' + guest.upper() + '_BOOT_'
    # Only complete guest-produced lines qualify. Echoed commands and a kernel
    # command line containing a marker cannot pass readiness by themselves.
    clean = console.replace('\r', '')
    matches = list(re.finditer(r'^' + prefix + r'BEGIN\n(.*?)^' + prefix + r'END\s*$',
                              clean, re.M | re.S))
    if not matches:
        return None
    body = matches[-1].group(1)
    fields = dict(re.findall(r'^([a-zA-Z0-9_.]+)=([^\n]*)$', body, re.M))
    expected = {'debian': {'os_id': 'debian', 'os_version': '13', 'pid1': 'systemd',
                           'multi_user': 'active', 'architecture': architecture},
                'android': {'os_id': 'android', 'os_version': '9', 'pid1': 'init',
                            'api_level': '28', 'sys.boot_completed': '1',
                            'init.svc.zygote': 'running', 'init.svc.surfaceflinger': 'running'}}
    if any(fields.get(key) != value for key, value in expected[guest].items()):
        return None
    if not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', fields.get('boot_id', '')):
        return None
    if guest == 'android' and not re.search(r'\bsystem_server\s*$', body, re.M):
        return None
    return {'fields': fields, 'text': body}


def stop_owned(run, expected_uuid):
    state = run('describe')
    if state['uuid'] != expected_uuid:
        raise RuntimeError('Guest identity changed; refusing to stop it')
    if state['state'] in ('running', 'paused'):
        state = run('stop', '--force')
    if state['state'] != 'stopped' or state['uuid'] != expected_uuid:
        raise RuntimeError('Owned guest did not stop')
    return state


def resource_root(path):
    root = generated_path(path)
    if (Path(__file__).resolve().parents[1] / 'resources').resolve() not in root.parents:
        raise ValueError('Guest instances must remain beneath resources/')
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--id', required=True)
    parser.add_argument('--guest', choices=['debian', 'android'], required=True)
    parser.add_argument('--accel', choices=['auto', 'hvf', 'kvm', 'tcg'], required=True)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=1200)
    parser.add_argument('--startup-timeout', type=int, default=30)
    parser.add_argument('--control-timeout', type=int, default=5)
    parser.add_argument('--reuse', action='store_true', help='Reuse a stopped matching instance')
    parser.add_argument('--clone-base', action='store_true', help='Prefer a copy-on-write base disk on supported hosts')
    args = parser.parse_args()
    require_test_container(args.accel)
    install_interrupt_handlers()
    if sorted(os.listdir('/sys/class/net')) != ['lo']:
        raise RuntimeError('The VM test container must have no network')
    output, root = generated_path(args.output), resource_root(args.root)
    if output.exists():
        parser.error('Evidence directory already exists')
    output.mkdir(parents=True)
    recipe = json.loads(args.image.read_text())
    for key, expected in recipe['artifact_sha256'].items():
        if digest(recipe[key]) != expected:
            raise ValueError('Prepared image checksum changed: ' + key)
    commands = []

    def run(command, *options):
        argv = [str(args.runtime.resolve()), command, args.id, '--root', str(root),
                '--qemu-' + recipe['architecture'], str(args.qemu.resolve()),
                '--startup-timeout', str(args.startup_timeout), '--control-timeout', str(args.control_timeout), *map(str, options)]
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=1800)
        commands.append({'argv': argv, 'returncode': completed.returncode,
                         'stdout': completed.stdout, 'stderr': completed.stderr})
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if completed.returncode:
            raise RuntimeError(completed.stderr)
        return json.loads(completed.stdout)

    if (root / args.id).exists():
        if not args.reuse:
            raise ValueError('Instance exists; explicit --reuse is required')
        vm = run('describe')
        if vm['state'] != 'stopped':
            raise ValueError('Reuse requires a stopped instance')
        for key in ['architecture', 'cpus', 'memory_mib', 'disk_mib', 'graphics', 'kernel_command_line']:
            if vm[key] != recipe[key]:
                raise ValueError('Existing instance differs from recipe: ' + key)
        for key in ['kernel', 'initramfs']:
            if digest(vm[key]) != recipe['artifact_sha256'][key]:
                raise ValueError('Existing instance boot file differs: ' + key)
    else:
        vm = run('create', '--kernel', recipe['kernel'], '--initramfs', recipe['initramfs'],
                 '--base-disk', recipe['base_disk'], '--arch', recipe['architecture'],
                 '--accel', args.accel, '--cpus', recipe['cpus'], '--memory', recipe['memory_mib'],
                 '--disk', recipe['disk_mib'], '--graphics', recipe['graphics'],
                 '--kernel-args', recipe['kernel_command_line'], *(['--clone-base'] if args.clone_base else []))
    if vm['accelerator'] != args.accel:
        raise ValueError('Instance accelerator differs from requested test')
    console = Path(vm['console'])
    expected_uuid = vm['uuid']
    before = time.monotonic()
    report = None
    try:
        vm = run('start')
        if vm['uuid'] != expected_uuid:
            raise RuntimeError('Started guest identity differs from the owned instance')
        while time.monotonic() - before < args.timeout:
            if console.exists():
                content = console.read_text(errors='replace')
                report = boot_report(content, args.guest, recipe['architecture'])
                if report:
                    break
            time.sleep(1)
        if not report:
            raise TimeoutError('Guest userspace readiness proof timed out')
        elapsed = time.monotonic() - before
        lifecycle = [run('pause'), run('resume'), run('describe')]
        if [row['state'] for row in lifecycle] != ['paused', 'running', 'running']:
            raise AssertionError('QMP lifecycle state mismatch')
        result = {'passed': True, 'guest': args.guest, 'uuid': vm['uuid'],
                  'accelerator': args.accel, 'boot_seconds': round(elapsed, 3),
                  'recorded_at': datetime.now(timezone.utc).isoformat(), 'proof': report,
                  'image_manifest': str(args.image.resolve()), 'lifecycle': lifecycle,
                  'network': vm['network'], 'iiOS_guest_agent': False}
    finally:
        # Only this test's own VM is stopped. Failed evidence is retained too.
        protect_cleanup_from_interrupts()
        if console.exists():
            (output / 'console.log').write_bytes(console.read_bytes())
        stopped = stop_owned(run, expected_uuid)
        (output / 'stopped.json').write_text(json.dumps(stopped, indent=2) + '\n')
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
