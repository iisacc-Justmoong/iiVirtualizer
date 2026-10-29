#!/usr/bin/env python3
"""Prepare pinned Debian/Android verification guests beneath build/."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tarfile

from prepare_guest import cpio_bytes, unpack_cpio

ROOT = Path(__file__).resolve().parents[1]


def generated_path(path):
    path = Path(path).resolve()
    if not any(directory in path.parents for directory in (ROOT / 'build', ROOT / 'resources')):
        raise ValueError('Guest files must remain beneath project build/ or resources/')
    return path


def digest(path, algorithm='sha256'):
    value = hashlib.new(algorithm)
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def verify(path, source):
    for algorithm, expected in [(source['checksum_algorithm'], source['checksum']),
                                ('sha256', source['sha256'])]:
        if digest(path, algorithm) != expected:
            raise ValueError('Upstream image checksum mismatch: ' + str(path))


def linux_partition(stream, image_size):
    """Return the bounded Linux filesystem partition from a GPT disk."""
    stream.seek(512)
    header = stream.read(92)
    if len(header) != 92 or header[:8] != b'EFI PART':
        raise ValueError('Expected GPT cloud disk')
    table, count, entry_size = struct.unpack_from('<QII', header, 72)
    if not 1 <= count <= 4096 or not 128 <= entry_size <= 4096:
        raise ValueError('Invalid GPT entry bounds')
    if table * 512 + count * entry_size > image_size:
        raise ValueError('GPT table extends beyond image')
    stream.seek(table * 512)
    linux_guids = {bytes.fromhex('af3dc60f838472478e793d69d8477de4'),
                   bytes.fromhex('45b021b9f01dc341af444c6f280d3fae'),
                   bytes.fromhex('e3bc684fcde8b14d96e7fbcaf984b709')}
    partitions = []
    for _ in range(count):
        entry = stream.read(entry_size)
        if entry[:16] in linux_guids:
            first, last = struct.unpack_from('<QQ', entry, 32)
            if first < 34 or last < first or (last + 1) * 512 > image_size:
                raise ValueError('Invalid GPT Linux partition bounds')
            partitions.append((first * 512, (last - first + 1) * 512))
    if len(partitions) != 1:
        raise ValueError('Expected exactly one Linux root partition')
    return partitions[0]


def extract_partition_stream(stream, image_size, output):
    start, length = linux_partition(stream, image_size)
    stream.seek(start)
    with output.open('wb') as target:
        remaining = length
        while remaining:
            data = stream.read(min(8 * 1024 * 1024, remaining))
            if not data:
                raise ValueError('Truncated root partition')
            if data.count(0) == len(data):
                target.seek(len(data), 1)
            else:
                target.write(data)
            remaining -= len(data)
        target.truncate(length)


def extract_partition(image, output):
    with image.open('rb') as stream:
        extract_partition_stream(stream, image.stat().st_size, output)


def extract_partition_archive(archive, output):
    # Read the sparse disk as a bounded stream. This avoids first materializing
    # another complete disk image on the same storage device.
    with tarfile.open(archive, 'r:xz') as package:
        member = package.next()
        if member is None or member.name != 'disk.raw' or not member.isfile():
            raise ValueError('Expected a regular disk.raw as the first archive member')
        stream = package.extractfile(member)
        if stream is None:
            raise ValueError('Cannot read archive disk')
        with stream:
            extract_partition_stream(stream, member.size, output)


def debugfs(tool, image, command, write=False):
    # Paths are quoted for debugfs's own parser; subprocess never invokes a shell.
    result = subprocess.run([str(tool), *(['-w'] if write else []), '-R', command,
                             str(image)], check=True, capture_output=True, text=True)
    errors = ('File not found', 'not found by ext2_lookup', 'while opening',
              'while reading', 'while writing', 'Filesystem not open',
              'Command not found', 'Ext2 inode is not a directory')
    if any(error in result.stderr for error in errors):
        raise RuntimeError(result.stderr)
    return result.stdout


def quote(path):
    text = str(path)
    if any(c in text for c in '\n\r\0"\\'):
        raise ValueError('Unsupported debugfs path')
    return '"' + text + '"'


def debian_console(architecture):
    consoles = {'aarch64': 'ttyAMA0', 'x86_64': 'ttyS0'}
    if architecture not in consoles:
        raise ValueError('Unsupported Debian architecture')
    return consoles[architecture]


def prepare_debian(archive, output, tool, architecture='aarch64'):
    console = debian_console(architecture)
    image = output / 'root.raw'
    extract_partition_archive(archive, image)
    listing = debugfs(tool, image, 'ls -l /boot')
    versions = re.findall(r'\bvmlinuz-([^\s]+)', listing)
    if len(versions) != 1:
        raise ValueError('Expected one Debian cloud kernel')
    version = versions[0]
    for source, target in [('vmlinuz-' + version, 'Image'), ('initrd.img-' + version, 'initramfs')]:
        debugfs(tool, image, 'dump /boot/' + source + ' ' + quote(output / target))
        if not (output / target).is_file() or (output / target).stat().st_size == 0:
            raise RuntimeError('Kernel extraction failed')
    fstab = output / 'fstab'
    # The extracted root partition becomes the instance's complete virtio disk.
    fstab.write_text('/dev/vda / ext4 defaults 0 1\n')
    debugfs(tool, image, 'rm /etc/fstab', True)
    debugfs(tool, image, 'write ' + quote(fstab) + ' /etc/fstab', True)
    for source, target in [('debian-boot-probe.sh', '/usr/local/sbin/iiv-boot-probe'),
                           ('debian-boot-probe.service', '/etc/systemd/system/iiv-boot-probe.service')]:
        debugfs(tool, image, 'write ' + quote(ROOT / 'example' / 'guest-os' / source) + ' ' + target, True)
    debugfs(tool, image, 'symlink /etc/systemd/system/multi-user.target.wants/iiv-boot-probe.service '
            '/etc/systemd/system/iiv-boot-probe.service', True)
    return {'architecture': architecture, 'kernel_version': version, 'memory_mib': 768,
            'cpus': 2, 'disk_mib': 3072, 'base_disk': str(image),
            'kernel': str(output / 'Image'), 'initramfs': str(output / 'initramfs'),
            'kernel_command_line': 'console=' + console + ',115200 root=/dev/vda rw rootwait '
                                   'cloud-init=disabled systemd.hostname=iiv-debian13',
            'graphics': 'none'}


def prepare_android(iso, output, mke2fs):
    upstream = output / 'upstream'
    upstream.mkdir()
    subprocess.run(['bsdtar', '-xf', str(iso), '-C', str(upstream),
                    'kernel', 'initrd.img', 'ramdisk.img', 'system.sfs'], check=True)
    entries = unpack_cpio(gzip.decompress((upstream / 'ramdisk.img').read_bytes()))
    # TCG should spend its CPU budget on Android initialization. These standard
    # guest properties retain real ART verification and omit the boot animation.
    prop_mode, properties = entries['default.prop']
    properties = re.sub(rb'(?m)^pm\.dexopt\.first-boot=.*$', b'pm.dexopt.first-boot=verify', properties)
    properties += b'\ndebug.sf.nobootanimation=1\n'
    entries['default.prop'] = (prop_mode, properties)
    entries['iiv-boot-probe.sh'] = (stat.S_IFREG | 0o755,
        (ROOT / 'example' / 'guest-os' / 'android-boot-probe.sh').read_bytes())
    mode, init = entries['init.rc']
    init += b'''\n# iiVirtualizer records observed guest state.
on boot
    start iiv-boot-probe

service iiv-boot-probe /system/bin/sh /iiv-boot-probe.sh
    user root
    group root log readproc
    disabled
    oneshot
    seclabel u:r:init:s0
'''
    entries['init.rc'] = (mode, init)
    folder = output / 'root' / 'android'
    folder.mkdir(parents=True)
    (folder / 'data').mkdir()
    (folder / 'ramdisk.img').write_bytes(gzip.compress(
        cpio_bytes([(name, *value) for name, value in entries.items()]), mtime=0))
    shutil.copyfile(upstream / 'system.sfs', folder / 'system.sfs')
    image = output / 'android.raw'
    with image.open('wb') as stream:
        stream.truncate(4 * 1024**3)
    subprocess.run([str(mke2fs), '-q', '-t', 'ext4', '-F', '-b', '4096', '-I', '256',
        '-O', '^64bit,^metadata_csum,^orphan_file', '-E', 'lazy_itable_init=0',
        '-L', 'IIV_ANDROID', '-d', str(folder.parent), str(image)], check=True)
    return {'architecture': 'x86_64', 'memory_mib': 3072, 'cpus': 4, 'disk_mib': 4096,
            'base_disk': str(image), 'kernel': str(upstream / 'kernel'),
            'initramfs': str(upstream / 'initrd.img'), 'graphics': 'virtio-gpu',
            'kernel_command_line': 'console=ttyS0,115200 video=Virtual-1:640x480@60 root=/dev/ram0 SRC=/android '
                'DATA=data SETUPWIZARD=0 androidboot.hardware=android_x86_64 '
                'androidboot.selinux=permissive HWACCEL=0'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('guest', choices=['debian', 'android'])
    parser.add_argument('--arch', choices=['aarch64', 'x86_64'], default='aarch64',
                        help='Debian guest CPU architecture')
    parser.add_argument('--source', type=Path, help='Verified local archive; otherwise download the pinned source')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--debugfs', type=Path,
        default=ROOT / 'build/dependencies/e2fsprogs-1.47.2/build/debugfs/debugfs')
    parser.add_argument('--mke2fs', type=Path, default=Path(shutil.which('mke2fs') or 'mke2fs'))
    args = parser.parse_args()
    output = generated_path(args.output)
    if output.exists():
        parser.error('Output already exists; select a fresh build/ directory')
    sources = json.loads((ROOT / 'tools' / 'os_sources.json').read_text())
    source = sources[('debian_13_arm64' if args.arch == 'aarch64' else 'debian_13_amd64')
                     if args.guest == 'debian' else 'android_x86_64_9_0_r2']
    archive = args.source
    if archive is None:
        archive = ROOT / 'resources' / 'platform-guests' / source['filename']
        archive.parent.mkdir(parents=True, exist_ok=True)
        if not archive.exists():
            partial = archive.with_suffix(archive.suffix + '.partial')
            subprocess.run(['curl', '--fail', '--location', '--retry', '2', '--output',
                            str(partial), source['url']], check=True)
            verify(partial, source)
            partial.replace(archive)
    verify(archive, source)
    output.mkdir(parents=True)
    recipe = prepare_debian(archive.resolve(), output, args.debugfs.resolve(), args.arch) if args.guest == 'debian' else prepare_android(archive.resolve(), output, args.mke2fs.resolve())
    recipe['source'] = source
    recipe['artifact_sha256'] = {key: digest(recipe[key]) for key in ['kernel', 'initramfs', 'base_disk']}
    (output / 'image.json').write_text(json.dumps(recipe, indent=2) + '\n')
    print(json.dumps(recipe, indent=2))


if __name__ == '__main__':
    main()
