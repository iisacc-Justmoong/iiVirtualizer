#!/usr/bin/env python3
"""Install and run a native Windows ARM probe in an isolated TCG test container."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_os_guests import digest, generated_path
from resumable_digest import verify_fd
from test_isolation import install_interrupt_handlers, protect_cleanup_from_interrupts, require_test_container
from windows_native_proof import parse_report, pe_architecture, validate_payload


def resource_path(path):
    path = Path(path).resolve(strict=False)
    resources = (ROOT / 'resources').resolve(strict=True)
    if resources not in path.parents:
        raise ValueError('Windows guest inputs and instances must be beneath resources/')
    return path


def tpm_command(executable, state, control):
    if len(os.fsencode(control)) >= 108:
        raise ValueError('TPM control socket path is too long')
    return [str(executable), 'socket', '--tpm2', '--tpmstate', 'dir=' + str(state),
            '--ctrl', 'type=unixio,path=' + str(control), '--terminate']


def stop_owned(run, expected_uuid):
    state = run('describe')
    if state['uuid'] != expected_uuid:
        raise RuntimeError('Guest identity changed; refusing to stop it')
    if state['state'] in ('running', 'paused'):
        state = run('stop', '--force')
    if state['state'] != 'stopped' or state['uuid'] != expected_uuid:
        raise RuntimeError('Owned Windows ARM guest did not stop')
    return state


def verify_installer(installer, expected, checkpoint=None):
    if checkpoint is None:
        if digest(installer) != expected:
            raise ValueError('Windows installation media checksum mismatch')
        return
    import fcntl
    fd = os.open(installer, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        verify_fd(fd, checkpoint, expected)
    finally:
        os.close(fd)


def prepared_record(vm, root, binding, durable=False):
    expected = {'state': 'stopped', 'architecture': 'aarch64', 'accelerator': 'tcg',
                'cpu_model': binding['cpu_model'], 'cpus': 2, 'memory_mib': binding['memory_mib'],
                'disk_mib': binding['disk_mib'], 'network': False, 'boot_mode': 'disk',
                'disk_bus': 'nvme', 'guest_channel': 'serial', 'graphics': 'ramfb',
                'external_tpm': True, 'allow_reboot': True, 'boot_disk_first': True}
    identifier = vm.get('id', '')
    if (not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?', identifier) or
            not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', vm.get('uuid', '')) or
            any(vm.get(key) != value for key, value in expected.items())):
        raise ValueError('Prepared VM does not match the isolated Windows ARM profile')
    root = Path(root).resolve(strict=True)
    directory = root / identifier
    if (directory.is_symlink() or not directory.is_dir() or
            Path(vm.get('directory', '')).resolve(strict=True) != directory):
        raise ValueError('Prepared VM directory does not match its owned root')
    files = {}
    sizes = {'data.raw': binding['disk_mib'] * 1024**2, 'boot_iso': binding['installer_size'],
             'payload_iso': binding['payload_size'], 'firmware': binding['firmware_size'],
             'firmware_vars': binding['firmware_vars_size']}
    hashes = {'payload_iso': binding['payload_sha256'], 'firmware': binding['firmware_sha256'],
              'firmware_vars': binding['firmware_vars_sha256']}
    for name in ('data.raw', 'boot_iso', 'payload_iso', 'firmware', 'firmware_vars'):
        fd = os.open(directory / name, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size != sizes[name]:
                raise ValueError('Prepared VM file is not regular or has the wrong size')
            if durable:
                os.fsync(fd)
                info = os.fstat(fd)
            if name in hashes:
                checksum = hashlib.sha256()
                while True:
                    block = os.read(fd, 1024 * 1024)
                    if not block:
                        break
                    checksum.update(block)
                after = os.fstat(fd)
                if (checksum.hexdigest() != hashes[name] or
                        (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) !=
                        (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)):
                    raise ValueError('Prepared VM media changed or failed checksum validation')
            files[name] = {'device': info.st_dev, 'inode': info.st_ino, 'size': info.st_size,
                           'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns}
        finally:
            os.close(fd)
    return {'schema': 1, 'id': identifier, 'uuid': vm['uuid'], 'root': str(root),
            'binding': binding, 'files': files}


def validate_prepared(vm, root, binding, recorded):
    if not isinstance(recorded, dict) or prepared_record(vm, root, binding) != recorded:
        raise ValueError('Prepared Windows ARM VM or its files changed')
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime', 'installer', 'payload-iso', 'payload-manifest', 'binary', 'firmware',
                 'firmware-vars', 'swtpm', 'isoinfo', 'root', 'output', 'qemu'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--installer-sha256', required=True)
    parser.add_argument('--installer-verification-checkpoint', type=Path)
    parser.add_argument('--isoinfo-lib-dir', type=Path)
    parser.add_argument('--id', required=True)
    parser.add_argument('--timeout', type=int, default=14400)
    parser.add_argument('--startup-timeout', type=int, default=180)
    parser.add_argument('--control-timeout', type=int, default=60)
    parser.add_argument('--memory-mib', type=int, default=4096)
    parser.add_argument('--disk-mib', type=int, default=65536)
    parser.add_argument('--cpu-model', default='max,pauth-impdef=on')
    phase = parser.add_mutually_exclusive_group()
    phase.add_argument('--prepare-only', action='store_true', help='Copy and validate inputs without starting a VM')
    phase.add_argument('--resume-prepared', action='store_true', help='Start a previously prepared matching VM')
    args = parser.parse_args()
    require_test_container('tcg')
    install_interrupt_handlers()
    if sorted(os.listdir('/sys/class/net')) != ['lo']:
        raise RuntimeError('The VM test container must have no network')
    if not re.fullmatch(r'[0-9a-f]{64}', args.installer_sha256):
        raise ValueError('Expected a lowercase installer SHA-256')
    if not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?', args.id):
        raise ValueError('Invalid owned Windows VM identifier')
    if not 4096 <= args.memory_mib <= 8192 or not 65536 <= args.disk_mib <= 131072 or args.timeout < 1:
        raise ValueError('Windows ARM resource limits are outside the test profile')
    memory_limit = Path('/sys/fs/cgroup/memory.max').read_text().strip()
    if not args.prepare_only and memory_limit != 'max' and int(memory_limit) < (args.memory_mib + 512) * 1024**2:
        raise RuntimeError('Container memory limit must leave at least 512 MiB beyond guest RAM')
    root = resource_path(args.root)
    output = generated_path(args.output)
    inputs = [resource_path(p) for p in (args.installer, args.payload_iso, args.payload_manifest,
                                         args.binary, args.firmware, args.firmware_vars)]
    installer, payload_iso, manifest_file, binary, firmware, firmware_vars = inputs
    if not all(path.is_file() for path in inputs):
        raise ValueError('All Windows test inputs must be regular files')
    if not all(p.is_file() for p in (args.runtime, args.qemu, args.swtpm, args.isoinfo)):
        raise ValueError('The runtime, QEMU, TPM emulator and ISO inspector must be regular files')
    if output.exists() or ((root / args.id).exists() != args.resume_prepared):
        raise ValueError('Choose a fresh evidence directory and an appropriate owned VM identifier')
    root.mkdir(parents=True, exist_ok=True)
    free = os.statvfs(root).f_bavail * os.statvfs(root).f_frsize
    required = args.disk_mib * 1024**2 + 2 * 1024**3
    if not args.resume_prepared:
        required += installer.stat().st_size + payload_iso.stat().st_size
    if free < required:
        raise RuntimeError('The private test host disk does not have enough free space')
    verify_installer(installer, args.installer_sha256, args.installer_verification_checkpoint)
    manifest = json.loads(manifest_file.read_text())
    nonce = manifest.get('nonce', '')
    if (manifest.get('binary_architecture') != 'aarch64' or pe_architecture(binary) != 'aarch64' or
            digest(binary) != manifest.get('binary_sha256') or not re.fullmatch(r'[0-9a-f]{32}', nonce)):
        raise ValueError('Windows probe does not match the ARM payload manifest')
    iso_environment = os.environ.copy()
    if args.isoinfo_lib_dir:
        iso_environment['LD_LIBRARY_PATH'] = str(args.isoinfo_lib_dir.resolve())
    def iso_extract(name, limit):
        completed = subprocess.run([str(args.isoinfo.resolve()), '-J', '-i', str(payload_iso), '-x', name],
                                   env=iso_environment, capture_output=True, timeout=300, check=True)
        if len(completed.stdout) > limit:
            raise ValueError('ISO payload entry exceeds its size limit')
        return completed.stdout
    image_binary = iso_extract('/Payload/windows-native-probe.exe', 32 * 1024 * 1024)
    image_answer = iso_extract('/Autounattend.xml', 1024 * 1024)
    if not validate_payload(image_binary, image_answer, manifest):
        raise ValueError('ISO payload does not match the native binary and answer manifest')
    output.mkdir(parents=True)
    commands = []
    def run(command, *options):
        argv = [str(args.runtime.resolve()), command, args.id, '--root', str(root),
                '--qemu-aarch64', str(args.qemu.resolve()), '--startup-timeout', str(args.startup_timeout),
                '--control-timeout', str(args.control_timeout), *map(str, options)]
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=14400 if command == 'create' else 1800)
        commands.append({'argv': argv, 'returncode': completed.returncode,
                         'stdout': completed.stdout, 'stderr': completed.stderr})
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if completed.returncode:
            raise RuntimeError('VM command failed: ' + completed.stderr)
        return json.loads(completed.stdout)

    binding = {'installer_sha256': args.installer_sha256, 'payload_sha256': digest(payload_iso),
               'binary_sha256': manifest['binary_sha256'], 'firmware_sha256': digest(firmware),
               'firmware_vars_sha256': digest(firmware_vars), 'cpu_model': args.cpu_model,
               'memory_mib': args.memory_mib, 'disk_mib': args.disk_mib,
               'installer_size': installer.stat().st_size, 'payload_size': payload_iso.stat().st_size,
               'firmware_size': firmware.stat().st_size, 'firmware_vars_size': firmware_vars.stat().st_size}
    marker = root / args.id / '.iiv-arm-prepared.json'
    if args.resume_prepared:
        created = run('describe')
        fd = os.open(marker, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
                raise ValueError('Invalid prepared VM marker')
            recorded = json.loads(os.read(fd, info.st_size))
        finally:
            os.close(fd)
        validate_prepared(created, root, binding, recorded)
    else:
        created = run('create', '--arch', 'aarch64', '--accel', 'tcg', '--boot', 'disk',
                      '--boot-iso', installer, '--payload-iso', payload_iso, '--firmware', firmware,
                      '--firmware-vars', firmware_vars, '--disk-bus', 'nvme', '--guest-channel', 'serial',
                      '--graphics', 'ramfb', '--allow-reboot', '--boot-disk-first', '--external-tpm', '--cpus', '2',
                      '--cpu-model', args.cpu_model, '--memory', args.memory_mib, '--disk', args.disk_mib)
        if args.installer_verification_checkpoint is not None:
            verify_installer(installer, args.installer_sha256, args.installer_verification_checkpoint)
    expected_uuid = created['uuid']
    plan = run('plan')
    if (plan[plan.index('-cpu') + 1] != args.cpu_model or plan[plan.index('-accel') + 1] != 'tcg' or
            plan[plan.index('-nic') + 1] != 'none' or
            not any(value.startswith('nvme,drive=data,serial=') and value.endswith(',bootindex=1') for value in plan) or
            'usb-storage,bus=usb.0,drive=bootiso,bootindex=2' not in plan):
        raise RuntimeError('Unexpected VM CPU, acceleration, boot order or network plan')
    if args.prepare_only:
        record = prepared_record(created, root, binding, durable=True)
        fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        try:
            data = (json.dumps(record, indent=2) + '\n').encode()
            written = 0
            while written < len(data):
                count = os.write(fd, data[written:])
                if count <= 0:
                    raise OSError('Could not write prepared VM marker')
                written += count
            os.fsync(fd)
        finally:
            os.close(fd)
        fd = os.open(marker.parent, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        result = {'prepared': True, 'uuid': created['uuid'], 'id': args.id, 'marker': str(marker)}
        (output / 'prepared.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        return
    vm_directory = Path(created['directory'])
    tpm_state = vm_directory / 'tpm-state'
    tpm_state.mkdir(mode=0o700)
    tpm_socket = vm_directory / 'tpm.sock'
    tpm_log = (output / 'tpm.log').open('wb')
    tpm = None
    stopped = False
    try:
        tpm = subprocess.Popen(tpm_command(args.swtpm.resolve(), tpm_state, tpm_socket),
                               stdin=subprocess.DEVNULL, stdout=tpm_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while not tpm_socket.exists() and tpm.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if not tpm_socket.exists() or tpm.poll() is not None:
            raise RuntimeError('The owned TPM emulator did not become ready')
        vm = run('start')
        if vm['uuid'] != expected_uuid:
            raise RuntimeError('Started guest identity differs from the owned instance')
        if vm['accelerator'] != 'tcg' or vm['network']:
            raise RuntimeError('Unexpected VM acceleration or network')
        started = time.monotonic()
        data = bytearray()
        proof = None
        winpe_proof = None
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as serial, (output / 'serial.log').open('wb') as log:
            serial.settimeout(2)
            serial.connect(str(vm_directory / 'agent.sock'))
            while time.monotonic() - started < args.timeout:
                try:
                    chunk = serial.recv(65536)
                    if not chunk:
                        raise RuntimeError('Guest serial channel closed before the native probe')
                    log.write(chunk)
                    log.flush()
                    data.extend(chunk)
                    if len(data) > 8 * 1024 * 1024:
                        raise RuntimeError('Guest serial report exceeded its bound')
                    console = data.decode(errors='replace')
                    if not winpe_proof:
                        winpe_proof = parse_report(console, 'aarch64', nonce, environment='winpe')
                        if winpe_proof:
                            (output / 'winpe.json').write_text(json.dumps({
                                'passed': True, 'environment': 'Windows PE', 'proof': winpe_proof,
                                'uuid': vm['uuid'], 'recorded_at': datetime.now(timezone.utc).isoformat(),
                            }, indent=2) + '\n')
                    proof = parse_report(console, 'aarch64', nonce)
                    if proof:
                        break
                except socket.timeout:
                    continue
        if not proof:
            raise TimeoutError('Installed Windows ARM native userspace did not report readiness')
        lifecycle = [run('pause'), run('resume'), run('describe')]
        if [item['state'] for item in lifecycle] != ['paused', 'running', 'running']:
            raise RuntimeError('Windows ARM VM lifecycle mismatch')
        result = {'passed': True, 'guest': 'windows', 'architecture': 'aarch64', 'accelerator': 'tcg',
                  'recorded_at': datetime.now(timezone.utc).isoformat(), 'boot_seconds': round(time.monotonic()-started, 3),
                  'uuid': vm['uuid'], 'proof': proof, 'installer_sha256': args.installer_sha256,
                  'payload_sha256': digest(payload_iso), 'native_binary_sha256': manifest['binary_sha256'],
                  'network': False, 'lifecycle': lifecycle}
    finally:
        protect_cleanup_from_interrupts()
        state = stop_owned(run, expected_uuid)
        stopped = state['state'] == 'stopped'
        (output / 'stopped.json').write_text(json.dumps(state, indent=2) + '\n')
        if stopped and tpm is not None and tpm.poll() is None:
            tpm.terminate()
            try:
                tpm.wait(timeout=30)
            except subprocess.TimeoutExpired:
                tpm.kill()
                tpm.wait(timeout=5)
        tpm_log.close()
        if not stopped:
            raise RuntimeError('Guest may still be running; retained its TPM process and state')
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    removed = run('remove')
    (output / 'removed.json').write_text(json.dumps(removed, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
