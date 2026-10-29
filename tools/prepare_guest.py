#!/usr/bin/env python3
"""Package a Linux kernel and an iiSDK PID 1 into a reproducible boot image."""
import argparse
import gzip
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def static_aarch64_elf(binary):
    """Accept only bounded ELF64 little-endian aarch64 static executables."""
    if len(binary) < 64 or binary[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', binary, 16)[0] not in (2, 3) or struct.unpack_from('<H', binary, 18)[0] != 183:
        raise ValueError('Guest programs must be static aarch64 ELF64 executables')
    offset = struct.unpack_from('<Q', binary, 32)[0]
    size, count = struct.unpack_from('<HH', binary, 54)
    if size < 56 or not count or count > 1024 or offset < 64 or offset + size * count > len(binary):
        raise ValueError('Invalid guest ELF program headers')
    for index in range(count):
        if struct.unpack_from('<I', binary, offset + index * size)[0] == 3:
            raise ValueError('Dynamically linked workloads are unavailable in the sealed runtime root')
    return binary

def download(spec, directory):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / Path(spec['url']).name
    if not path.exists():
        print('Downloading', spec['url'], flush=True)
        temporary = path.with_suffix(path.suffix + '.partial')
        with urllib.request.urlopen(spec['url'], timeout=120) as response, temporary.open('wb') as out:
            shutil.copyfileobj(response, out)
        temporary.replace(path)
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() != spec['sha256']:
        raise RuntimeError('Checksum mismatch: ' + str(path))
    return path

def unpack_cpio(data):
    """Read newc metadata without extracting device nodes or following links."""
    entries = {}
    offset = 0
    while offset + 110 <= len(data):
        if data[offset:offset + 6] not in (b'070701', b'070702'):
            # Some initramfs streams have an uncompressed archive prefix.
            if not data[offset:].strip(b'\0'):
                break
            raise ValueError('Unsupported initramfs cpio header at ' + str(offset))
        numbers = [int(data[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
        mode, length, name_length = numbers[1], numbers[6], numbers[11]
        start = offset + 110
        name = data[start:start + name_length - 1].decode('utf-8')
        body = (start + name_length + 3) & ~3
        value = data[body:body + length]
        if len(value) != length:
            raise ValueError('Truncated cpio body')
        offset = (body + length + 3) & ~3
        if name == 'TRAILER!!!':
            break
        safe = PurePosixPath(name)
        if safe.is_absolute() or '..' in safe.parts:
            raise ValueError('Unsafe cpio path')
        entries[str(safe)] = (mode, value)
    return entries

def cpio_bytes(entries):
    out = bytearray()
    for inode, (name, mode, data) in enumerate(entries + [('TRAILER!!!', stat.S_IFREG, b'')], 1):
        encoded = name.encode() + b'\0'
        device = (5, 1) if name == 'dev/console' else (1, 3) if name == 'dev/null' else (0, 0)
        fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0, *device, len(encoded), 0]
        out.extend(b'070701' + ''.join(f'{value:08x}' for value in fields).encode())
        out.extend(encoded)
        out.extend(b'\0' * (-len(out) % 4))
        out.extend(data)
        out.extend(b'\0' * (-len(out) % 4))
    return bytes(out)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init', type=Path, help='Static Linux ii-init executable')
    parser.add_argument('--workload', type=Path, help='Optional static Linux executable installed as /app/service')
    parser.add_argument('--sandbox', type=Path, help='Static ii-workload launcher (default: sibling of --init)')
    parser.add_argument('--files-agent', type=Path, help='Static ii-files worker (default: sibling of --init)')
    parser.add_argument('--process-launcher', type=Path, help='Static ii-process launcher (default: sibling of --init)')
    parser.add_argument('--program', action='append', default=[], metavar='NAME=ELF', help='Trusted static guest program catalog entry')
    parser.add_argument('--download-only', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / 'build' / 'images' / 'aarch64')
    args = parser.parse_args()
    output = args.output.resolve()
    if ROOT / 'build' not in output.parents:
        raise ValueError('Generated images must remain beneath this project build/')
    output.mkdir(parents=True, exist_ok=True)
    sources = json.loads((ROOT / 'tools' / 'sources.json').read_text())
    archives = ROOT / 'build' / 'images' / 'downloads'
    netboot = download(sources['alpine_netboot_aarch64'], archives)
    root = download(sources['alpine_root_aarch64'], archives)
    cached = output / 'upstream.json'
    metadata = json.loads(cached.read_text()) if cached.exists() else {}
    kernel_path, initrd_path = output / 'Image', output / 'upstream-initramfs'
    hit = metadata.get('source_sha256') == sources['alpine_netboot_aarch64']['sha256'] and kernel_path.exists() and initrd_path.exists()
    if hit:
        kernel, original_initrd = kernel_path.read_bytes(), initrd_path.read_bytes()
        hit = metadata.get('kernel_sha256') == hashlib.sha256(kernel).hexdigest() and metadata.get('initrd_sha256') == hashlib.sha256(original_initrd).hexdigest()
    if not hit:
        selected = {}
        # Stream once through the large netboot archive instead of repeatedly
        # seeking through gzip for each selected member.
        with tarfile.open(netboot, 'r|gz') as tar:
            for member in tar:
                if Path(member.name).name in ('vmlinuz-virt', 'initramfs-virt'):
                    selected[Path(member.name).name] = tar.extractfile(member).read()
        kernel, original_initrd = selected['vmlinuz-virt'], selected['initramfs-virt']
        if kernel[:2] == b'\x1f\x8b':
            kernel = gzip.decompress(kernel)
        if kernel[:2] == b'MZ' and kernel[4:8] == b'zimg':
            offset, size = struct.unpack_from('<II', kernel, 8)
            compression = kernel[24:56].split(b'\0')[0]
            payload = kernel[offset:offset + size]
            if len(payload) != size:
                raise ValueError('Truncated EFI zboot payload')
            if compression == b'gzip':
                kernel = gzip.decompress(payload)
            elif compression == b'zstd':
                kernel = subprocess.run(['zstd', '-d', '-c'], input=payload, check=True, capture_output=True).stdout
            else:
                raise ValueError('Unsupported EFI zboot compression: ' + repr(compression))
        kernel_path.write_bytes(kernel)
        initrd_path.write_bytes(original_initrd)
        cached.write_text(json.dumps({'source_sha256': sources['alpine_netboot_aarch64']['sha256'],
            'kernel_sha256': hashlib.sha256(kernel).hexdigest(), 'initrd_sha256': hashlib.sha256(original_initrd).hexdigest()}, indent=2) + '\n')
    if args.download_only:
        print(json.dumps({'kernel': str(output / 'Image'), 'upstream_initramfs': len(original_initrd)}))
        return
    if not args.init:
        parser.error('--init is required unless --download-only is used')
    binary = static_aarch64_elf(args.init.read_bytes())
    launcher = args.sandbox or args.init.with_name('ii-workload')
    sandbox = static_aarch64_elf(launcher.read_bytes()) if launcher.is_file() else None
    files_path = args.files_agent or args.init.with_name('ii-files')
    files_agent = static_aarch64_elf(files_path.read_bytes()) if files_path.is_file() else None
    if files_agent is None:
        raise ValueError('The confined ii-files worker is required for the iiOS Files API')
    process_path = args.process_launcher or args.init.with_name('ii-process')
    process_launcher = static_aarch64_elf(process_path.read_bytes()) if process_path.is_file() else None
    if process_launcher is None:
        raise ValueError('The confined ii-process launcher is required for the process API')
    if args.workload and sandbox is None:
        raise ValueError('A sandbox launcher is required whenever a workload is supplied')
    upstream = unpack_cpio(gzip.decompress(original_initrd))
    entries = {}
    # Include the upstream early-boot drivers and dependency metadata, but own
    # PID 1 and userspace initialization rather than invoking Alpine init.
    for name, value in upstream.items():
        if name.startswith('lib/modules/') and not name.endswith('.bin') and not stat.S_ISDIR(value[0]):
            entries[name] = value
    for package, spec in sources['guest_packages'].items():
        archive = download(spec, archives)
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                name = str(PurePosixPath(member.name))
                if package == 'linux-virt':
                    if name.endswith(('/ext4.ko.gz', '/jbd2.ko.gz', '/mbcache.ko.gz', '/crc16.ko.gz')):
                        entries[name[:-3]] = (stat.S_IFREG | 0o644, gzip.decompress(tar.extractfile(member).read()))
                    elif name.endswith('/modules.dep'):
                        dep = tar.extractfile(member).read().decode()
                        added = '\n'.join(line.replace('.ko.gz', '.ko') for line in dep.splitlines()
                                          if any(x in line.split(':')[0] for x in ['ext4.', 'jbd2.', 'mbcache.', 'crc16.'])) + '\n'
                        original = entries.get(name, (stat.S_IFREG | 0o644, b''))[1]
                        entries[name] = (stat.S_IFREG | 0o644, original + added.encode())
                elif name.startswith(('usr/lib/', 'lib/')) or name in ('sbin/mke2fs', 'etc/mke2fs.conf'):
                    if member.isfile():
                        entries[name] = (stat.S_IFREG | (member.mode & 0o777), tar.extractfile(member).read())
                    elif member.issym():
                        entries[name] = (stat.S_IFLNK | 0o777, member.linkname.encode())
    with tarfile.open(root) as tar:
        for name in ('bin/busybox', 'lib/ld-musl-aarch64.so.1', 'usr/share/udhcpc/default.script'):
            try:
                member = tar.getmember('./' + name)
            except KeyError:
                try:
                    member = tar.getmember(name)
                except KeyError:
                    continue
            content = tar.extractfile(member).read()
            entries[name] = (stat.S_IFREG | (member.mode & 0o777), content)
    entries['init'] = (stat.S_IFREG | 0o755, binary)
    entries['etc/os-release'] = (stat.S_IFREG | 0o644, b'NAME="iiOS"\nID=iios\nPRETTY_NAME="iiOS (Linux + iiSDK)"\nVERSION_ID=0.6.0\n')
    entries['etc/resolv.conf'] = (stat.S_IFREG | 0o644, b'nameserver 10.0.2.3\n')
    # A small deterministic DHCP hook; BusyBox supplies the interpreter.
    hook = b'#!/bin/sh\ncase "$1" in\n bound|renew)\n /bin/busybox ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}" up\n /bin/busybox ip route replace default via "${router%% *}" dev "$interface"\n ;;\nesac\n'
    entries['etc/dhcp.script'] = (stat.S_IFREG | 0o755, hook)
    entries['bin/sh'] = (stat.S_IFLNK | 0o777, b'busybox')
    entries['sbin/modprobe'] = (stat.S_IFLNK | 0o777, b'/bin/busybox')
    entries['dev/console'] = (stat.S_IFCHR | 0o600, b'')
    entries['dev/null'] = (stat.S_IFCHR | 0o666, b'')
    if sandbox:
        entries['sbin/ii-workload'] = (stat.S_IFREG | 0o755, sandbox)
    entries['sbin/ii-files'] = (stat.S_IFREG | 0o755, files_agent)
    entries['sbin/ii-process'] = (stat.S_IFREG | 0o755, process_launcher)
    programs = {}
    for value in args.program:
        name, separator, path = value.partition('=')
        if not separator or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', name) or name == 'service' or name in programs:
            raise ValueError('Invalid or duplicate guest program catalog entry')
        if len(programs) >= 16:
            raise ValueError('Guest program catalog exceeds its limit')
        program = static_aarch64_elf(Path(path).read_bytes())
        entries['app/programs/' + name] = (stat.S_IFREG | 0o755, program)
        programs[name] = hashlib.sha256(program).hexdigest()
    workload = static_aarch64_elf(args.workload.read_bytes()) if args.workload else None
    if workload:
        entries['app/service'] = (stat.S_IFREG | 0o755, workload)
    directories = {'proc', 'sys', 'dev', 'run', 'tmp', 'data', 'etc', 'app', 'bin', 'lib'}
    for name in list(entries):
        for parent in PurePosixPath(name).parents:
            if str(parent) != '.':
                directories.add(str(parent))
    ordered = [(name, stat.S_IFDIR | 0o755, b'') for name in sorted(directories, key=lambda s: (s.count('/'), s))]
    ordered += [(name, *entries[name]) for name in sorted(entries)]
    image = gzip.compress(cpio_bytes(ordered), mtime=0)
    (output / 'initramfs.cpio.gz').write_bytes(image)
    manifest = {'schema': 1, 'version': '0.6.0', 'architecture': 'aarch64', 'os': 'iiOS', 'init': 'iiSDKCore PID 1',
                'filesystem_profile': 'society-v1', 'container_root': '/data/Society.societycontainer',
                'processes_api': 1, 'process_launcher_sha256': hashlib.sha256(process_launcher).hexdigest(), 'programs': programs,
                'files_api': 1, 'files_agent_sha256': hashlib.sha256(files_agent).hexdigest(),
                'sandbox_sha256': hashlib.sha256(sandbox).hexdigest() if sandbox else None,
                'workload_sha256': hashlib.sha256(workload).hexdigest() if workload else None,
                'kernel_sha256': hashlib.sha256(kernel).hexdigest(), 'init_sha256': hashlib.sha256(binary).hexdigest(),
                'initramfs_sha256': hashlib.sha256(image).hexdigest(), 'entries': len(ordered),
                'sources': {k:v for k,v in sources.items() if k.startswith('alpine_') or k == 'guest_packages'}}
    (output / 'image.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    main()
