#!/usr/bin/env python3
"""Check resumable transfer planning, receipts and safe reconstruction."""
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from chunk_transfer import (assemble_parts, make_checkpoint, matches_receipts, plan_ranges,
                            record_part, validate_checkpoint, write_checkpoint)

assert plan_ranges(10, 4) == [(0, 4), (4, 4), (8, 2)]
for size, chunk in ((0, 1), (1, 0), (-1, 1)):
    try:
        plan_ranges(size, chunk)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid chunk range accepted')
digest = 'a' * 64
sent = {'complete': True, 'sha256': digest, 'data_sent': 3, 'source_offset': 4, 'source_length': 4}
received = {'complete': True, 'sha256': digest, 'written': 3, 'logical_size': 4}
assert matches_receipts(sent, received, 4, 4)
assert not matches_receipts(sent, dict(received, sha256='b' * 64), 4, 4)
assert not matches_receipts(sent, dict(received, written=2), 4, 4)
assert not matches_receipts(sent, received, 5, 4)
if os.name != 'posix':
    print('Chunk transfer filesystem tests require POSIX')
    sys.exit(77)

with tempfile.TemporaryDirectory(dir=ROOT / 'resources/platform-guests') as temporary:
    base = Path(temporary)
    source = base / 'source.bin'
    source.write_bytes(b'abcdefghij')
    token = '1' * 32
    checkpoint = make_checkpoint(source, 4, 'iiv-test-0123456789ab', token)
    assert validate_checkpoint(checkpoint, source, 'iiv-test-0123456789ab')
    saved = base / 'checkpoint.json'
    write_checkpoint(saved, checkpoint)
    old_checkpoint = json.loads(saved.read_text())
    first_sender = dict(sent, source_offset=0, source_length=4)
    record_part(checkpoint, 'part-0000-' + token + '.bin', first_sender, received)
    assert len(checkpoint['parts']) == 1
    write_checkpoint(saved, checkpoint)
    try:
        write_checkpoint(saved, old_checkpoint)
    except ValueError:
        pass
    else:
        raise AssertionError('Completed checkpoint was rolled back')
    foreign = base / 'foreign.json'
    foreign.write_text('preserve')
    try:
        write_checkpoint(foreign, checkpoint)
    except ValueError:
        pass
    else:
        raise AssertionError('Unrelated checkpoint was overwritten')
    assert foreign.read_text() == 'preserve'
    try:
        record_part(checkpoint, '../outside', sent, received)
    except ValueError:
        pass
    else:
        raise AssertionError('Unsafe part name was accepted')
    source.write_bytes(b'changed-source')
    try:
        validate_checkpoint(checkpoint, source, 'iiv-test-0123456789ab')
    except ValueError:
        pass
    else:
        raise AssertionError('Changed source was accepted')
    directory = base / 'parts'
    directory.mkdir()
    (directory / '.owner').write_text(token + '\n')
    data = b'abcdefghij'
    parts = []
    for index, (offset, length) in enumerate(plan_ranges(len(data), 4)):
        name = 'part-%04d-%s.bin' % (index, token)
        (directory / name).write_bytes(data[offset:offset+length])
        parts.append({'name': name, 'offset': offset, 'length': length})
    target = base / 'result.bin'
    sha = hashlib.sha256(data).hexdigest()
    with patch('chunk_transfer.os.statvfs', return_value=SimpleNamespace(f_bavail=0, f_frsize=4096)):
        try:
            assemble_parts(directory, parts, target, sha, len(data), token)
        except RuntimeError:
            pass
        else:
            raise AssertionError('Assembly started without reserved space')
    assert not target.exists()
    # Exercise the same periodic durability and cache-release path on a tiny
    # fixture, while preserving the complete-image checksum contract.
    with patch('chunk_transfer.ASSEMBLY_FLUSH_BYTES', 4), \
         patch('chunk_transfer.os.fsync', wraps=os.fsync) as sync, \
         patch('chunk_transfer.release_written_cache', return_value=True) as release:
        assert assemble_parts(directory, parts, target, sha, len(data), token) == sha
        assert sync.call_count >= 4
        assert release.call_count >= 6
    assert target.read_bytes() == data
    try:
        assemble_parts(directory, parts, target, sha, len(data), token)
    except FileExistsError:
        pass
    else:
        raise AssertionError('Existing output was overwritten')
    assert target.read_bytes() == data
    bad_parts = [dict(parts[0], name='../result.bin'), *parts[1:]]
    try:
        assemble_parts(directory, bad_parts, base / 'bad.bin', sha, len(data), token)
    except ValueError:
        pass
    else:
        raise AssertionError('Part path traversal was accepted')
    assert not (base / 'bad.bin').exists()
    first_part = directory / parts[0]['name']
    saved_part = directory / 'saved-part.bin'
    first_part.rename(saved_part)
    first_part.symlink_to(source)
    try:
        assemble_parts(directory, parts, base / 'symlink.bin', sha, len(data), token)
    except ValueError:
        pass
    else:
        raise AssertionError('Symlink transfer chunk was accepted')
    assert not (base / 'symlink.bin').exists()
    first_part.unlink()
    saved_part.rename(first_part)
    try:
        assemble_parts(directory, parts, base / 'wrong.bin', 'f' * 64, len(data), token)
    except ValueError:
        pass
    else:
        raise AssertionError('Incorrect complete-image hash was accepted')
    # A failed assembly must resume only after its existing prefix has been
    # checked against the original chunks, without overwriting foreign files.
    real_pwrite = os.pwrite
    def interrupt_after_first_part(fd, block, offset):
        if offset >= 4:
            raise RuntimeError('simulated interruption')
        return real_pwrite(fd, block, offset)
    resumed = base / 'resumed.bin'
    with patch('chunk_transfer.os.pwrite', side_effect=interrupt_after_first_part):
        try:
            assemble_parts(directory, parts, resumed, sha, len(data), token, resume=True)
        except RuntimeError as error:
            assert str(error) == 'simulated interruption'
        else:
            raise AssertionError('Assembly interruption was not propagated')
    assert resumed.read_bytes() == data[:4]
    assert assemble_parts(directory, parts, resumed, sha, len(data), token, resume=True) == sha
    assert resumed.read_bytes() == data
    # A complete owned output only needs its actual bytes rehashed; reading all
    # the transfer chunks again is unnecessary and slows interrupted recovery.
    real_open = os.open
    def reject_chunk_open(path, *args, **kwargs):
        if Path(path).parent == directory and Path(path).name.startswith('part-'):
            raise AssertionError('Complete output reopened a transfer chunk')
        return real_open(path, *args, **kwargs)
    with patch('chunk_transfer.os.open', side_effect=reject_chunk_open):
        assert assemble_parts(directory, parts, resumed, sha, len(data), token, resume=True) == sha
    verification = base / 'resumed.verify.json'
    assert assemble_parts(directory, parts, resumed, sha, len(data), token, resume=True,
                          verification_checkpoint=verification) == sha
    assert json.loads(verification.read_text())['complete'] is True
    with patch('resumable_digest.os.pread', side_effect=AssertionError('Verified output was read again')):
        assert assemble_parts(directory, parts, resumed, sha, len(data), token, resume=True,
                              verification_checkpoint=verification) == sha
    completed_corrupt = base / 'completed-corrupt.bin'
    assert assemble_parts(directory, parts, completed_corrupt, sha, len(data), token, resume=True) == sha
    completed_corrupt.write_bytes(b'X' + data[1:])
    try:
        assemble_parts(directory, parts, completed_corrupt, sha, len(data), token, resume=True)
    except ValueError:
        pass
    else:
        raise AssertionError('A corrupt full-sized output was accepted on resume')
    changed = base / 'changed.bin'
    with patch('chunk_transfer.os.pwrite', side_effect=interrupt_after_first_part):
        try:
            assemble_parts(directory, parts, changed, sha, len(data), token, resume=True)
        except RuntimeError:
            pass
    changed.write_bytes(b'bad!')
    try:
        assemble_parts(directory, parts, changed, sha, len(data), token, resume=True)
    except ValueError:
        pass
    else:
        raise AssertionError('Corrupt output prefix was accepted')
    assert changed.read_bytes() == b'bad!'
    unrelated = base / 'unrelated.bin'
    unrelated.write_bytes(b'preserve')
    try:
        assemble_parts(directory, parts, unrelated, sha, len(data), token, resume=True)
    except ValueError:
        pass
    else:
        raise AssertionError('Unowned existing output was accepted')
    assert unrelated.read_bytes() == b'preserve'
    missing = base / 'missing.bin'
    with patch('chunk_transfer.os.pwrite', side_effect=interrupt_after_first_part):
        try:
            assemble_parts(directory, parts, missing, sha, len(data), token, resume=True)
        except RuntimeError:
            pass
    missing.unlink()
    try:
        assemble_parts(directory, parts, missing, sha, len(data), token, resume=True)
    except ValueError:
        pass
    else:
        raise AssertionError('Missing owned output was silently replaced')
    assert not missing.exists()
    mismatched = base / 'mismatched.bin'
    with patch('chunk_transfer.os.pwrite', side_effect=interrupt_after_first_part):
        try:
            assemble_parts(directory, parts, mismatched, sha, len(data), token, resume=True)
        except RuntimeError:
            pass
    try:
        assemble_parts(directory, parts, mismatched, 'f' * 64, len(data), token, resume=True)
    except ValueError:
        pass
    else:
        raise AssertionError('A different expected hash reused the partial output')
    assert mismatched.read_bytes() == data[:4]
    def corrupt_write(fd, block, offset):
        if offset == 0:
            block = b'X' + block[1:]
        return real_pwrite(fd, block, offset)
    with patch('chunk_transfer.os.pwrite', side_effect=corrupt_write):
        try:
            assemble_parts(directory, parts, base / 'corrupt-output.bin', sha, len(data), token)
        except ValueError:
            pass
        else:
            raise AssertionError('Corrupted assembled output was reported as complete')
    interrupt_source = base / 'interrupt-source.bin'
    interrupt_source.write_bytes(b'x' * (2 * 1024**2))
    child_code = r'''
import sys
sys.path.insert(0, sys.argv[1])
import chunk_transfer
from test_isolation import TestInterrupted, install_interrupt_handlers
chunk_transfer.ssh_command = lambda container, argv, stdin=False: [sys.executable, '-B', '-c',
    'import sys,time; sys.stdin.buffer.read(1); time.sleep(60)']
install_interrupt_handlers()
try:
    chunk_transfer.transfer_one({'source': sys.argv[2], 'remote_dir': '/unused', 'container': 'unused'},
                                'part-0000-' + '1'*32 + '.bin', 0, 2*1024**2, 1)
except TestInterrupted:
    print('IIV_INTERRUPT_HANDLED', flush=True)
else:
    raise AssertionError('Transfer was not interrupted')
'''
    child = subprocess.Popen([sys.executable, '-B', '-c', child_code, str(ROOT / 'tools'),
                              str(interrupt_source)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, start_new_session=True)
    try:
        ready, _, _ = select.select([child.stdout], [], [], 30)
        assert ready and json.loads(child.stdout.readline())['event'] == 'chunk-active'
        child.terminate()
        out, err = child.communicate(timeout=30)
        assert child.returncode == 0 and 'IIV_INTERRUPT_HANDLED' in out, err
    finally:
        if child.poll() is None:
            assert os.getpgid(child.pid) == child.pid
            os.killpg(child.pid, signal.SIGTERM)
            child.wait(timeout=10)
print('Chunk planning, receipts and reconstruction contracts passed')
