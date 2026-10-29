#!/usr/bin/env python3
"""Reject misleading or wrong-architecture Windows execution reports."""
from pathlib import Path
import contextlib
import io
import json
import struct
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from windows_native_proof import parse_report, pe_architecture, validate_payload
from prepare_windows_arm_probe import prepare

nonce = '0123456789abcdef0123456789abcdef'
report = ('IIV_WINDOWS_PROBE_BEGIN\nos=Windows\nnative_arch=aarch64\nbinary_arch=aarch64\n'
          'major=10\nminor=0\nbuild=26300\nsystem_drive=C\nnonce=' + nonce + '\nIIV_WINDOWS_PROBE_END\n')
assert parse_report(report, 'aarch64', nonce)['build'] == '26300'
assert parse_report('echo ' + report, 'aarch64', nonce) is None
assert parse_report(report, 'x86_64', nonce) is None
assert parse_report(report, 'aarch64', 'f' * 32) is None
assert parse_report(report.replace('system_drive=C', 'system_drive=X'), 'aarch64', nonce) is None
winpe_report = report.replace('system_drive=C', 'system_drive=X')
assert parse_report(winpe_report, 'aarch64', nonce, environment='winpe')['system_drive'] == 'X'
assert parse_report(report, 'aarch64', nonce, environment='winpe') is None
assert parse_report(winpe_report, 'aarch64', nonce, environment='unknown') is None
assert parse_report(report.replace('binary_arch=aarch64', 'binary_arch=x86_64'), 'aarch64', nonce) is None
assert parse_report(report.replace('major=10', 'major=10\nmajor=11'), 'aarch64', nonce) is None
assert parse_report(report.replace('IIV_WINDOWS_PROBE_END\n', ''), 'aarch64', nonce) is None

with tempfile.TemporaryDirectory(dir=Path(sys.argv[1]).resolve()) as temp:
    path = Path(temp) / 'test.exe'
    image = bytearray(128)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 60, 64)
    image[64:68] = b'PE\0\0'
    for machine, arch in [(0x8664, 'x86_64'), (0xaa64, 'aarch64')]:
        struct.pack_into('<H', image, 68, machine)
        path.write_bytes(image)
        assert pe_architecture(path) == arch
    struct.pack_into('<H', image, 68, 0x14c)
    path.write_bytes(image)
    try:
        pe_architecture(path)
    except ValueError:
        pass
    else:
        raise AssertionError('Unsupported PE architecture accepted')
    path.write_bytes(b'MZ')
    try:
        pe_architecture(path)
    except ValueError:
        pass
    else:
        raise AssertionError('Truncated PE accepted')
