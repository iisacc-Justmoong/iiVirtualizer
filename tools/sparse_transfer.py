#!/usr/bin/env python3
"""Stream a sparse, task-owned disk over stdin without a host bind mount."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import time
import zlib

MAGIC = b'IIVSPRS1'
META = struct.Struct('>QQ')
RECORD = struct.Struct('>QII')
END = (1 << 64) - 1
CHUNK = 1024 * 1024
ROOT = Path(__file__).resolve().parents[1]


def declared_allocation(size, blocks):
    """Bound source filesystem allocation to the logical data a receiver can write."""
    if size <= 0 or blocks < 0:
        raise ValueError('Invalid source size or allocation')
    # Some filesystems include metadata and allocation rounding in st_blocks.
    # The destination also keeps a separate two-GiB free-space reserve.
    return min(size, blocks * 512)


def create_destination(path):
    """Create a new output without ever opening an existing file or symlink."""
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0)
    return os.open(path, flags, 0o600)


def prepare_source_io(fd):
    """Request uncached macOS reads without changing the source file."""
    if sys.platform != 'darwin':
        return False
    import fcntl
    if not hasattr(fcntl, 'F_NOCACHE'):
        return False
    try:
        fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
        return True
    except OSError:
        return False


def release_written_cache(fd):
    """Request release of clean Linux pages after durable output."""
    if not hasattr(os, 'posix_fadvise') or not hasattr(os, 'POSIX_FADV_DONTNEED'):
        return False
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        return True
    except OSError:
        return False


def exact(stream, count):
    data = bytearray()
    while len(data) < count:
        block = stream.read(count - len(data))
        if not block:
            raise EOFError('Incomplete sparse transfer')
        data.extend(block)
    return bytes(data)


def send(path, rate, offset=0, length=None):
    path = path.resolve(strict=True)
    if not any(path.is_relative_to((ROOT / parent).resolve(strict=True)) for parent in ('build', 'resources')):
        raise ValueError('Source must be beneath project build/ or resources/')
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    try:
        start = os.fstat(fd)
        if not stat.S_ISREG(start.st_mode):
            raise ValueError('Source must be a regular file')
        source_cache_bypassed = prepare_source_io(fd)
        if offset < 0 or offset >= start.st_size:
            raise ValueError('Source offset is outside the file')
        segment_size = start.st_size - offset if length is None else length
        if segment_size <= 0 or offset + segment_size > start.st_size:
            raise ValueError('Source range is outside the file')
        end_offset = offset + segment_size
        output = sys.stdout.buffer
        output.write(MAGIC + META.pack(segment_size, declared_allocation(segment_size, start.st_blocks)))
        digest = hashlib.sha256()
        cursor, read_bytes, sent_bytes = offset, 0, 0
        while cursor < end_offset:
            try:
                data_at = os.lseek(fd, cursor, os.SEEK_DATA)
            except OSError as error:
                if error.errno == errno.ENXIO:  # no remaining data
                    break
                raise
            if data_at >= end_offset:
                break
            end = min(os.lseek(fd, data_at, os.SEEK_HOLE), end_offset)
            if end <= data_at:
                raise RuntimeError('Invalid sparse extent')
            position = data_at
            while position < end:
                data = os.pread(fd, min(CHUNK, end - position), position)
                if not data:
                    raise RuntimeError('Source shortened during transfer')
                read_bytes += len(data)
                if data != bytes(len(data)):
                    packed = zlib.compress(data, 1)
                    header = RECORD.pack(position - offset, len(data), len(packed))
                    output.write(header)
                    output.write(packed)
                    digest.update(struct.pack('>QI', position - offset, len(data)))
                    digest.update(data)
                    sent_bytes += len(data)
                position += len(data)
                if rate:
                    # Per-chunk pacing cannot accumulate credit during a long
                    # disk stall and then burst when I/O resumes.
                    time.sleep(len(data) / rate)
                if read_bytes // (64 * CHUNK) != (read_bytes - len(data)) // (64 * CHUNK):
                    print(json.dumps({'source_read': read_bytes, 'data_sent': sent_bytes, 'logical_offset': position - offset, 'logical_size': segment_size}), file=sys.stderr, flush=True)
            cursor = end
        final = os.fstat(fd)
        if (final.st_dev, final.st_ino, final.st_size, final.st_mtime_ns) != (start.st_dev, start.st_ino, start.st_size, start.st_mtime_ns):
            raise RuntimeError('Source disk changed during transfer')
        output.write(RECORD.pack(END, 0, 0) + digest.digest())
        output.flush()
        print(json.dumps({'complete': True, 'source_read': read_bytes, 'data_sent': sent_bytes,
                          'sha256': digest.hexdigest(), 'source_offset': offset,
                          'source_length': segment_size, 'source_cache_bypassed': source_cache_bypassed}),
              file=sys.stderr, flush=True)
    finally:
        os.close(fd)


def receive(path):
    if os.environ.get('IIV_TEST_CONTAINER') != '1' or os.uname().sysname != 'Linux' or Path('/dev/kvm').exists():
        raise RuntimeError('Isolated test container required')
    if sorted(os.listdir('/sys/class/net')) != ['lo']:
        raise RuntimeError('Container networking must be disabled')
    path = Path(path)
    parent = path.parent.resolve(strict=True)
    allowed = (ROOT / 'resources/platform-guests').resolve(strict=True)
    if not parent.is_relative_to(allowed) or path.is_symlink():
        raise ValueError('Destination must be an existing owned guest resource')
    source = sys.stdin.buffer
    if exact(source, len(MAGIC)) != MAGIC:
        raise ValueError('Invalid sparse transfer header')
    size, allocated = META.unpack(exact(source, META.size))
    if size == 0 or size > 96 * 1024**3 or allocated > size:
        raise ValueError('Invalid sparse source size')
    info = os.statvfs(parent)
    if info.f_bavail * info.f_frsize < allocated + 2 * 1024**3:
        raise RuntimeError('Insufficient private host disk space')
    fd = create_destination(parent / path.name)
    try:
        os.ftruncate(fd, size)
        digest = hashlib.sha256()
        last = written = 0
        while True:
            offset, raw_len, packed_len = RECORD.unpack(exact(source, RECORD.size))
            if offset == END:
                if raw_len or packed_len or exact(source, 32) != digest.digest():
                    raise ValueError('Sparse transfer checksum mismatch')
                break
            if offset < last or raw_len < 1 or raw_len > CHUNK or offset + raw_len > size or packed_len < 1 or packed_len > CHUNK + 1024:
                raise ValueError('Invalid sparse transfer extent')
            unpacker = zlib.decompressobj()
            data = unpacker.decompress(exact(source, packed_len), raw_len + 1)
            if len(data) != raw_len or not unpacker.eof or unpacker.unused_data:
                raise ValueError('Invalid sparse compressed block')
            done = 0
            while done < len(data):
                done += os.pwrite(fd, data[done:], offset + done)
            digest.update(struct.pack('>QI', offset, raw_len))
            digest.update(data)
            last = offset + raw_len
            written += raw_len
        os.fsync(fd)
        cache_release_requested = release_written_cache(fd)
        directory_fd = os.open(parent, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        print(json.dumps({'complete': True, 'logical_size': size, 'written': written, 'sha256': digest.hexdigest(),
                          'write_cache_release_requested': cache_release_requested}), flush=True)
    finally:
        os.close(fd)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['send', 'receive'])
    parser.add_argument('path', type=Path)
    parser.add_argument('--rate-mib', type=int, default=2)
    parser.add_argument('--offset', type=int, default=0)
    parser.add_argument('--length', type=int)
    args = parser.parse_args()
    if not 1 <= args.rate_mib <= 16:
        parser.error('Rate must be between 1 and 16 MiB/s')
    if args.mode == 'send':
        send(args.path, args.rate_mib * CHUNK, args.offset, args.length)
    else:
        if args.offset or args.length is not None:
            parser.error('Offsets and lengths apply to send only')
        receive(args.path)
