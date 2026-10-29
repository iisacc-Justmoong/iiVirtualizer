#!/usr/bin/env python3
"""Regression checks for frozen sources and literal Windows startup scripts."""
import hashlib
from pathlib import Path
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
import subprocess

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_build_payload import ROOT,extract_zip,snapshot,write_windows_bootstrap

root=Path(sys.argv[1]).resolve()/str(time.time_ns())[-8:]
root.mkdir(parents=True)
help_text=subprocess.check_output([sys.executable,str(ROOT/'tools/bootstrap_qemu.py'),'--help'],text=True)
assert '--with-tpm' in help_text, 'QEMU bootstrap must expose its TPM build option'
source=root/'source';source.mkdir()
(source/'Main.cpp').write_bytes(b'preserved source\x00bytes')
for excluded in ['build','.git','__pycache__']:
    (source/excluded).mkdir();(source/excluded/'ignored').write_text('generated')
(source/'linked').symlink_to(source/'Main.cpp')
inventory=snapshot(source,root/'frozen')
assert inventory=={'Main.cpp':hashlib.sha256((source/'Main.cpp').read_bytes()).hexdigest()}
assert (root/'frozen/Main.cpp').read_bytes()==(source/'Main.cpp').read_bytes()

payload=root/'payload';payload.mkdir();write_windows_bootstrap(payload)
for name in ['bootstrap.cmd','start.cmd','register-build.cmd']:
    body=(payload/name).read_bytes()
    assert body.endswith(b'\r\n') and b'\n' not in body.replace(b'\r\n',b'')
start=(payload/'start.cmd').read_bytes().split(b'\r\n')
assert len(start)==3 and start[0]==b'@echo off'
assert start[1].endswith(b'2>&1'), 'CMD must retain the complete stderr redirection'
assert b'\\\\.\\COM1 ' in start[1]
bootstrap=(payload/'bootstrap.cmd').read_text()
assert 'echo C:' not in bootstrap and 'copy /Y "%~dp0start.cmd"' in bootstrap
assert 'copy /Y "%~dp0register-build.cmd"' in bootstrap
task=(payload/'register-build.cmd').read_text()
assert '/SC ONSTART' in task and '/RU SYSTEM' in task and 'schtasks /Run' in task
ns={'u':'urn:schemas-microsoft-com:unattend'}
answer=ET.parse(ROOT/'example/guest-build/Autounattend.xml.in')
assert any('register-build.cmd' in (node.text or '') for node in answer.findall('.//u:CommandLine',ns))

archive=root/'tool.zip'
with zipfile.ZipFile(archive,'w') as package:package.writestr('../escaped','unsafe')
checksum=hashlib.sha256(archive.read_bytes()).hexdigest()
for expected in ['0'*64,checksum]:
    try:extract_zip(archive,root/'tool',expected)
    except ValueError:pass
    else:raise AssertionError('Invalid or escaping toolchain archive accepted')
assert not (root/'escaped').exists()
print('Source freeze, toolchain integrity, CRLF startup and scheduled Windows build contracts passed')