template = Path(__file__).resolve().parents[1] / 'example/guest-container/Autounattend-arm64.xml.in'
answer = ET.parse(template)
ns = {'u': 'urn:schemas-microsoft-com:unattend'}
components = answer.findall('.//u:component', ns)
assert components and all(component.attrib.get('processorArchitecture') == 'arm64' for component in components)
assert [node.text for node in answer.findall('.//u:CreatePartition/u:Type', ns)] == ['EFI', 'MSR', 'Primary']
assert answer.find('.//u:Disk/u:WillWipeDisk', ns).text == 'true'
assert answer.find('.//u:InstallTo/u:PartitionID', ns).text == '3'
assert answer.find('.//u:OSImage/u:InstallFrom/u:MetaData/u:Value', ns).text == '@IMAGE_INDEX@'
assert '@NONCE@' in answer.find('.//u:FirstLogonCommands/u:SynchronousCommand/u:CommandLine', ns).text
assert answer.find('.//u:UserAccounts/u:LocalAccounts/u:LocalAccount/u:Name', ns).text == 'IIVProbe'
assert answer.find('.//u:UserAccounts/u:LocalAccounts/u:LocalAccount/u:Group', ns).text == 'Administrators'
assert answer.find('.//u:AutoLogon/u:Username', ns).text == 'IIVProbe'
assert 'bootstrap-probe.cmd' in answer.find(".//u:settings[@pass='specialize']/u:component[@name='Microsoft-Windows-Deployment']/u:RunSynchronous/u:RunSynchronousCommand/u:Path", ns).text
winpe = answer.find(".//u:settings[@pass='windowsPE']/u:component[@name='Microsoft-Windows-Setup']/u:RunSynchronous/u:RunSynchronousCommand/u:Path", ns)
assert winpe is not None and 'windows-native-probe.exe @NONCE@ --winpe' in winpe.text
module_path = Path(__file__).resolve().parent / 'windows_arm_boot.py'
spec = importlib.util.spec_from_file_location('windows_arm_boot_contract', module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.resource_path(Path(__file__).resolve().parents[1] / 'resources' / 'owned-test').name == 'owned-test'

# Cleanup must never stop a different instance that reused the test's name.
calls = []
def matching_instance(command, *options):
    calls.append((command, options))
    return {'uuid': 'owned-id', 'state': 'stopped' if command == 'stop' else 'running'}
assert module.stop_owned(matching_instance, 'owned-id')['state'] == 'stopped'
assert calls == [('describe', ()), ('stop', ('--force',))]
calls.clear()
def foreign_instance(command, *options):
    calls.append((command, options))
    return {'uuid': 'foreign-id', 'state': 'running'}
try:
    module.stop_owned(foreign_instance, 'owned-id')
except RuntimeError:
    pass
else:
    raise AssertionError('Foreign Windows VM was accepted for cleanup')
assert calls == [('describe', ())]
def changed_after_stop(command, *options):
    return {'uuid': 'owned-id' if command == 'describe' else 'foreign-id',
            'state': 'running' if command == 'describe' else 'stopped'}
try:
    module.stop_owned(changed_after_stop, 'owned-id')
except RuntimeError:
    pass
else:
    raise AssertionError('Changed Windows VM identity was accepted after stop')

try:
    module.tpm_command(Path('/usr/bin/swtpm'), Path('/tmp/state'), Path('/' + 'x' * 108))
except ValueError:
    pass
else:
    raise AssertionError('Oversized TPM Unix socket path accepted')
try:
    module.resource_path(Path(__file__).resolve().parents[1] / 'build' / 'wrong-root')
except ValueError:
    pass
else:
    raise AssertionError('Windows VM resources outside resources/ accepted')
resource_root = Path(__file__).resolve().parents[1] / 'resources'
resource_root.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=resource_root) as temp:
    temp = Path(temp)
    if sys.platform in ('darwin', 'linux'):
        installer = temp / 'installer.bin'
        installer.write_bytes(b'test installer media')
        import hashlib
        media_sha = hashlib.sha256(installer.read_bytes()).hexdigest()
        verification = temp / 'installer.verify.json'
        module.verify_installer(installer, media_sha, verification)
        module.verify_installer(installer, media_sha, verification)
        installer.write_bytes(b'changed installer media')
        try:
            module.verify_installer(installer, media_sha, verification)
        except ValueError:
            pass
        else:
            raise AssertionError('Changed verified installer was accepted')
        vm_root = temp / 'instances'
        vm_dir = vm_root / 'owned-arm'
        vm_dir.mkdir(parents=True)
        for filename in ('data.raw', 'boot_iso', 'payload_iso', 'firmware', 'firmware_vars'):
            (vm_dir / filename).write_bytes(('owned-' + filename).encode())
        with (vm_dir / 'data.raw').open('r+b') as disk:
            disk.truncate(16 * 1024**2)
        fake_vm = {'id': 'owned-arm', 'uuid': '01234567-89ab-4def-8123-456789abcdef',
                   'directory': str(vm_dir), 'state': 'stopped', 'architecture': 'aarch64',
                   'accelerator': 'tcg', 'cpu_model': 'max,pauth-impdef=on', 'cpus': 2,
                   'memory_mib': 4096, 'disk_mib': 16, 'network': False, 'boot_mode': 'disk',
                   'disk_bus': 'nvme', 'guest_channel': 'serial', 'graphics': 'ramfb',
                   'external_tpm': True, 'allow_reboot': True, 'boot_disk_first': True}
        binding = {'installer_sha256': '1' * 64,
                   'payload_sha256': hashlib.sha256((vm_dir / 'payload_iso').read_bytes()).hexdigest(),
                   'binary_sha256': '3' * 64,
                   'firmware_sha256': hashlib.sha256((vm_dir / 'firmware').read_bytes()).hexdigest(),
                   'firmware_vars_sha256': hashlib.sha256((vm_dir / 'firmware_vars').read_bytes()).hexdigest(),
                   'cpu_model': 'max,pauth-impdef=on',
                   'memory_mib': 4096, 'disk_mib': 16,
                   'installer_size': (vm_dir / 'boot_iso').stat().st_size,
                   'payload_size': (vm_dir / 'payload_iso').stat().st_size,
                   'firmware_size': (vm_dir / 'firmware').stat().st_size,
                   'firmware_vars_size': (vm_dir / 'firmware_vars').stat().st_size}
        prepared = module.prepared_record(fake_vm, vm_root, binding, durable=True)
        assert module.validate_prepared(fake_vm, vm_root, binding, prepared)
        for changed_vm in (dict(fake_vm, uuid='00000000-0000-0000-0000-000000000000'),
                           dict(fake_vm, state='running'), dict(fake_vm, network=True),
                           dict(fake_vm, boot_disk_first=False)):
            try:
                module.validate_prepared(changed_vm, vm_root, binding, prepared)
            except ValueError:
                pass
            else:
                raise AssertionError('Changed prepared VM was accepted')
        try:
            module.validate_prepared(fake_vm, vm_root, dict(binding, installer_sha256='f' * 64), prepared)
        except ValueError:
            pass
        else:
            raise AssertionError('Changed prepared media was accepted')
        media = vm_dir / 'boot_iso'
        before = media.stat()
        with media.open('r+b') as stream:
            stream.write(b'X')
        os.utime(media, ns=(before.st_atime_ns, before.st_mtime_ns))
        try:
            module.validate_prepared(fake_vm, vm_root, binding, prepared)
        except ValueError:
            pass
        else:
            raise AssertionError('Changed prepared VM file was accepted')
    image = bytearray(128)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 60, 64)
    image[64:68] = b'PE\0\0'
    struct.pack_into('<H', image, 68, 0xaa64)
    binary = temp / 'arm.exe'
    binary.write_bytes(image)
    made = prepare(binary, temp / 'payload', 3)
    assert len(made['nonce']) == 32 and made['image_index'] == 3
    text = (temp / 'payload/Autounattend.xml').read_text()
    assert '@NONCE@' not in text and '@PASSWORD@' not in text and '@IMAGE_INDEX@' not in text
    assert made['nonce'] in text
    assert (temp / 'payload/Payload/windows-native-probe.exe').read_bytes() == image
    answer_bytes = (temp / 'payload/Autounattend.xml').read_bytes()
    assert validate_payload(bytes(image), answer_bytes, made)
    assert not validate_payload(bytes(image), answer_bytes.replace(b'<Username>IIVProbe</Username>', b'<Username>Wrong</Username>'), made)
    assert not validate_payload(bytes(image) + b'altered', answer_bytes, made)
    assert not validate_payload(bytes(image), answer_bytes.replace(made['nonce'].encode(), b'f' * 32), made)
    assert not validate_payload(bytes(image), answer_bytes.replace(b'--winpe', b'--wrong'), made)
    changed = dict(made, image_index=2)
    assert not validate_payload(bytes(image), answer_bytes, changed)
    bootstrap = (temp / 'payload/Payload/bootstrap-probe.cmd').read_bytes()
    assert bootstrap.endswith(b'\r\n') and b'\n' not in bootstrap.replace(b'\r\n', b'')
    if sys.platform in ('darwin', 'linux'):
        # Exercise the phase boundary using a file-only runtime double: a
        # 1 GiB preparation container must never start a guest or TPM.
        fake_tool = temp / 'fake-tool'
        fake_tool.write_bytes(b'not-executable')
        fake_installer = temp / 'phase-installer.iso'
        fake_installer.write_bytes(b'installer')
        fake_payload = temp / 'phase-payload.iso'
        fake_payload.write_bytes(b'payload')
        fake_firmware = temp / 'phase-firmware.fd'
        fake_vars = temp / 'phase-vars.fd'
        fake_firmware.write_bytes(b'code')
        fake_vars.write_bytes(b'vars')
        phase_root = temp / 'phase-vms'
        vm_dir = phase_root / 'phase-arm'
        fake_vm = {'id': 'phase-arm', 'uuid': '01234567-89ab-4def-8123-456789abcdef',
                   'directory': str(vm_dir), 'state': 'stopped', 'architecture': 'aarch64',
                   'accelerator': 'tcg', 'cpu_model': 'max,pauth-impdef=on', 'cpus': 2,
                   'memory_mib': 4096, 'disk_mib': 65536, 'network': False, 'boot_mode': 'disk',
                   'disk_bus': 'nvme', 'guest_channel': 'serial', 'graphics': 'ramfb',
                   'external_tpm': True, 'allow_reboot': True, 'boot_disk_first': True}
        commands = []
        def fake_run(argv, **_kwargs):
            if '-x' in argv:
                return SimpleNamespace(stdout=image if argv[-1].endswith('.exe') else answer_bytes)
            command = argv[1]
            commands.append(command)
            if command == 'create':
                vm_dir.mkdir(parents=True)
                for name, source in [('boot_iso', fake_installer), ('payload_iso', fake_payload),
                                     ('firmware', fake_firmware), ('firmware_vars', fake_vars)]:
                    shutil.copyfile(source, vm_dir / name)
                with (vm_dir / 'data.raw').open('wb') as disk:
                    disk.truncate(65536 * 1024**2)
                result = fake_vm
            elif command == 'describe':
                result = fake_vm
            elif command == 'plan':
                result = ['-cpu', 'max,pauth-impdef=on', '-accel', 'tcg', '-nic', 'none',
                          '-device', 'nvme,drive=data,serial=iiv01234567,bootindex=1',
                          '-device', 'usb-storage,bus=usb.0,drive=bootiso,bootindex=2']
            else:
                raise AssertionError('Unexpected VM operation: ' + command)
            return SimpleNamespace(returncode=0, stdout=json.dumps(result), stderr='')
        original_read = Path.read_text
        memory_limit = ['1073741824']
        def fake_read(path, *args, **kwargs):
            if str(path) == '/sys/fs/cgroup/memory.max':
                return memory_limit[0]
            return original_read(path, *args, **kwargs)
        arguments = ['windows_arm_boot.py', '--runtime', str(fake_tool), '--qemu', str(fake_tool),
                     '--installer', str(fake_installer), '--installer-sha256', hashlib.sha256(b'installer').hexdigest(),
                     '--installer-verification-checkpoint', str(temp / 'phase-installer.verify.json'),
                     '--payload-iso', str(fake_payload), '--payload-manifest', str(temp / 'payload/probe-manifest.json'),
                     '--binary', str(binary), '--firmware', str(fake_firmware), '--firmware-vars', str(fake_vars),
                     '--swtpm', str(fake_tool), '--isoinfo', str(fake_tool), '--root', str(phase_root),
                     '--id', 'phase-arm']
        def call_phase(extra, output):
            with patch.object(module, 'require_test_container'), patch.object(module, 'install_interrupt_handlers'), \
                 patch.object(module, 'protect_cleanup_from_interrupts'), patch.object(module.os, 'listdir', return_value=['lo']), \
                 patch.object(module.os, 'statvfs', return_value=SimpleNamespace(f_bavail=100*1024**3//4096, f_frsize=4096)), \
                 patch.object(Path, 'read_text', fake_read), patch.object(module.subprocess, 'run', fake_run), \
                 patch.object(module.subprocess, 'Popen', side_effect=OSError('simulated TPM failure')), \
                 patch.object(sys, 'argv', arguments + ['--output', str(output)] + extra), \
                 contextlib.redirect_stdout(io.StringIO()):
                module.main()
        call_phase(['--prepare-only'], temp / 'phase-preparation')
        assert commands == ['create', 'plan'] and (vm_dir / '.iiv-arm-prepared.json').is_file()
        commands.clear()
        try:
            call_phase(['--resume-prepared'], temp / 'phase-low-memory')
        except RuntimeError as error:
            assert 'memory limit' in str(error)
        else:
            raise AssertionError('Prepared VM was started without enough container memory')
        assert not commands
        memory_limit[0] = str(5 * 1024**3)
        try:
            call_phase(['--resume-prepared'], temp / 'phase-resume')
        except OSError as error:
            assert str(error) == 'simulated TPM failure'
        else:
            raise AssertionError('Simulated TPM failure did not propagate')
        assert commands == ['describe', 'plan', 'describe']
        assert json.loads((temp / 'phase-resume/stopped.json').read_text())['uuid'] == fake_vm['uuid']
    for invalid_index in (0, -1, 257):
        try:
            prepare(binary, temp / ('bad-' + str(invalid_index)), invalid_index)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid image index accepted')
    try:
        prepare(binary, temp / 'payload', 3)
    except ValueError:
        pass
    else:
        raise AssertionError('Existing payload overwritten')
    struct.pack_into('<H', image, 68, 0x8664)
    binary.write_bytes(image)
    try:
        prepare(binary, temp / 'wrong-machine', 1)
    except ValueError:
        pass
    else:
        raise AssertionError('x86 executable accepted for an ARM payload')

# A stop request during the ARM installer's early preparation must unwind the
# harness, rather than leaving an unmonitored preparation process behind.
early_stop = r'''
import importlib.util, os, signal, sys
spec = importlib.util.spec_from_file_location('windows_arm_boot_early_stop', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
from test_isolation import TestInterrupted
module.require_test_container = lambda accelerator: None
def interrupt_at_network_check(path):
    assert path == '/sys/class/net'
    os.kill(os.getpid(), signal.SIGTERM)
    raise AssertionError('SIGTERM was ignored')
module.os.listdir = interrupt_at_network_check
sys.argv = ['windows_arm_boot.py']
for name in ('runtime', 'installer', 'payload-iso', 'payload-manifest', 'binary', 'firmware',
             'firmware-vars', 'swtpm', 'isoinfo', 'root', 'output', 'qemu'):
    sys.argv.extend(['--' + name, '.'])
sys.argv.extend(['--installer-sha256', '0' * 64, '--id', 'early-stop-contract'])
try:
    module.main()
except TestInterrupted:
    print('EARLY_STOP_HANDLED')
else:
    raise AssertionError('Early stop did not interrupt the harness')
'''
if sys.platform != 'win32':
    stopped = subprocess.run([sys.executable, '-B', '-c', early_stop, str(module_path)],
                             capture_output=True, text=True, timeout=30)
    assert stopped.returncode == 0 and 'EARLY_STOP_HANDLED' in stopped.stdout, stopped.stderr
print('Windows native proof and PE architecture contracts passed')
