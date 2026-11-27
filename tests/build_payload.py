#!/usr/bin/env python3
"""Regression checks for frozen sources and literal Windows startup scripts."""
import hashlib
import os
from pathlib import Path
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
import subprocess

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_build_payload import ROOT,create_private_output,extract_zip,snapshot,write_windows_bootstrap

root=Path(sys.argv[1]).resolve()/str(time.time_ns())[-8:]
root.mkdir(parents=True)
private=root/'private'
create_private_output(private)
if sys.platform != 'win32':assert private.stat().st_mode & 0o077 == 0
try:create_private_output(private)
except ValueError:pass
else:raise AssertionError('Existing payload directory accepted')
help_text=subprocess.check_output([sys.executable,str(ROOT/'tools/bootstrap_qemu.py'),'--help'],text=True)
assert '--with-tpm' in help_text, 'QEMU bootstrap must expose its TPM build option'
source=root/'source';source.mkdir()
(source/'Main.cpp').write_bytes(b'preserved source\x00bytes')
for excluded in ['build','.git','__pycache__']:
    (source/excluded).mkdir();(source/excluded/'ignored').write_text('generated')
(source/'resources').mkdir()
(source/'resources'/'asset.txt').write_text('project asset')
(source/'resources'/'.recovery').mkdir()
(source/'resources'/'.recovery'/'secret').write_text('old test state')
(source/'.Main.cpp.iisacc-preview').write_text('old temporary preview')
(source/'.iisacc-temporary.lock').write_text('old lock')
if hasattr(os,'mkfifo'):os.mkfifo(source/'pipe')
(source/'linked').symlink_to(source/'Main.cpp')
inventory=snapshot(source,root/'frozen')
assert inventory=={'Main.cpp':hashlib.sha256((source/'Main.cpp').read_bytes()).hexdigest(),
                   'resources/asset.txt':hashlib.sha256((source/'resources'/'asset.txt').read_bytes()).hexdigest()}
assert (root/'frozen/Main.cpp').read_bytes()==(source/'Main.cpp').read_bytes()
assert not (root/'frozen/resources/.recovery').exists()
sdk_inventory=snapshot(source,root/'sdk-frozen',exclude_resources=True)
assert list(sdk_inventory)==['Main.cpp'] and not (root/'sdk-frozen/resources').exists()

# The initial Windows and Linux jobs honor explicit build concurrency and
# deadlines; invalid limits are rejected before creating an output directory.
generated=root/'low-memory-payload'
subprocess.run([sys.executable,'-B',str(ROOT/'tools/prepare_build_payload.py'),
                '--sources-only','--source-snapshot',str(source),'--output',str(generated),
                '--build-parallel','1','--build-timeout-ms','3600000'],check=True,capture_output=True,text=True,timeout=60)
import json
for job_path in (generated/'windows/Payload/job.json',generated/'linux-job.json'):
    job=json.loads(job_path.read_text())
    build_steps=[s for s in job['steps'] if '--build' in s['argv']]
    assert build_steps and all(s['argv'][s['argv'].index('--parallel')+1]=='1' and
                               s['timeout_ms']==3600000 for s in build_steps)
    assert all(s['timeout_ms']==1800000 for s in job['steps'] if '--build' not in s['argv'])
for option,value in [('--build-parallel','0'),('--build-timeout-ms','0'),
                     ('--build-timeout-ms','3600001')]:
    bad=root/('bad-'+option[2:]+'-'+value)
    result=subprocess.run([sys.executable,'-B',str(ROOT/'tools/prepare_build_payload.py'),
                           '--sources-only','--source-snapshot',str(source),'--output',str(bad),
                           option,value],capture_output=True,text=True,timeout=60)
    assert result.returncode and not bad.exists()

payload=root/'payload';payload.mkdir();write_windows_bootstrap(payload)
for name in ['bootstrap.cmd','start.cmd','register-build.cmd']:
    body=(payload/name).read_bytes()
    assert body.endswith(b'\r\n') and b'\n' not in body.replace(b'\r\n',b'')
start=(payload/'start.cmd').read_bytes().split(b'\r\n')
assert start[0]==b'@echo off'
assert any(b'IIVJOB\\stage.cmd' in line for line in start), 'Startup must detect a new job medium'
legacy=next(line for line in start if b'iiv-build-agent.exe' in line)
assert legacy.endswith(b'2>&1'), 'CMD must retain the complete stderr redirection'
assert b'\\\\.\\COM1 ' in legacy
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
