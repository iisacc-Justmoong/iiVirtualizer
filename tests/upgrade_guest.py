#!/usr/bin/env python3
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('guest_upgrade', ROOT / 'tools/upgrade_guest.py')
module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
runtime = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve() / str(time.time_ns())[-6:]
root.mkdir(parents=True)
image = root / 'image';image.mkdir()
(image / 'Image').write_bytes(b'kernel-fixture')
(image / 'initramfs.cpio.gz').write_bytes(b'old-initramfs')
state = root / 'state'
created = subprocess.run([str(runtime), 'create', 'demo', '--kernel', str(image / 'Image'), '--initramfs', str(image / 'initramfs.cpio.gz'), '--disk', '16', '--root', str(state)], capture_output=True, text=True, timeout=30)
assert created.returncode == 0, created.stderr
before = json.loads(created.stdout)
(image / 'initramfs.cpio.gz').write_bytes(b'new-profile-initramfs')
metadata = {'filesystem_profile': 'society-v1', 'sandbox_sha256': 'test-launcher', 'kernel_sha256': module.digest(image / 'Image'), 'initramfs_sha256': module.digest(image / 'initramfs.cpio.gz')}
(image / 'image.json').write_text(json.dumps(metadata))
volume = module.digest(state / 'demo/data.raw')
result = module.upgrade('demo', image, state, runtime)
assert result['changed'] and result['data_unchanged'] and result['uuid'] == before['uuid']
assert (state / 'demo/initramfs').read_bytes() == b'new-profile-initramfs'
assert (Path(result['backup']) / 'initramfs.before').read_bytes() == b'old-initramfs'
assert module.digest(state / 'demo/data.raw') == volume
assert not module.upgrade('demo', image, state, runtime)['changed']

def rejected():
    try:
        module.upgrade('demo', image, state, runtime)
    except (ValueError, BlockingIOError):
        pass
    else:
        raise AssertionError('Unsafe guest upgrade accepted')
    assert module.digest(state / 'demo/data.raw') == volume
    assert (state / 'demo/initramfs').read_bytes() == b'new-profile-initramfs'

(image / 'Image').write_bytes(b'wrong-kernel')
rejected()
(image / 'Image').write_bytes(b'kernel-fixture')
with (state / '.vm-demo.lock').open('r+') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    rejected()
(state / 'demo/qemu.pid').write_text(str(os.getpid()))
rejected()
(state / 'demo/qemu.pid').unlink()
target = state / 'demo/initramfs';target.rename(state / 'demo/old');target.symlink_to(state / 'demo/old')
rejected()
print('Stopped image upgrade, backup, identity/data preservation, idempotence and race guards passed')
