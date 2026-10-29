#!/usr/bin/env python3
"""Verify resumable SHA-256 state and reject stale or corrupt checkpoints."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

if sys.platform not in ('darwin', 'linux'):
    print('Resumable native SHA-256 tests require macOS or Linux')
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import resumable_digest as digest


def rejects(function):
    try:
        function()
    except (ValueError, OSError):
        return
    raise AssertionError('Invalid verification state was accepted')


data = bytes(range(256)) * 19 + b'last partial block'
expected = hashlib.sha256(data).hexdigest()
for split in (0, 1, 63, 64, 65, 127, 256, len(data)):
    state = digest.NativeSHA256()
    state.update(data[:split])
    restored = digest.NativeSHA256(state.snapshot())
    restored.update(data[split:])
    assert restored.hexdigest() == expected

# The bit count must remain correct beyond OpenSSL's low 32-bit counter.
large_offset = 1 << 29
state = digest.NativeSHA256()
state.context = digest._OpenSSLContext()
state.context.high = 1
state.context.digest_length = 32
state.validate_offset(large_offset)
state.context.buffered = 64
rejects(lambda: state.validate_offset(large_offset))

resources = ROOT / 'resources'
resources.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='digest-contract-', dir=resources) as temporary:
    root = Path(temporary)
    source = root / 'media.bin'
    checkpoint = root / 'media.verify.json'
    source.write_bytes(data)
    fd = os.open(source, os.O_RDONLY)
    try:
        def stop_after_two(offset):
            if offset >= 512:
                raise RuntimeError('simulated interruption')
        try:
            digest.verify_fd(fd, checkpoint, expected, interval=256, block_size=128,
                             on_checkpoint=stop_after_two)
        except RuntimeError as error:
            assert str(error) == 'simulated interruption'
        else:
            raise AssertionError('Interruption did not stop verification')
        assert json.loads(checkpoint.read_text())['offset'] == 512
        original = os.pread
        offsets = []
        def observed(fd, size, offset):
            offsets.append(offset)
            return original(fd, size, offset)
        os.pread = observed
        try:
            assert digest.verify_fd(fd, checkpoint, expected, interval=256, block_size=128) == expected
        finally:
            os.pread = original
        assert offsets and min(offsets) >= 512
        assert json.loads(checkpoint.read_text())['complete'] is True
        os.pread = lambda *_: (_ for _ in ()).throw(AssertionError('Completed input was read again'))
        try:
            assert digest.verify_fd(fd, checkpoint, expected, interval=256, block_size=128) == expected
        finally:
            os.pread = original
        rejects(lambda: digest.verify_fd(fd, checkpoint, '0' * 64, interval=256, block_size=128))
        saved = checkpoint.read_text()
        corrupt = json.loads(saved)
        corrupt['state'] = '00' * (len(corrupt['state']) // 2)
        checkpoint.write_text(json.dumps(corrupt))
        rejects(lambda: digest.verify_fd(fd, checkpoint, expected, interval=256, block_size=128))
        corrupt = json.loads(saved)
        corrupt['offset'] -= 1
        corrupt['checksum'] = digest._checksum({key: value for key, value in corrupt.items() if key != 'checksum'})
        checkpoint.write_text(json.dumps(corrupt))
        rejects(lambda: digest.verify_fd(fd, checkpoint, expected, interval=256, block_size=128))
        checkpoint.write_text(saved)
        before = source.stat()
        with source.open('r+b') as stream:
            stream.write(b'CHANGED')
        os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
        rejects(lambda: digest.verify_fd(fd, checkpoint, expected, interval=256, block_size=128))
    finally:
        os.close(fd)
    link = root / 'link.verify.json'
    link.symlink_to(checkpoint)
    fd = os.open(source, os.O_RDONLY)
    try:
        rejects(lambda: digest.verify_fd(fd, link, expected, interval=256, block_size=128))
    finally:
        os.close(fd)
    crash_source = root / 'crash.bin'
    crash_source.write_bytes(data * 2)
    crash_hash = hashlib.sha256(crash_source.read_bytes()).hexdigest()
    crash_checkpoint = root / 'crash.verify.json'
    crash_code = r'''
import os, signal, sys
sys.path.insert(0, sys.argv[1])
from resumable_digest import verify_fd
fd = os.open(sys.argv[2], os.O_RDONLY)
def terminate(offset):
    if offset >= 512:
        os.kill(os.getpid(), signal.SIGKILL)
verify_fd(fd, sys.argv[3], sys.argv[4], interval=256, block_size=128, on_checkpoint=terminate)
'''
    killed = subprocess.run([sys.executable, '-B', '-c', crash_code, str(ROOT / 'tools'),
                             str(crash_source), str(crash_checkpoint), crash_hash], timeout=30)
    assert killed.returncode == -signal.SIGKILL
    assert json.loads(crash_checkpoint.read_text())['offset'] == 512
    fd = os.open(crash_source, os.O_RDONLY)
    try:
        assert digest.verify_fd(fd, crash_checkpoint, crash_hash, interval=256, block_size=128) == crash_hash
    finally:
        os.close(fd)
print('Resumable native SHA-256 verification contracts passed')
