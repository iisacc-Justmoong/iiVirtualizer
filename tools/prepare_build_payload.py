#!/usr/bin/env python3
"""Snapshot iiSDK sources and make Windows/Linux guest-native build jobs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import uuid
import zipfile
from prepare_os_guests import generated_path

ROOT=Path(__file__).resolve().parents[1]

def sha(path):
    value=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(8*1024*1024),b''):value.update(chunk)
    return value.hexdigest()

def snapshot(source,target):
    hashes={}
    for folder,dirs,files in os.walk(source):
        dirs[:]=[d for d in dirs if d not in ['build','.git','__pycache__','.venv','node_modules']]
        for name in files:
            if name in ['.DS_Store'] or name.endswith('.pyc'):continue
            original=Path(folder)/name
            if original.is_symlink():continue
            relative=original.relative_to(source);dest=target/relative;dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(original,dest);hashes[str(relative)]=sha(dest)
    return hashes

def extract_zip(path,output,expected):
    if sha(path)!=expected:raise ValueError('Toolchain checksum mismatch: '+str(path))
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if name.startswith('/') or '..' in Path(name).parts:raise ValueError('Unsafe toolchain archive')
        archive.extractall(output)

def step(argv,timeout=1800000,**kwargs):return {'argv':list(map(str,argv)),'timeout_ms':timeout,**kwargs}

def write_windows_bootstrap(payload):
    for source,destination in [('windows-start.cmd','start.cmd'),('windows-register-build.cmd','register-build.cmd')]:
        body=(ROOT/'example/guest-build'/source).read_text()
        (payload/destination).write_bytes(body.replace('\n','\r\n').encode('ascii'))
    # Copy complete scripts rather than echoing redirections through another
    # cmd parser: an adjacent 1>> would consume the final 1 of literal 2>&1.
    body=r'''@echo off
if exist C:\iiv\ready.txt exit /b 0
mkdir C:\iiv\work
xcopy /E /I /Y /Q "%~dp0source" C:\iiv\work\source
if errorlevel 1 exit /b 1
xcopy /E /I /Y /Q "%~dp0tools" C:\iiv\tools
if errorlevel 1 exit /b 1
xcopy /E /I /Y /Q "%~dp0agent" C:\iiv\agent
if errorlevel 1 exit /b 1
copy /Y "%~dp0job.json" C:\iiv\job.json
if errorlevel 1 exit /b 1
copy /Y "%~dp0start.cmd" C:\iiv\start.cmd
if errorlevel 1 exit /b 1
copy /Y "%~dp0register-build.cmd" C:\iiv\register-build.cmd
if errorlevel 1 exit /b 1
echo ready>C:\iiv\ready.txt
exit /b 0
'''
    (payload/'bootstrap.cmd').write_bytes(body.replace('\n','\r\n').encode('ascii'))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'build/build-layer/payload')
    parser.add_argument('--agent',type=Path,default=ROOT/'build/build-layer/windows-bootstrap/build/iiv-build-agent.exe')
    parser.add_argument('--sources-only',action='store_true')
    parser.add_argument('--source-snapshot',type=Path)
    args=parser.parse_args();output=generated_path(args.output)
    if output.exists():raise ValueError('Choose a fresh directory beneath build/ or resources/')
    output.mkdir(parents=True);windows=output/'windows';payload=windows/'Payload';source=payload/'source';source.mkdir(parents=True)
    print('Snapshotting SDK source files',flush=True)
    inventory={}
    if args.source_snapshot:
        inventory['frozen']=snapshot(args.source_snapshot,source)
    else:
        for name in ['iiSDKCore','iiJson']:
            inventory[name]=snapshot(ROOT.parent/name,source/name)
        for folder in ['src/builder','example/guest-build']:
            inventory['iiVirtualizer/'+folder]=snapshot(ROOT/folder,source/'iiVirtualizer'/folder)
    (output/'source-manifest.json').write_text(json.dumps(inventory,indent=2)+'\n')
    nonce=uuid.uuid4().hex
    work='C:/iiv/work';cmake='C:/iiv/tools/cmake/bin/cmake.exe';ctest='C:/iiv/tools/cmake/bin/ctest.exe'
    compiler='C:/iiv/tools/clang/bin/x86_64-w64-mingw32-clang++.exe';ninja='C:/iiv/tools/ninja.exe'
    envpath='C:\\iiv\\tools\\clang\\bin;C:\\iiv\\tools\\cmake\\bin;C:\\iiv\\work\\build\\install\\bin;C:\\Windows\\system32;C:\\Windows'
    windows_job={'job':nonce+'-win','workspace':work,'steps':[
        step([compiler,'--version']),
        step(['C:/iiv/tools/clang/python/bin/python3.exe','--version']),
        step([cmake,'-S','source/iiSDKCore','-B','build/core','-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
              '-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG','-DCMAKE_SHARED_LINKER_FLAGS=-static','-DCMAKE_CXX_SCAN_FOR_MODULES=OFF',
              '-DPython3_EXECUTABLE=C:/iiv/tools/clang/python/bin/python3.exe',
              '-DBUILD_SHARED_LIBS=OFF','-DBUILD_TESTING=OFF','-DIISDKCORE_BUILD_EXAMPLES=OFF',
              '-DCMAKE_CXX_COMPILER='+compiler,'-DCMAKE_MAKE_PROGRAM='+ninja,'-DCMAKE_INSTALL_PREFIX='+work+'/build/install']),
        step([cmake,'--build','build/core','--target','install','--parallel','3']),
        step([cmake,'-S','source/iiVirtualizer/example/guest-build','-B','build/app','-G','Ninja',
              '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_SCAN_FOR_MODULES=OFF','-DCMAKE_CXX_COMPILER='+compiler,'-DCMAKE_MAKE_PROGRAM='+ninja,
              '-DCMAKE_PREFIX_PATH='+work+'/build/install']),
        step([cmake,'--build','build/app','--parallel','3']),
        step([ctest,'--test-dir','build/app','--output-on-failure']),
        step([work+'/build/app/iiv-guest-app.exe',nonce+'-win'])],
        'artifacts':['build/app/iiv-guest-app.exe','build/app/libiiSDKCoreObservation.dll']}
    for action in windows_job['steps']:action['path']=envpath
    linux_work='/opt/iiv/work'
    linux_job={'job':nonce+'-linux','workspace':linux_work,'steps':[
        step(['/usr/bin/g++','--version']),
        step(['/usr/bin/cmake','-S','source/iiVirtualizer/example/guest-build','-B','build/app','-G','Ninja',
              '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_PREFIX_PATH='+linux_work+'/build/install']),
        step(['/usr/bin/cmake','--build','build/app','--parallel','3']),
        step(['/usr/bin/ctest','--test-dir','build/app','--output-on-failure']),
        step([linux_work+'/build/app/iiv-guest-app',nonce+'-linux'])],
        'artifacts':['build/app/iiv-guest-app','build/app/libiiSDKCoreObservation.so.0','build/bootstrap.log']}
    (payload/'job.json').write_text(json.dumps(windows_job,indent=2)+'\n')
    (output/'linux-job.json').write_text(json.dumps(linux_job,indent=2)+'\n')
    (output/'jobs.json').write_text(json.dumps({'windows':windows_job,'linux':linux_job},indent=2)+'\n')
    with tarfile.open(output/'sources.tar.gz','w:gz') as archive:archive.add(source,arcname='source')
    if args.sources_only:
        print(json.dumps({'payload':str(output),'source':str(source),'job':linux_job['job']},indent=2));return
    print('Unpacking pinned Windows tools',flush=True)
    downloads=ROOT/'build/build-layer/downloads';tools=payload/'tools';tools.mkdir()
    extract_zip(downloads/'llvm-mingw-20260922-ucrt-x86_64.zip',tools,'e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666')
    (tools/'llvm-mingw-20260922-ucrt-x86_64').rename(tools/'clang')
    extract_zip(downloads/'cmake-3.31.8-windows-x86_64.zip',tools,'81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719')
    (tools/'cmake-3.31.8-windows-x86_64').rename(tools/'cmake')
    extract_zip(downloads/'ninja-win.zip',tools,'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a')
    agent=payload/'agent';agent.mkdir();shutil.copy2(args.agent,agent/'iiv-build-agent.exe')
    bootstrap=ROOT/'build/build-layer/windows-core/install/bin'
    for dll in bootstrap.glob('*.dll'):shutil.copy2(dll,agent/dll.name)
    for name in ['libc++.dll','libunwind.dll','libwinpthread-1.dll']:
        original=tools/'clang/bin'/name
        if original.exists():shutil.copy2(original,agent/name)
    write_windows_bootstrap(payload)
    password='Iiv2026-'+uuid.uuid4().hex
    template=(ROOT/'example/guest-build/Autounattend.xml.in').read_text()
    (windows/'Autounattend.xml').write_text(template.replace('@PASSWORD@',password))
    print('Building Windows answer/payload ISO',flush=True)
    subprocess.run(['hdiutil','makehybrid','-iso','-joliet','-iso-volume-name','IIVBUILD','-joliet-volume-name','IIVBUILD',
                    '-o',str(output/'windows-payload.iso'),str(windows)],check=True)
    print(json.dumps({'payload':str(output),'job_windows':windows_job['job'],'job_linux':linux_job['job'],
                      'sources_sha256':sha(output/'sources.tar.gz'),'iso_sha256':sha(output/'windows-payload.iso')},indent=2))

if __name__=='__main__':main()
