#!/usr/bin/env python3
"""Check Windows build VM reuse isolation without starting a virtual machine."""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from guest_build import check_container_memory, check_windows_resources, validate_windows_reuse

safe = {'state': 'stopped', 'architecture': 'x86_64', 'boot_mode': 'disk',
        'guest_channel': 'serial', 'network': False, 'boot_iso': 'installer.iso',
        'payload_iso': 'job.iso', 'boot_disk_first': True,
        'cpus': 2, 'memory_mib': 1024, 'disk_mib': 49152}
validate_windows_reuse(safe)
try:
    validate_windows_reuse(safe, require_direct_io=True)
except ValueError:
    pass
else:
    raise AssertionError('Reuse without the requested direct I/O was accepted')
validate_windows_reuse(dict(safe, direct_io=True), require_direct_io=True)
for key, value in [('state', 'running'), ('architecture', 'aarch64'), ('network', True),
                   ('guest_channel', 'virtio'), ('boot_mode', 'kernel'),
                   ('payload_iso', ''), ('boot_disk_first', False),
                   ('cpus', 5), ('memory_mib', 8193), ('disk_mib', 98305),
                   ('memory_mib', None), ('cpus', True)]:
    try:
        validate_windows_reuse(dict(safe, **{key: value}))
    except ValueError:
        pass
    else:
        raise AssertionError('Unsafe Windows build VM accepted: ' + key)
check_container_memory(4096, 'MemTotal:       5242880 kB\n', 'max')
check_container_memory(4096, 'MemTotal:       5242880 kB\n', str(5 * 1024**3))
check_windows_resources(2, 1024, 32768)
for cpu, memory, disk in [(2, 1023, 32768), (2, 8193, 32768), (0, 1024, 32768),
                          (2, 1024, 32767), (True, 1024, 32768), (2, None, 32768)]:
    try:
        check_windows_resources(cpu, memory, disk)
    except ValueError:
        pass
    else:
        raise AssertionError('Out-of-range Windows resources were accepted')
for info, limit in [('MemTotal: 4194304 kB\n', 'max'),
                    ('MemTotal: 5242880 kB\n', str(4 * 1024**3)), ('missing', 'max')]:
    try:
        check_container_memory(4096, info, limit)
    except ValueError:
        pass
    else:
        raise AssertionError('Insufficient or unknown container memory was accepted')
if os.name == 'posix':
    # A signal during the potentially long media copy must terminate the owned
    # child command, even though no guest has started yet.
    resources = Path(__file__).resolve().parents[1] / 'resources'
    with tempfile.TemporaryDirectory(dir=resources) as directory:
        root = Path(directory)
        marker = root / 'child.pid'
        argv_file = root / 'child-argv.json'
        fake = root / 'fake-runtime'
        fake.write_text('#!/usr/bin/env python3\nimport json, os, pathlib, sys, time\n'
                        'pathlib.Path(os.environ["IIV_FAKE_ARGV"]).write_text(json.dumps(sys.argv))\n'
                        'pathlib.Path(os.environ["IIV_FAKE_MARKER"]).write_text(str(os.getpid()))\n'
                        'time.sleep(60)\n')
        fake.chmod(0o700)
        job = root / 'job.json'
        job.write_text(json.dumps({'job': 'fixture', 'steps': [], 'artifacts': []}))
        script = r'''import sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import guest_build
from test_isolation import TestInterrupted
guest_build.require_test_container=lambda _accel: None
original=Path.iterdir
def listing(path):
    if str(path)=="/sys/class/net": return iter([Path("/sys/class/net/lo")])
    return original(path)
Path.iterdir=listing
sys.argv=['guest_build.py','--guest','windows','--runtime',sys.argv[2],
          '--qemu','/unused','--id','interrupt-proof','--job',sys.argv[3],
          '--boot-iso','/unused','--payload-iso','/unused',
          '--root',sys.argv[4], '--output',sys.argv[5], '--direct-io', '--media-copy-rate', '8',
          '--tcg-cache-mib', '64', '--memory-mib', '1280']
try: guest_build.main()
except TestInterrupted: print('IIV_CREATE_INTERRUPTED')
'''
        env = dict(os.environ, IIV_FAKE_MARKER=str(marker), IIV_FAKE_ARGV=str(argv_file))
        child = subprocess.Popen([sys.executable, '-B', '-c', script,
                                  str(Path(__file__).resolve().parents[1] / 'tools'), str(fake),
                                  str(job), str(root / 'vms'), str(root / 'result')],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            deadline = time.monotonic() + 15
            while not marker.exists() and child.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            assert marker.exists(), 'Fake media copy did not start'
            argv = json.loads(argv_file.read_text())
            assert '--direct-io' in argv and argv[argv.index('--media-copy-rate')+1] == '8'
            assert argv[argv.index('--tcg-cache-mib')+1] == '64'
            assert argv[argv.index('--memory')+1] == '1280'
            fake_pid = int(marker.read_text())
            child.send_signal(signal.SIGTERM)
            stdout, stderr = child.communicate(timeout=15)
            assert child.returncode == 0 and 'IIV_CREATE_INTERRUPTED' in stdout, stderr
            try:
                os.kill(fake_pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError('Interrupted media-copy child survived')
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
print('Windows build VM reuse isolation contracts passed')
