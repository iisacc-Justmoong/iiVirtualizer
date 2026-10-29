#!/usr/bin/env python3
"""Transfer and reassemble resumable files in a private VM-test container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys
import uuid

from sparse_transfer import create_destination, release_written_cache
from resumable_digest import verify_fd

BLOCK = 1024 * 1024
ASSEMBLY_FLUSH_BYTES = 256 * BLOCK
ROOT = Path(__file__).resolve().parents[1]


def plan_ranges(size, chunk_size):
    if not isinstance(size, int) or not isinstance(chunk_size, int) or size <= 0 or chunk_size <= 0:
        raise ValueError('Invalid source or chunk size')
    if (size + chunk_size - 1) // chunk_size > 10000:
        raise ValueError('Too many transfer chunks')
    return [(offset, min(chunk_size, size - offset)) for offset in range(0, size, chunk_size)]


def matches_receipts(sender, receiver, offset, length):
    digest = sender.get('sha256')
    return (sender.get('complete') is True and receiver.get('complete') is True and
            isinstance(digest, str) and re.fullmatch(r'[0-9a-f]{64}', digest) is not None and
            digest == receiver.get('sha256') and sender.get('source_offset') == offset and
            sender.get('source_length') == length and receiver.get('logical_size') == length and
            isinstance(sender.get('data_sent'), int) and 0 <= sender['data_sent'] <= length and
            sender['data_sent'] == receiver.get('written'))


def source_identity(path):
    path = Path(path).resolve(strict=True)
    if not any(path.is_relative_to((ROOT / name).resolve(strict=True)) for name in ('build', 'resources')):
        raise ValueError('Source must be in project build/ or resources/')
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
        raise ValueError('Source must be a nonempty regular file')
    return path, {'device': info.st_dev, 'inode': info.st_ino, 'size': info.st_size, 'mtime_ns': info.st_mtime_ns}


def make_checkpoint(source, chunk_size, container, token):
    source, identity = source_identity(source)
    if not re.fullmatch(r'iiv-test-[0-9a-f]{12}', container) or not re.fullmatch(r'[0-9a-f]{32}', token):
        raise ValueError('Invalid test container or transfer identity')
    plan_ranges(identity['size'], chunk_size)
    return {'schema': 1, 'source': str(source), 'source_identity': identity, 'chunk_size': chunk_size,
            'container': container, 'token': token,
            'remote_dir': '/work/iiVirtualizer/resources/platform-guests/transfer-' + token, 'parts': []}


def validate_checkpoint(checkpoint, source, container):
    current, identity = source_identity(source)
    token = checkpoint.get('token', '')
    if (checkpoint.get('schema') != 1 or not isinstance(token, str) or not re.fullmatch(r'[0-9a-f]{32}', token) or
            checkpoint.get('source') != str(current) or checkpoint.get('source_identity') != identity or
            checkpoint.get('container') != container or not re.fullmatch(r'iiv-test-[0-9a-f]{12}', container) or
            checkpoint.get('remote_dir') != '/work/iiVirtualizer/resources/platform-guests/transfer-' + token):
        raise ValueError('Transfer checkpoint identity does not match')
    ranges = plan_ranges(identity['size'], checkpoint.get('chunk_size'))
    parts = checkpoint.get('parts')
    if not isinstance(parts, list) or len(parts) > len(ranges):
        raise ValueError('Invalid completed chunk list')
    names = set()
    for index, (part, expected) in enumerate(zip(parts, ranges)):
        if (not isinstance(part, dict) or not isinstance(part.get('name'), str) or
                not re.fullmatch(r'part-%04d-[0-9a-f]{32}\.bin' % index, part['name']) or
                part['name'] in names or (part.get('offset'), part.get('length')) != expected or
                not isinstance(part.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', part['sha256'])):
            raise ValueError('Invalid completed chunk')
        names.add(part['name'])
    return True


def record_part(checkpoint, name, sender, receiver):
    validate_checkpoint(checkpoint, checkpoint['source'], checkpoint['container'])
    ranges = plan_ranges(checkpoint['source_identity']['size'], checkpoint['chunk_size'])
    index = len(checkpoint['parts'])
    if (index >= len(ranges) or not isinstance(name, str) or
            not re.fullmatch(r'part-%04d-[0-9a-f]{32}\.bin' % index, name)):
        raise ValueError('Invalid destination chunk name')
    offset, length = ranges[index]
    if not matches_receipts(sender, receiver, offset, length):
        raise ValueError('Transfer chunk receipts do not match')
    checkpoint['parts'].append({'name': name, 'offset': offset, 'length': length, 'sha256': sender['sha256']})


def fsync_directory(path):
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_checkpoint(path, checkpoint):
    path = Path(path)
    parent = path.parent.resolve(strict=True)
    allowed = (ROOT / 'resources/platform-guests').resolve(strict=True)
    if not parent.is_relative_to(allowed) or path.is_symlink():
        raise ValueError('Checkpoint must be in private guest resources')
    path = parent / path.name
    validate_checkpoint(checkpoint, checkpoint['source'], checkpoint['container'])
    existing = None
    existing_info = None
    if path.exists():
        fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        try:
            existing_info = os.fstat(fd)
            if not stat.S_ISREG(existing_info.st_mode) or existing_info.st_size > 1024 * 1024:
                raise ValueError('Invalid existing checkpoint')
            existing = json.loads(os.read(fd, existing_info.st_size))
        except (ValueError, UnicodeDecodeError) as error:
            raise ValueError('Existing file is not this transfer checkpoint') from error
        finally:
            os.close(fd)
        fields = ('schema', 'source', 'source_identity', 'chunk_size', 'container', 'token', 'remote_dir')
        if (not isinstance(existing, dict) or any(existing.get(key) != checkpoint.get(key) for key in fields) or
                not isinstance(existing.get('parts'), list) or
                existing['parts'] != checkpoint['parts'][:len(existing['parts'])]):
            raise ValueError('Existing checkpoint belongs to different work or would be rolled back')
    temporary = parent / ('.checkpoint-' + uuid.uuid4().hex + '.tmp')
    fd = create_destination(temporary)
    try:
        data = (json.dumps(checkpoint, indent=2) + '\n').encode()
        written = 0
        while written < len(data):
            written += os.write(fd, data[written:])
        os.fsync(fd)
    finally:
        os.close(fd)
    try:
        if existing_info is None:
            os.link(temporary, path)
            temporary.unlink()
        else:
            current = path.lstat()
            if (current.st_dev, current.st_ino) != (existing_info.st_dev, existing_info.st_ino):
                raise ValueError('Checkpoint identity changed before updating')
            os.replace(temporary, path)
        fsync_directory(parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def assemble_parts(directory, parts, target, expected_sha256, total_size, token, resume=False,
                   verification_checkpoint=None):
    directory = Path(directory).resolve(strict=True)
    target = Path(target)
    if not re.fullmatch(r'[0-9a-f]{32}', token) or not re.fullmatch(r'[0-9a-f]{64}', expected_sha256):
        raise ValueError('Invalid transfer identity or complete-image hash')
    if verification_checkpoint is not None and not resume:
        raise ValueError('Checkpointed verification requires an owned resumable assembly')
    if target.parent.resolve(strict=True) != directory.parent or not directory.is_dir():
        raise ValueError('Output must be a new sibling of the private parts directory')
    marker = directory / '.owner'
    marker_stat = marker.lstat()
    if not stat.S_ISREG(marker_stat.st_mode) or marker.read_text().strip() != token:
        raise ValueError('Transfer directory ownership does not match')
    if not isinstance(parts, list) or not parts:
        raise ValueError('No completed transfer chunks')
    expected_offset = 0
    names = set()
    for index, part in enumerate(parts):
        name, offset, length = part.get('name'), part.get('offset'), part.get('length')
        if (not isinstance(name, str) or not re.fullmatch(r'part-%04d-[0-9a-f]{32}\.bin' % index, name) or
                name in names or offset != expected_offset or not isinstance(length, int) or length <= 0):
            raise ValueError('Invalid transfer chunk sequence')
        info = (directory / name).lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_size != length:
            raise ValueError('Transfer chunk is missing or has the wrong size')
        names.add(name)
        expected_offset += length
    if expected_offset != total_size:
        raise ValueError('Transfer chunks do not cover the expected source size')
    sidecar = target.with_name(target.name + '.iiv-assembly.json')
    fingerprint = hashlib.sha256(json.dumps(parts, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    identity = {'schema': 1, 'token': token, 'target': target.name, 'sha256': expected_sha256,
                'size': total_size, 'parts_sha256': fingerprint}
    if target.is_symlink() or (resume and sidecar.is_symlink()):
        raise ValueError('Assembly output or ownership marker is a symlink')
    existing = resume and (target.exists() or sidecar.exists())
    if existing and (not target.exists() or not sidecar.exists()):
        raise ValueError('Existing assembly output lacks its ownership marker')
    if not existing:
        space = os.statvfs(target.parent)
        if space.f_bavail * space.f_frsize < total_size + 2 * 1024**3:
            raise RuntimeError('Insufficient space to assemble the private image')
        output = create_destination(target)
    else:
        output = os.open(target, os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0))
    digest = hashlib.sha256()
    pending = 0
    try:
        import fcntl
        fcntl.flock(output, fcntl.LOCK_EX | fcntl.LOCK_NB)
        info = os.fstat(output)
        if not stat.S_ISREG(info.st_mode) or info.st_size > total_size:
            raise ValueError('Invalid assembly output')
        if resume:
            owned = dict(identity, device=info.st_dev, inode=info.st_ino)
            if existing:
                if read_checkpoint(sidecar) != owned:
                    raise ValueError('Existing assembly output belongs to different work')
            else:
                fd = create_destination(sidecar)
                try:
                    data = (json.dumps(owned, sort_keys=True) + '\n').encode()
                    done = 0
                    while done < len(data):
                        written = os.write(fd, data[done:])
                        if written <= 0:
                            raise OSError('Could not write the assembly ownership marker')
                        done += written
                    os.fsync(fd)
                finally:
                    os.close(fd)
                fsync_directory(target.parent)
        existing_size = info.st_size
        # A complete owned output is independently verified against the full
        # expected hash below. Re-reading every part first would duplicate I/O
        # after an interruption without strengthening that verification.
        complete_output = existing and existing_size == total_size
        space = os.statvfs(target.parent)
        if space.f_bavail * space.f_frsize < total_size - existing_size + 2 * 1024**3:
            raise RuntimeError('Insufficient space to finish the private image')
        if existing_size:
            os.fsync(output)
        for part in (() if complete_output else parts):
            source = os.open(directory / part['name'], os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
            try:
                before = os.fstat(source)
                if not stat.S_ISREG(before.st_mode) or before.st_size != part['length']:
                    raise ValueError('Transfer chunk changed before reading')
                copied = 0
                while copied < part['length']:
                    block = os.read(source, min(BLOCK, part['length'] - copied))
                    if not block:
                        raise ValueError('Transfer chunk was truncated')
                    position = part['offset'] + copied
                    prefix = min(len(block), max(0, existing_size - position))
                    if prefix and os.pread(output, prefix, position) != block[:prefix]:
                        raise ValueError('Existing assembly prefix does not match the transfer chunks')
                    digest.update(block)
                    done = prefix
                    while done < len(block):
                        written = os.pwrite(output, block[done:], position + done)
                        if written <= 0:
                            raise OSError('Could not write the assembly output')
                        done += written
                    pending += len(block) - prefix
                    copied += len(block)
                after = os.fstat(source)
                if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
                    raise ValueError('Transfer chunk changed while reading')
                release_written_cache(source)
            finally:
                os.close(source)
            if part['offset'] < existing_size:
                release_written_cache(output)
            if pending >= ASSEMBLY_FLUSH_BYTES:
                os.fsync(output)
                release_written_cache(output)
                pending = 0
        if not complete_output and digest.hexdigest() != expected_sha256:
            raise ValueError('Complete-image hash mismatch')
        os.fsync(output)
        verified_info = os.fstat(output)
        if verified_info.st_size != total_size:
            raise ValueError('Assembled output has the wrong size')
        release_written_cache(output)
        if verification_checkpoint is not None:
            actual = verify_fd(output, verification_checkpoint, expected_sha256)
        else:
            checksum = hashlib.sha256()
            position = 0
            while position < total_size:
                block = os.pread(output, min(BLOCK, total_size - position), position)
                if not block:
                    raise ValueError('Assembled output was truncated during verification')
                checksum.update(block)
                position += len(block)
                if position % ASSEMBLY_FLUSH_BYTES == 0:
                    release_written_cache(output)
            actual = checksum.hexdigest()
        after = os.fstat(output)
        if ((verified_info.st_dev, verified_info.st_ino, verified_info.st_size, verified_info.st_mtime_ns) !=
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)):
            raise ValueError('Assembled output changed during verification')
        if actual != expected_sha256:
            raise ValueError('Assembled output checksum mismatch')
        release_written_cache(output)
        fsync_directory(target.parent)
        return actual
    finally:
        os.close(output)


def read_checkpoint(path):
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > 1024 * 1024:
            raise ValueError('Invalid transfer checkpoint file')
        return json.loads(os.read(fd, info.st_size))
    finally:
        os.close(fd)


def ssh_command(container, argv, stdin=False):
    config = (ROOT / 'resources/ct/iivc/ssh.config').resolve(strict=True)
    command = ['sudo', '/usr/local/bin/nerdctl', 'exec']
    if stdin:
        command.append('-i')
    command += [container, *map(str, argv)]
    return ['/usr/bin/ssh', '-o', 'ControlMaster=no', '-o', 'ControlPath=none',
            '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', '-F', str(config),
            'lima-iivc', shlex.join(command)]


def ensure_remote_directory(checkpoint):
    script = '''import json,os,stat,sys
from pathlib import Path
sys.path.insert(0, "/work/iiVirtualizer/tools")
from test_isolation import require_test_container
require_test_container("tcg")
assert sorted(os.listdir("/sys/class/net")) == ["lo"]
base=Path("/work/iiVirtualizer/resources/platform-guests").resolve(strict=True)
path=Path(sys.argv[1]); token=sys.argv[2]
assert path.parent.resolve(strict=True)==base and path.name=="transfer-"+token
if path.exists():
    assert not path.is_symlink() and path.is_dir()
    marker=path/".owner"
    assert not marker.is_symlink() and marker.read_text().strip()==token
else:
    path.mkdir(mode=0o700)
    with (path/".owner").open("x") as f:
        f.write(token+"\\n"); f.flush(); os.fsync(f.fileno())
    fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)
    fd=os.open(base,os.O_RDONLY|os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)
for part in json.loads(sys.argv[3]):
    item=path/part["name"]; info=item.lstat()
    assert stat.S_ISREG(info.st_mode) and info.st_size==part["length"]
'''
    result = subprocess.run(ssh_command(checkpoint['container'],
                           ['python3', '-B', '-c', script, checkpoint['remote_dir'], checkpoint['token'],
                            json.dumps(checkpoint['parts'])]),
                            capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError('Cannot confirm the private transfer directory: ' + result.stderr[-1000:])


def transfer_one(checkpoint, name, offset, length, rate_mib):
    sender = subprocess.Popen(['/usr/bin/nice', '-n', '5', sys.executable, '-B',
                               str(ROOT / 'tools/sparse_transfer.py'), 'send',
                               checkpoint['source'], '--offset', str(offset), '--length', str(length),
                               '--rate-mib', str(rate_mib)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    receiver = None
    try:
        destination = checkpoint['remote_dir'] + '/' + name
        receiver = subprocess.Popen(ssh_command(checkpoint['container'],
                                    ['python3', '-B', '/work/iiVirtualizer/tools/sparse_transfer.py',
                                     'receive', destination], stdin=True),
                                    stdin=sender.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sender.stdout.close()
        print(json.dumps({'event': 'chunk-active', 'offset': offset, 'length': length}), flush=True)
        output, error = receiver.communicate(timeout=900)
        sender_code = sender.wait(timeout=30)
        sender_error = sender.stderr.read()
        if sender_code or receiver.returncode:
            raise RuntimeError('Transfer chunk failed: ' + error.decode(errors='replace')[-1000:] +
                               sender_error.decode(errors='replace')[-1000:])
        sent = json.loads(sender_error.decode().splitlines()[-1])
        received = json.loads(output)
        if not matches_receipts(sent, received, offset, length):
            raise ValueError('Transfer chunk receipts do not match')
        return sent, received
    finally:
        for child in (sender, receiver):
            if child is not None and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)


def transfer(source, container, checkpoint_path, chunk_size, rate_mib, max_chunks=None):
    # A kernel-backed lock is released even if this process exits unexpectedly.
    import fcntl
    from test_isolation import install_interrupt_handlers, protect_cleanup_from_interrupts
    path = Path(checkpoint_path)
    parent = path.parent.resolve(strict=True)
    allowed = (ROOT / 'resources/platform-guests').resolve(strict=True)
    if (not parent.is_relative_to(allowed) or path.is_symlink() or not 1 <= rate_mib <= 4 or
            (max_chunks is not None and not 1 <= max_chunks <= 10000)):
        raise ValueError('Invalid private checkpoint or transfer rate')
    path = parent / path.name
    lock_path = parent / (path.name + '.lock')
    lock = os.open(lock_path, os.O_RDWR | os.O_CREAT | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    try:
        if not stat.S_ISREG(os.fstat(lock).st_mode):
            raise ValueError('Invalid checkpoint lock')
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if path.exists():
            checkpoint = read_checkpoint(path)
            validate_checkpoint(checkpoint, source, container)
            if checkpoint['chunk_size'] != chunk_size:
                raise ValueError('Chunk size differs from the saved checkpoint')
        else:
            checkpoint = make_checkpoint(source, chunk_size, container, uuid.uuid4().hex)
            write_checkpoint(path, checkpoint)
        ensure_remote_directory(checkpoint)
        install_interrupt_handlers()
        try:
            ranges = plan_ranges(checkpoint['source_identity']['size'], chunk_size)
            initial_count = len(checkpoint['parts'])
            while (len(checkpoint['parts']) < len(ranges) and
                   (max_chunks is None or len(checkpoint['parts']) - initial_count < max_chunks)):
                validate_checkpoint(checkpoint, source, container)
                index = len(checkpoint['parts'])
                offset, length = ranges[index]
                name = 'part-%04d-%s.bin' % (index, uuid.uuid4().hex)
                sent, received = transfer_one(checkpoint, name, offset, length, rate_mib)
                record_part(checkpoint, name, sent, received)
                write_checkpoint(path, checkpoint)
                print(json.dumps({'event': 'chunk-complete', 'index': index, 'chunks': len(ranges),
                                  'bytes_complete': offset + length, 'total_bytes': checkpoint['source_identity']['size']}),
                      flush=True)
        finally:
            protect_cleanup_from_interrupts()
        event = 'transfer-complete' if len(checkpoint['parts']) == len(ranges) else 'transfer-paused'
        print(json.dumps({'event': event, 'checkpoint': str(path),
                          'remote_dir': checkpoint['remote_dir'], 'chunks': len(checkpoint['parts'])}), flush=True)
    finally:
        os.close(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    send = sub.add_parser('transfer')
    send.add_argument('--source', required=True, type=Path)
    send.add_argument('--container', required=True)
    send.add_argument('--checkpoint', required=True, type=Path)
    send.add_argument('--chunk-mib', type=int, default=64)
    send.add_argument('--rate-mib', type=int, default=2)
    send.add_argument('--max-chunks', type=int, help='Stop after this many new chunks, retaining the checkpoint')
    assemble = sub.add_parser('assemble')
    assemble.add_argument('--target', required=True, type=Path)
    assemble.add_argument('--sha256', required=True)
    assemble.add_argument('--resume', action='store_true', help='Verify and resume this owned partial output')
    assemble.add_argument('--verification-checkpoint', type=Path,
                          help='Preserve native SHA-256 progress in private resources')
    args = parser.parse_args()
    if args.action == 'transfer':
        if sys.platform != 'darwin' or not 1 <= args.chunk_mib <= 256:
            raise ValueError('Transfer requires macOS and a chunk size of 1 through 256 MiB')
        transfer(args.source, args.container, args.checkpoint, args.chunk_mib * BLOCK, args.rate_mib, args.max_chunks)
    else:
        from test_isolation import require_test_container
        require_test_container('tcg')
        if sorted(os.listdir('/sys/class/net')) != ['lo']:
            raise RuntimeError('The VM test container must have no network')
        raw = sys.stdin.buffer.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise ValueError('Transfer manifest exceeds its size limit')
        checkpoint = json.loads(raw)
        token = checkpoint.get('token', '')
        if not isinstance(token, str) or not re.fullmatch(r'[0-9a-f]{32}', token):
            raise ValueError('Invalid transfer identity')
        directory = (ROOT / 'resources/platform-guests' / ('transfer-' + token)).resolve(strict=True)
        if checkpoint.get('remote_dir') != str(directory):
            raise ValueError('Transfer directory does not match its manifest')
        actual = assemble_parts(directory, checkpoint.get('parts'), args.target, args.sha256,
                                checkpoint.get('source_identity', {}).get('size'), token, resume=args.resume,
                                verification_checkpoint=args.verification_checkpoint)
        print(json.dumps({'complete': True, 'path': str(args.target), 'sha256': actual,
                          'size': checkpoint['source_identity']['size']}), flush=True)


if __name__ == '__main__':
    main()
