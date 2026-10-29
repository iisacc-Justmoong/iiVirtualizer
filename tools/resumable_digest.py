"""Checkpoint an actual native SHA-256 calculation for a private regular file.

The caller must exclusively lock the file descriptor for the duration of the
calculation. Checkpoints are private task state, not an authentication boundary.
"""
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import sys
import uuid

from sparse_transfer import release_written_cache

ROOT = Path(__file__).resolve().parents[1]
BLOCK = 1024 * 1024
CHECKPOINT_INTERVAL = 64 * BLOCK


class _CommonCryptoContext(ctypes.Structure):
    _fields_ = [('count', ctypes.c_uint32 * 2), ('state', ctypes.c_uint32 * 8),
                ('buffer', ctypes.c_uint32 * 16)]


class _OpenSSLContext(ctypes.Structure):
    # The public OpenSSL SHA256_CTX layout, for OpenSSL 1.1 and 3.x.
    _fields_ = [('state', ctypes.c_uint32 * 8), ('low', ctypes.c_uint32),
                ('high', ctypes.c_uint32), ('buffer', ctypes.c_uint32 * 16),
                ('buffered', ctypes.c_uint), ('digest_length', ctypes.c_uint)]


def _load_provider():
    if sys.platform == 'darwin':
        context = _CommonCryptoContext
        library = ctypes.CDLL('/usr/lib/system/libcommonCrypto.dylib')
        prefix = 'CC_SHA256_'
        label = 'commoncrypto:' + platform.mac_ver()[0]
        length_type = ctypes.c_uint32
    elif sys.platform == 'linux':
        name = ctypes.util.find_library('crypto')
        if not name:
            raise RuntimeError('OpenSSL is required for resumable SHA-256')
        library = ctypes.CDLL(name)
        library.OpenSSL_version.argtypes = [ctypes.c_int]
        library.OpenSSL_version.restype = ctypes.c_char_p
        version = library.OpenSSL_version(0).decode('ascii')
        if not re.match(r'^OpenSSL (?:1\.1\.|3\.)', version):
            raise RuntimeError('Unsupported OpenSSL SHA-256 context')
        context = _OpenSSLContext
        prefix = 'SHA256_'
        label = 'openssl:' + version
        length_type = ctypes.c_size_t
    else:
        raise RuntimeError('Resumable SHA-256 requires macOS or Linux')
    initialize = getattr(library, prefix + 'Init')
    update = getattr(library, prefix + 'Update')
    finish = getattr(library, prefix + 'Final')
    initialize.argtypes, initialize.restype = [ctypes.POINTER(context)], ctypes.c_int
    update.argtypes, update.restype = [ctypes.POINTER(context), ctypes.c_void_p, length_type], ctypes.c_int
    finish.argtypes, finish.restype = [ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(context)], ctypes.c_int
    label += ':' + platform.machine() + ':' + sys.byteorder + ':' + str(ctypes.sizeof(context))
    # Detect an incompatible native ABI before trusting a serialized context.
    sample = b'a' * 65 + b'resumable-digest-abi-check'
    current = context()
    if initialize(ctypes.byref(current)) != 1 or update(ctypes.byref(current), sample[:65], 65) != 1:
        raise RuntimeError('Native SHA-256 initialization failed')
    current = context.from_buffer_copy(bytes(current))
    if update(ctypes.byref(current), sample[65:], len(sample) - 65) != 1:
        raise RuntimeError('Native SHA-256 update failed')
    output = (ctypes.c_ubyte * 32)()
    if finish(output, ctypes.byref(current)) != 1 or bytes(output).hex() != hashlib.sha256(sample).hexdigest():
        raise RuntimeError('Native SHA-256 context failed its ABI check')
    return context, library, initialize, update, finish, label


_PROVIDER = None


class NativeSHA256:
    def __init__(self, snapshot=None):
        global _PROVIDER
        if _PROVIDER is None:
            _PROVIDER = _load_provider()
        context, _, initialize, _, _, self.provider = _PROVIDER
        if snapshot is None:
            self.context = context()
            if initialize(ctypes.byref(self.context)) != 1:
                raise RuntimeError('Native SHA-256 initialization failed')
        else:
            if not isinstance(snapshot, bytes) or len(snapshot) != ctypes.sizeof(context):
                raise ValueError('Invalid SHA-256 context length')
            self.context = context.from_buffer_copy(snapshot)

    def update(self, data):
        if data and _PROVIDER[3](ctypes.byref(self.context), data, len(data)) != 1:
            raise RuntimeError('Native SHA-256 update failed')

    def snapshot(self):
        return bytes(self.context)

    def validate_offset(self, offset):
        if isinstance(self.context, _OpenSSLContext):
            bit_count = (self.context.high << 32) | self.context.low
            if (self.context.buffered > 63 or self.context.digest_length != 32 or
                    self.context.buffered != offset % 64 or bit_count != (offset * 8) % (1 << 64)):
                raise ValueError('Invalid OpenSSL SHA-256 checkpoint state')
        else:
            byte_count = (self.context.count[1] << 32) | self.context.count[0]
            if byte_count != offset % (1 << 64):
                raise ValueError('Invalid CommonCrypto SHA-256 checkpoint state')

    def hexdigest(self):
        clone = type(self)(self.snapshot())
        result = (ctypes.c_ubyte * 32)()
        if _PROVIDER[4](result, ctypes.byref(clone.context)) != 1:
            raise RuntimeError('Native SHA-256 finalization failed')
        return bytes(result).hex()


