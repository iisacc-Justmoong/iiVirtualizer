#!/usr/bin/env python3
"""Atomically update a stopped VM's initramfs without changing its disk or UUID."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def direct(path, directory=False):
    if path.is_symlink() or path.resolve(strict=True) != path or (not path.is_dir() if directory else not path.is_file()):
        raise ValueError('Guest upgrade paths must be direct canonical entries')


def upgrade(identity, image, state, runtime):
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,31}', identity):
        raise ValueError('Invalid instance ID')
    state, image, runtime = [Path(os.path.abspath(value)) for value in (state, image, runtime)]
    if not state.is_relative_to(ROOT / 'build'):
        raise ValueError('Upgrade instance state must remain inside this project build/')
    direct(state, True);direct(image, True)
    directory = state / identity;direct(directory, True)
    for path in [directory / 'instance.json', directory / 'kernel', directory / 'initramfs', directory / 'data.raw', image / 'image.json', image / 'Image', image / 'initramfs.cpio.gz']:
        direct(path)
    manifest = json.loads((image / 'image.json').read_text())
    if manifest.get('filesystem_profile') != 'society-v1' or not manifest.get('sandbox_sha256'):
        raise ValueError('The source image must include the enforced Society workload profile')
    if digest(image / 'Image') != manifest['kernel_sha256'] or digest(image / 'initramfs.cpio.gz') != manifest['initramfs_sha256']:
        raise ValueError('Source boot image checksum mismatch')
    if digest(directory / 'kernel') != manifest['kernel_sha256']:
        raise ValueError('Kernel changes require a separate reviewed image migration')
    observed = subprocess.run([str(runtime), 'describe', identity, '--root', str(state)], capture_output=True, text=True, timeout=30)
    if observed.returncode or json.loads(observed.stdout).get('state') != 'stopped':
        raise ValueError('Stop the owned VM before updating its boot image')
    descriptor = os.open(state / ('.vm-' + identity + '.lock'), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    temporary = directory / ('.initramfs-upgrade-' + uuid.uuid4().hex)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # The runtime uses the same stable per-instance lock. Recheck process
        # artifacts after acquiring it, so a start between observation and lock
        # acquisition can never race the replacement. Stale artifacts fail safe.
        for name in ('qemu.pid', 'qmp.sock', 'agent.sock'):
            if os.path.lexists(directory / name):
                raise ValueError('VM process artifacts remain; inspect the instance before upgrading')
        before = json.loads((directory / 'instance.json').read_text())
        if before.get('schema') != 1 or before.get('id') != identity or before.get('uuid') != json.loads(observed.stdout)['uuid']:
            raise ValueError('Instance identity changed before upgrade')
        old_hash, new_hash = digest(directory / 'initramfs'), manifest['initramfs_sha256']
        volume_hash = digest(directory / 'data.raw')
        result = {'instance': identity, 'uuid': before['uuid'], 'filesystem_profile': 'society-v1', 'changed': old_hash != new_hash,
                  'old_initramfs_sha256': old_hash, 'new_initramfs_sha256': new_hash, 'volume_sha256': volume_hash, 'data_unchanged': True}
        if old_hash == new_hash:
            return result
        token = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + identity + '-' + uuid.uuid4().hex[:6]
        backup = ROOT / 'build/guest-upgrades' / token
        backup.mkdir(parents=True, mode=0o700)
        shutil.copyfile(directory / 'initramfs', backup / 'initramfs.before')
        shutil.copyfile(directory / 'instance.json', backup / 'instance.before.json')
        (backup / 'image.after.json').write_text(json.dumps(manifest, indent=2) + '\n')
        result['backup'] = str(backup)
        shutil.copyfile(image / 'initramfs.cpio.gz', temporary)
        temporary.chmod(stat.S_IMODE((directory / 'initramfs').stat().st_mode))
        with temporary.open('rb') as file:
            os.fsync(file.fileno())
        if digest(temporary) != new_hash:
            raise ValueError('Staged initramfs checksum mismatch')
        os.replace(temporary, directory / 'initramfs')
        parent = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent)
        finally:
            os.close(parent)
        if digest(directory / 'data.raw') != volume_hash or json.loads((directory / 'instance.json').read_text()) != before:
            raise RuntimeError('Instance data changed outside the upgrade operation')
        (backup / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        return result
    finally:
        temporary.unlink(missing_ok=True)
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('instance')
    parser.add_argument('--image', type=Path, default=ROOT / 'build/images/aarch64')
    parser.add_argument('--root', type=Path, default=ROOT / 'build/instances')
    parser.add_argument('--runtime', type=Path, default=ROOT / 'build/iivirtualizer')
    args = parser.parse_args()
    try:
        print(json.dumps(upgrade(args.instance, args.image, args.root, args.runtime)))
    except Exception as error:
        print('Guest upgrade blocked: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
