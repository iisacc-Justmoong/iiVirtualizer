#!/usr/bin/env python3
"""Check allocation reporting and fail-closed sparse transfer behavior."""
from pathlib import Path
import os
import struct
import hashlib
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from sparse_transfer import create_destination, declared_allocation, prepare_source_io, release_written_cache

assert declared_allocation(13, 8) == 13
assert declared_allocation(8192, 8) == 4096
assert declared_allocation(8192, 0) == 0
for size, blocks in ((0, 0), (-1, 2), (5, -1)):
    try:
        declared_allocation(size, blocks)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid source allocation accepted')

if os.name == 'posix' and hasattr(os, 'SEEK_DATA'):
    with tempfile.TemporaryDirectory(dir=ROOT / 'resources') as temporary:
        source = Path(temporary) / 'source.bin'
        source.write_bytes(b'isolated-transfer-check')
        try:
            create_destination(source)
        except FileExistsError:
            pass
        else:
            raise AssertionError('An existing file was opened as a transfer destination')
        assert source.read_bytes() == b'isolated-transfer-check'
        fresh = Path(temporary) / 'fresh.bin'
        fd = create_destination(fresh)
        os.write(fd, b'cache-hint-check')
        os.fsync(fd)
        assert isinstance(release_written_cache(fd), bool)
        os.close(fd)
        assert fresh.read_bytes() == b'cache-hint-check'
        fd = os.open(source, os.O_RDONLY)
        try:
            assert isinstance(prepare_source_io(fd), bool)
            assert os.pread(fd, 23, 0) == b'isolated-transfer-check'
        finally:
            os.close(fd)
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sparse_transfer.py'),
                                 'send', str(source), '--rate-mib', '1'], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr
        assert result.stdout[:8] == b'IIVSPRS1'
        logical, allocated = struct.unpack('>QQ', result.stdout[8:24])
        assert logical == source.stat().st_size and 0 <= allocated <= logical
        ranged = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sparse_transfer.py'),
                                  'send', str(source), '--offset', '9', '--length', '8', '--rate-mib', '1'],
                                 capture_output=True, timeout=30)
        assert ranged.returncode == 0, ranged.stderr
        length, _ = struct.unpack('>QQ', ranged.stdout[8:24])
        offset, raw_size, packed_size = struct.unpack('>QII', ranged.stdout[24:40])
        assert length == 8 and offset == 0 and raw_size == 8
        assert zlib.decompress(ranged.stdout[40:40 + packed_size]) == b'transfer'
        invalid_range = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sparse_transfer.py'),
                                        'send', str(source), '--offset', '999', '--length', '1'],
                                       capture_output=True, timeout=30)
        assert invalid_range.returncode != 0
        sparse = Path(temporary) / 'sparse.bin'
        with sparse.open('xb') as f:
            f.write(b'head')
            f.seek(128 * 1024)
            f.write(b'tail')
        begin, segment_length = 65530, 65546
        piece = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sparse_transfer.py'),
                                'send', str(sparse), '--offset', str(begin), '--length', str(segment_length),
                                '--rate-mib', '16'], capture_output=True, timeout=30)
        assert piece.returncode == 0, piece.stderr
        image = bytearray(segment_length)
        checksum = hashlib.sha256()
        cursor = 24
        while True:
            offset, raw_size, packed_size = struct.unpack('>QII', piece.stdout[cursor:cursor+16])
            cursor += 16
            if offset == (1 << 64) - 1:
                assert raw_size == 0 and packed_size == 0 and piece.stdout[cursor:cursor+32] == checksum.digest()
                break
            block = zlib.decompress(piece.stdout[cursor:cursor+packed_size])
            cursor += packed_size
            assert len(block) == raw_size and offset + raw_size <= segment_length
            image[offset:offset+raw_size] = block
            checksum.update(struct.pack('>QI', offset, raw_size) + block)
        assert bytes(image) == sparse.read_bytes()[begin:begin+segment_length]
        destination = Path(temporary) / 'destination.bin'
        destination.write_bytes(b'preserve')
        rejected = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sparse_transfer.py'),
                                   'receive', str(destination)], input=result.stdout,
                                  capture_output=True, timeout=30,
                                  env={'PATH': '/usr/bin:/bin'})
        assert rejected.returncode != 0 and destination.read_bytes() == b'preserve'
print('Sparse transfer allocation and isolation contracts passed')