def _identity(fd):
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode):
        raise ValueError('Verification input must be a regular file')
    return {'device': info.st_dev, 'inode': info.st_ino, 'size': info.st_size,
            'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns}


def _checksum(record):
    return hashlib.sha256(json.dumps(record, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def _load_checkpoint(path):
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
            raise ValueError('Invalid verification checkpoint')
        record = json.loads(os.read(fd, info.st_size))
        if not isinstance(record, dict):
            raise ValueError('Invalid verification checkpoint')
        checksum = record.pop('checksum', None)
        if not isinstance(checksum, str) or checksum != _checksum(record):
            raise ValueError('Verification checkpoint checksum mismatch')
        return record, (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
    finally:
        os.close(fd)


def _write_checkpoint(path, record, previous):
    directory = path.parent
    data = (json.dumps(dict(record, checksum=_checksum(record)), sort_keys=True) + '\n').encode()
    temporary = directory / ('.iiv-verify-' + uuid.uuid4().hex + '.tmp')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    try:
        offset = 0
        while offset < len(data):
            count = os.write(fd, data[offset:])
            if count <= 0:
                raise OSError('Could not write verification checkpoint')
            offset += count
        os.fsync(fd)
    finally:
        os.close(fd)
    try:
        if previous is None:
            os.link(temporary, path)
            temporary.unlink()
        else:
            current = path.lstat()
            current_identity = (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns, current.st_ctime_ns)
            if not stat.S_ISREG(current.st_mode) or current_identity != previous:
                raise ValueError('Verification checkpoint identity changed')
            os.replace(temporary, path)
        directory_fd = os.open(directory, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        current = path.lstat()
        return current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns, current.st_ctime_ns
    finally:
        if temporary.exists():
            temporary.unlink()


def verify_fd(fd, checkpoint, expected, *, interval=CHECKPOINT_INTERVAL, block_size=BLOCK, on_checkpoint=None):
    if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('Invalid expected SHA-256')
    if not isinstance(interval, int) or not isinstance(block_size, int) or interval < 1 or not 1 <= block_size <= 16 * BLOCK:
        raise ValueError('Invalid verification interval or block size')
    checkpoint = Path(checkpoint)
    resources = (ROOT / 'resources').resolve(strict=True)
    parent = checkpoint.parent.resolve(strict=True)
    if not parent.is_relative_to(resources) or checkpoint.is_symlink():
        raise ValueError('Verification checkpoint must be in private resources')
    checkpoint = parent / checkpoint.name
    identity = _identity(fd)
    state = NativeSHA256()
    previous = None
    offset = 0
    if checkpoint.exists():
        record, previous = _load_checkpoint(checkpoint)
        if (set(record) != {'schema', 'provider', 'identity', 'expected_sha256', 'offset', 'state', 'complete'} or
                record['schema'] != 1 or record['provider'] != state.provider or record['identity'] != identity or
                record['expected_sha256'] != expected or type(record['offset']) is not int or
                not 0 <= record['offset'] <= identity['size'] or type(record['complete']) is not bool or
                not isinstance(record['state'], str)):
            raise ValueError('Verification checkpoint does not match this file and provider')
        try:
            state = NativeSHA256(bytes.fromhex(record['state']))
        except ValueError as error:
            raise ValueError('Invalid saved SHA-256 context') from error
        offset = record['offset']
        state.validate_offset(offset)
        if record['complete']:
            if offset != identity['size'] or state.hexdigest() != expected or _identity(fd) != identity:
                raise ValueError('Invalid completed verification checkpoint')
            return expected

    def save(complete):
        nonlocal previous
        if _identity(fd) != identity:
            raise ValueError('Verification input changed while reading')
        record = {'schema': 1, 'provider': state.provider, 'identity': identity,
                  'expected_sha256': expected, 'offset': offset, 'state': state.snapshot().hex(),
                  'complete': complete}
        previous = _write_checkpoint(checkpoint, record, previous)
        release_written_cache(fd)
        if on_checkpoint is not None:
            on_checkpoint(offset)

    if previous is None:
        save(False)
    since_checkpoint = 0
    while offset < identity['size']:
        data = os.pread(fd, min(block_size, identity['size'] - offset), offset)
        if not data:
            raise ValueError('Verification input was truncated')
        state.update(data)
        offset += len(data)
        since_checkpoint += len(data)
        if since_checkpoint >= interval and offset < identity['size']:
            save(False)
            since_checkpoint = 0
    if _identity(fd) != identity or state.hexdigest() != expected:
        raise ValueError('Verification input changed or SHA-256 mismatched')
    save(True)
    return expected
