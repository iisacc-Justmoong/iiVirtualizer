#!/usr/bin/env python3
"""Validate the installed Python adapter and exclude temporary document copies."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

cmake, build, cli, source, resources = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='python-install-', dir=build) as prefix:
    subprocess.run([cmake, '--install', build, '--prefix', prefix],
                   check=True, capture_output=True, text=True, timeout=120)
    share = Path(prefix) / 'share/iiVirtualizer'
    api = share / 'python/ii_virtualizer.py'
    assert api.is_file(), 'Python adapter was not installed'
    unexpected = [path for path in (share / 'docs').rglob('*')
                  if path.name == '.DS_Store' or path.name == '.iisacc-temporary.lock'
                  or path.name.endswith('.iisacc-preview')]
    assert not unexpected, 'Private temporary files entered the install: ' + str(unexpected)
    with tempfile.TemporaryDirectory(prefix='iiv-py-', dir=resources) as workspace:
        env = dict(os.environ, IIV_PYTHON_API_PATH=str(api.parent))
        subprocess.run([sys.executable, '-B', str(Path(source) / 'tests/python_api.py'), cli, workspace],
                       env=env, check=True, timeout=60)
print('Installed Python adapter and documentation package passed')
