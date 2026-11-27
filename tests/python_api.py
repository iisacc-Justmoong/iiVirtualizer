#!/usr/bin/env python3
"""Exercise the installed Python-facing VM lifecycle without booting a guest."""
import json
import os
from pathlib import Path
import sys

root = Path(sys.argv[2]).resolve()
sys.path.insert(0, os.environ.get('IIV_PYTHON_API_PATH', str(Path(__file__).resolve().parents[1] / 'src/python')))
from ii_virtualizer import Virtualizer, VirtualizerError

installer, payload, replacement = (root / name for name in ('installer.iso', 'payload.iso', 'next.iso'))
installer.write_bytes(b'installer fixture')
payload.write_bytes(b'first job')
replacement.write_bytes(b'next job')
api = Virtualizer(sys.argv[1], root / 'vms', tcg_cache_mib=64)
vm = api.create_windows('python-windows', installer, payload, disk_mib=16, media_copy_rate=4, direct_io=True, accelerator='tcg')
assert vm['state'] == 'stopped' and vm['boot_mode'] == 'disk'
assert vm['guest_channel'] == 'serial' and vm['boot_disk_first'] and vm['direct_io']
plan = api.plan('python-windows')
assert plan[plan.index('-accel')+1] == 'tcg,tb-size=64'
updated = api.replace_payload('python-windows', replacement)
assert updated['uuid'] == vm['uuid']
assert Path(updated['payload_iso']).read_bytes() == b'next job'
assert api.describe('python-windows')['uuid'] == vm['uuid']
assert api.remove('python-windows')['removed'] == 'python-windows'
try:
    api.describe('python-windows')
except VirtualizerError as error:
    assert error.returncode != 0
else:
    raise AssertionError('Missing VM did not surface an error')
assert installer.read_bytes() == b'installer fixture' and payload.read_bytes() == b'first job'
for invalid in (-1, 1025, True, '64'):
    try:
        Virtualizer(sys.argv[1], root / 'vms', tcg_cache_mib=invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid TCG cache size accepted')
print('Python VM control and payload replacement passed')
