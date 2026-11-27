#!/usr/bin/env python3
"""Exercise reusable Windows build-job staging without starting a VM."""
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from windows_build_job import make_iso, stage_job

root = Path(sys.argv[1]).resolve()
source = root / 'project'
source.mkdir()
(source / 'CMakeLists.txt').write_text('project(example)\n')
(source / 'main.cpp').write_text('int main() { return 0; }\n')
(source / 'build').mkdir()
(source / 'build' / 'stale').write_text('excluded')
(source / 'link').symlink_to(source / 'main.cpp')
recipe = {
    'steps': [
        {'argv': ['C:/iiv/tools/cmake/bin/cmake.exe', '-S', 'source', '-B', 'build'], 'timeout_ms': 60000},
        {'argv': ['C:/iiv/tools/cmake/bin/cmake.exe', '--build', 'build'], 'timeout_ms': 60000},
    ],
    'artifacts': ['build/app.exe'],
}
result = stage_job(source, recipe, root / 'job')
if sys.platform != 'win32':assert (root / 'job').stat().st_mode & 0o077 == 0
staged = root / 'job' / 'media' / 'IIVJOB'
job = json.loads((staged / 'job.json').read_text())
assert job['job'] == result['job'] and job['workspace'] == 'C:/iiv/jobs/' + result['job']
assert job['steps'][0]['argv'] == recipe['steps'][0]['argv']
assert 'C:\\iiv\\tools\\clang\\bin' in job['steps'][0]['path']
assert (staged / 'source' / 'main.cpp').read_text() == (source / 'main.cpp').read_text()
assert not (staged / 'source' / 'build').exists() and not (staged / 'source' / 'link').exists()
assert json.loads((root / 'job' / 'source-manifest.json').read_text())['main.cpp']
batch = (staged / 'stage.cmd').read_bytes()
assert b'\\\\.\\COM1' in batch and result['job'].encode() in batch
assert b'\n' not in batch.replace(b'\r\n', b'')
if sys.platform == 'darwin' or shutil.which('genisoimage'):
    image = make_iso(root / 'job')
    with image.open('rb') as stream:
        stream.seek(32769)
        assert stream.read(5) == b'CD001', 'The staged job did not produce an ISO9660 image'
for bad in [
    {'steps': [], 'artifacts': []},
    {'steps': [{'argv': ['compiler'], 'timeout_ms': 0}], 'artifacts': []},
    {'steps': [{'argv': ['compiler'], 'timeout_ms': 1}], 'artifacts': ['../escape']},
    {'steps': [{'argv': 'not an argument array', 'timeout_ms': 1}], 'artifacts': []},
]:
    try:
        stage_job(source, bad, root / 'bad')
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid recipe accepted')
    assert not (root / 'bad').exists(), 'Invalid recipe left output behind'
print('Reusable Windows job staging contracts passed')
