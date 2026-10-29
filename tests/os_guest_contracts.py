#!/usr/bin/env python3
"""Reject invalid images and incomplete or forged boot markers."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_os_guests import debian_console, extract_partition_archive, generated_path, linux_partition, verify
from os_boot import boot_report, resource_root, stop_owned


def rejected(function):
    try:
        function()
    except ValueError:
        return
    raise AssertionError('Invalid guest input was accepted')


def stop_fixture(state, uuid='00000000-0000-4000-8000-000000000001'):
    calls = []
    def run(command, *options):
        calls.append((command, *options))
        return {'uuid': uuid, 'state': 'stopped' if command == 'stop' else state}
    return run, calls


owned_uuid = '00000000-0000-4000-8000-000000000001'
run, calls = stop_fixture('running')
assert stop_owned(run, owned_uuid)['state'] == 'stopped'
assert calls == [('describe',), ('stop', '--force')]
run, calls = stop_fixture('stopped')
assert stop_owned(run, owned_uuid)['state'] == 'stopped' and calls == [('describe',)]
run, calls = stop_fixture('running', '00000000-0000-4000-8000-000000000002')
try:
    stop_owned(run, owned_uuid)
except RuntimeError:
    pass
else:
    raise AssertionError('A different guest identity was stopped')
assert calls == [('describe',)]
run, calls = stop_fixture('unavailable')
try:
    stop_owned(run, owned_uuid)
except RuntimeError:
    pass
else:
    raise AssertionError('An unavailable guest was reported stopped')
assert calls == [('describe',)]


root = generated_path(Path(sys.argv[1]))
assert generated_path(Path(__file__).resolve().parents[1] / 'resources' / 'platform-guests' / 'sample')
assert resource_root(Path(__file__).resolve().parents[1] / 'resources' / 'platform-guests' / 'sample')
rejected(lambda: resource_root(root))
root.mkdir(parents=True, exist_ok=True)
fixture = root / 'checksum-fixture'
fixture.write_bytes(b'original upstream')
source = {'checksum_algorithm': 'sha512', 'checksum': hashlib.sha512(fixture.read_bytes()).hexdigest(),
          'sha256': hashlib.sha256(fixture.read_bytes()).hexdigest()}
verify(fixture, source)
fixture.write_bytes(b'altered upstream')
rejected(lambda: verify(fixture, source))
rejected(lambda: generated_path(root.parents[2] / 'outside-build'))

disk = bytearray(128 * 512)
disk[512:520] = b'EFI PART'
struct.pack_into('<QII', disk, 512 + 72, 2, 1, 128)
disk[1024:1040] = bytes.fromhex('af3dc60f838472478e793d69d8477de4')
struct.pack_into('<QQ', disk, 1024 + 32, 64, 127)
assert linux_partition(io.BytesIO(disk), len(disk)) == (64 * 512, 64 * 512)
disk[1024:1040] = bytes.fromhex('45b021b9f01dc341af444c6f280d3fae')
assert linux_partition(io.BytesIO(disk), len(disk)) == (64 * 512, 64 * 512)
disk[1024:1040] = bytes.fromhex('e3bc684fcde8b14d96e7fbcaf984b709')
assert linux_partition(io.BytesIO(disk), len(disk)) == (64 * 512, 64 * 512)
struct.pack_into('<Q', disk, 1024 + 40, 10000)
rejected(lambda: linux_partition(io.BytesIO(disk), len(disk)))
struct.pack_into('<I', disk, 512 + 80, 0xffffffff)
rejected(lambda: linux_partition(io.BytesIO(disk), len(disk)))
struct.pack_into('<I', disk, 512 + 80, 1)
struct.pack_into('<Q', disk, 1024 + 40, 127)
disk[64 * 512:64 * 512 + 12] = b'root-part-01'
archive = root / 'disk-fixture.tar.xz'
with tarfile.open(archive, 'w:xz') as package:
    member = tarfile.TarInfo('disk.raw'); member.size = len(disk)
    package.addfile(member, io.BytesIO(disk))
partition = root / 'disk-partition.raw'
extract_partition_archive(archive, partition)
assert partition.read_bytes() == bytes(disk[64 * 512:])
bad_archive = root / 'bad-disk-fixture.tar.xz'
with tarfile.open(bad_archive, 'w:xz') as package:
    member = tarfile.TarInfo('disk.raw'); member.type = tarfile.SYMTYPE; member.linkname = '/outside'
    package.addfile(member)
rejected(lambda: extract_partition_archive(bad_archive, root / 'bad-partition.raw'))

proof = ('IIV_DEBIAN_BOOT_BEGIN\nos_id=debian\nos_version=13\npid1=systemd\n'
         'multi_user=active\narchitecture=aarch64\n'
         'boot_id=00000000-0000-4000-8000-000000000001\nIIV_DEBIAN_BOOT_END\n')
assert boot_report(proof, 'debian')
assert not boot_report(proof, 'debian', 'x86_64')
proof_x86 = proof.replace('architecture=aarch64', 'architecture=x86_64')
assert boot_report(proof_x86, 'debian', 'x86_64')
assert not boot_report(proof_x86, 'debian', 'aarch64')
assert debian_console('aarch64') == 'ttyAMA0'
assert debian_console('x86_64') == 'ttyS0'
rejected(lambda: debian_console('riscv64'))
assert not boot_report(proof.replace('multi_user=active', 'multi_user=inactive'), 'debian')
assert not boot_report(proof.replace('pid1=systemd', 'pid1=sh'), 'debian')
assert not boot_report(proof.replace('IIV_DEBIAN_BOOT_END', ''), 'debian')
assert not boot_report('Kernel command line: ' + proof.replace('\n', ' '), 'debian')
android = ('IIV_ANDROID_BOOT_BEGIN\nos_id=android\nos_version=9\napi_level=28\npid1=init\n'
           'boot_id=00000000-0000-4000-8000-000000000002\nsys.boot_completed=1\n'
           'init.svc.zygote=running\ninit.svc.surfaceflinger=running\n'
           'system 123 1 1000 1000 x 0 S system_server\nIIV_ANDROID_BOOT_END\n')
assert boot_report(android, 'android')
assert not boot_report(android.replace('sys.boot_completed=1', 'sys.boot_completed=0'), 'android')
assert not boot_report(android.replace(' system_server', ' other'), 'android')

# A real termination signal must reach the same owned-guest cleanup path.
# The runtime below is a file-only fake: it never launches a VM or QEMU.
with tempfile.TemporaryDirectory(prefix='os-signal-', dir=Path(__file__).resolve().parents[1] / 'resources') as temp:
    fixture = Path(temp)
    vm_root = fixture / 'vms'
    executable = fixture / 'fake-runtime.py'
    executable.write_text('''#!/usr/bin/env python3
import json, sys
from pathlib import Path
args = sys.argv[1:]
command, name = args[:2]
folder = Path(args[args.index('--root') + 1]) / name
if command == 'create':
    folder.mkdir(parents=True)
    (folder / 'state').write_text('stopped')
elif command == 'start':
    (folder / 'state').write_text('running')
    (folder / 'console.log').touch()
elif command == 'stop':
    (folder / 'state').write_text('stopped')
state = (folder / 'state').read_text()
print(json.dumps({'uuid': '00000000-0000-4000-8000-000000000001', 'state': state,
                  'accelerator': 'tcg', 'console': str(folder / 'console.log')}))
''')
    executable.chmod(0o700)
    recipe = {'architecture': 'aarch64', 'cpus': 1, 'memory_mib': 64, 'disk_mib': 16,
              'graphics': 'none', 'kernel_command_line': 'console=ttyAMA0', 'artifact_sha256': {}}
    for key in ('kernel', 'initramfs', 'base_disk'):
        path = fixture / key
        path.write_bytes(key.encode())
        recipe[key] = str(path)
        recipe['artifact_sha256'][key] = hashlib.sha256(path.read_bytes()).hexdigest()
    image = fixture / 'image.json'
    image.write_text(json.dumps(recipe))
    output = root / ('signal-' + fixture.name)
    child_code = '''import os_boot, sys
os_boot.require_test_container = lambda _accel: None
os_boot.os.listdir = lambda _path: ['lo']
sys.argv = ['os_boot.py', *sys.argv[1:]]
os_boot.main()
'''
    child = subprocess.Popen([sys.executable, '-B', '-c', child_code, '--runtime', str(executable),
                              '--image', str(image), '--root', str(vm_root), '--output', str(output),
                              '--id', 'signal-fixture', '--guest', 'debian', '--accel', 'tcg',
                              '--qemu', str(fixture / 'unused-qemu')], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             cwd=Path(__file__).resolve().parent)
    try:
        deadline = time.monotonic() + 60
        state_file = vm_root / 'signal-fixture' / 'state'
        while time.monotonic() < deadline and child.poll() is None:
            if state_file.exists() and state_file.read_text() == 'running' and (output / 'commands.json').exists():
                try:
                    commands = json.loads((output / 'commands.json').read_text())
                except json.JSONDecodeError:
                    time.sleep(.05)
                    continue
                if any(item['argv'][1] == 'start' for item in commands):
                    break
            time.sleep(.05)
        else:
            raise AssertionError('Signal fixture did not start')
        child.terminate()
        _, error = child.communicate(timeout=30)
        assert child.returncode and state_file.read_text() == 'stopped', error.decode(errors='replace')
        assert json.loads((output / 'stopped.json').read_text())['uuid'] == owned_uuid
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=10)
print('Upstream integrity, GPT bounds, generated paths and userspace boot proof contracts passed')
