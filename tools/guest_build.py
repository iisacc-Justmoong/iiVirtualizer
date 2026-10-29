#!/usr/bin/env python3
"""Boot an owned build VM, validate native build events, and recover artifacts."""
import argparse
import base64
from datetime import datetime,timezone
import hashlib
import json
from pathlib import Path,PurePosixPath
import socket
import struct
import subprocess
import time
from prepare_os_guests import generated_path
from test_isolation import require_test_container

ROOT=Path(__file__).resolve().parents[1]

def artifact_path(root,name):
    path=PurePosixPath(name)
    if path.is_absolute() or not path.parts or any(x in ['..','.'] for x in path.parts) or '\\' in name or ':' in name:
        raise ValueError('Unsafe artifact path')
    destination=root.joinpath(*path.parts)
    if root.resolve() not in destination.resolve().parents:raise ValueError('Artifact escaped output')
    return destination

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest',choices=['linux','windows'],required=True)
    parser.add_argument('--runtime',type=Path,default=ROOT/'build/iivirtualizer')
    parser.add_argument('--qemu',type=Path,required=True)
    parser.add_argument('--root',type=Path,default=ROOT/'build/gb')
    parser.add_argument('--id',required=True)
    parser.add_argument('--job',type=Path,required=True)
    parser.add_argument('--image',type=Path)
    parser.add_argument('--boot-iso',type=Path)
    parser.add_argument('--payload-iso',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--timeout',type=int,default=5400)
    parser.add_argument('--startup-timeout',type=int,default=30)
    parser.add_argument('--control-timeout',type=int,default=5)
    parser.add_argument('--reuse',action='store_true')
    parser.add_argument('--clone-base',action='store_true')
    args=parser.parse_args();require_test_container('tcg')
    output=generated_path(args.output);root=generated_path(args.root)
    if output.exists():
        raise ValueError('Choose a fresh evidence directory beneath build/ or resources/')
    output.mkdir(parents=True);(output/'artifacts').mkdir();job=json.loads(args.job.read_text())
    commands=[]
    architecture='x86_64' if args.guest=='windows' else json.loads(args.image.read_text())['architecture']
    def run(command,*options):
        argv=[str(args.runtime.resolve()),command,args.id,'--root',str(root),
              '--qemu-'+architecture,str(args.qemu.resolve()),'--startup-timeout',str(args.startup_timeout),
              '--control-timeout',str(args.control_timeout),*map(str,options)]
        done=subprocess.run(argv,capture_output=True,text=True,timeout=3600)
        commands.append({'argv':argv,'returncode':done.returncode,'stdout':done.stdout,'stderr':done.stderr})
        (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if done.returncode:raise RuntimeError(done.stderr)
        return json.loads(done.stdout)
    if args.reuse:
        vm=run('describe')
        if vm['state']!='stopped':raise ValueError('Only a stopped VM can be reused')
    elif args.guest=='windows':
        vm=run('create','--arch','x86_64','--accel','tcg','--boot','disk','--boot-iso',args.boot_iso.resolve(),
               '--payload-iso',args.payload_iso.resolve(),'--disk-bus','sata','--guest-channel','serial',
               '--graphics','vga','--allow-reboot','--cpus','4','--memory','6144','--disk','49152')
    else:
        image=json.loads(args.image.read_text())
        vm=run('create','--arch',architecture,'--accel','tcg','--kernel',image['kernel'],'--initramfs',image['initramfs'],
               '--base-disk',image['base_disk'],'--kernel-args',image['kernel_command_line'],
               '--cpus','3','--memory','3072','--disk','16384','--network',*(['--clone-base'] if args.clone_base else []))
    plan=run('plan');(output/'plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    selected_accelerator=plan[plan.index('-accel')+1]
    if selected_accelerator!='tcg':raise ValueError('Reused test VM must select TCG')
    began=time.monotonic();vm=run('start');completed=False;started=False;exits={};files={};artifacts={};log=bytearray();events=[]
    try:
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as channel,(output/'stream.bin').open('wb') as stream:
            channel.settimeout(2)
            channel.connect(str(Path(vm['directory'])/'agent.sock'))
            buffer=bytearray();last_progress=0
            while time.monotonic()-began<args.timeout and not completed:
                try:
                    chunk=channel.recv(65536)
                    if not chunk:raise RuntimeError('Build channel closed before completion')
                    stream.write(chunk);stream.flush();buffer.extend(chunk)
                except socket.timeout:continue
                while b'\n' in buffer:
                    line,_,remaining=buffer.partition(b'\n');buffer=bytearray(remaining)
                    at=line.find(b'IIVB1 ')
                    if at<0:continue
                    if len(line)-at>32768:raise ValueError('Oversized build event')
                    event=json.loads(line[at+6:]);events.append(event)
                    if event.get('job')!=job['job']:raise ValueError('Build job identity mismatch')
                    kind=event['kind']
                    if kind=='started':
                        if started:raise ValueError('Guest restarted the build job')
                        if event['os'].lower()!=args.guest or event['architecture']!=architecture:raise ValueError('Guest OS/architecture mismatch')
                        started=True
                    elif not started:raise ValueError('Build events arrived before guest identity')
                    elif kind=='log':log.extend(base64.b64decode(event['data'],validate=True))
                    elif kind=='exit':
                        index=event['index']
                        if index in exits or event['code']!=0 or event['reason']!=0:raise ValueError('Guest build step failed')
                        exits[index]=True
                    elif kind in ['failed','error']:raise RuntimeError('Guest build failed: '+str(event))
                    elif kind=='artifact':
                        name=event['path']
                        if name not in job['artifacts'] or name in files or not 0<=event['size']<=32*1024*1024:raise ValueError('Invalid artifact declaration')
                        path=artifact_path(output/'artifacts',name);path.parent.mkdir(parents=True,exist_ok=True)
                        files[name]={'stream':path.open('xb'),'size':event['size'],'offset':0,'sha256':hashlib.sha256()}
                    elif kind=='chunk':
                        state=files[event['path']];data=base64.b64decode(event['data'],validate=True)
                        if event['offset']!=state['offset'] or len(data)>8192 or state['offset']+len(data)>state['size']:raise ValueError('Artifact chunk bounds mismatch')
                        state['stream'].write(data);state['sha256'].update(data);state['offset']+=len(data)
                    elif kind=='artifact-end':
                        name=event['path'];state=files[name]
                        if state['offset']!=state['size'] or event['size']!=state['size']:raise ValueError('Truncated artifact')
                        state['stream'].close();artifacts[name]={'size':state['size'],'sha256':state['sha256'].hexdigest()}
                    elif kind=='complete':
                        if set(exits)!=set(range(len(job['steps']))) or event['steps']!=len(job['steps']) or set(artifacts)!=set(job['artifacts']):raise ValueError('Incomplete native build proof')
                        if ('IIV_GUEST_APP_OK' not in log.decode(errors='replace') or job['job'] not in log.decode(errors='replace')):raise ValueError('Guest-built application did not execute')
                        completed=True
                    if kind!='log' or time.monotonic()-last_progress>5:
                        (output/'progress.json').write_text(json.dumps({'seconds':round(time.monotonic()-began,1),'event':{k:v for k,v in event.items() if k!='data'}},indent=2)+'\n');last_progress=time.monotonic()
                if len(buffer)>65536:raise ValueError('Unbounded build channel line')
        if not completed:raise TimeoutError('Guest native build timed out')
        binary=next(name for name in job['artifacts'] if 'iiv-guest-app' in name)
        data=artifact_path(output/'artifacts',binary).read_bytes()
        if args.guest=='windows':
            if data[:2]!=b'MZ':raise ValueError('Missing Windows PE artifact')
            offset=struct.unpack_from('<I',data,60)[0]
            if data[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',data,offset+4)[0]!=0x8664:raise ValueError('Unexpected PE machine')
        elif data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=(183 if architecture=='aarch64' else 62):raise ValueError('Unexpected ELF machine')
        lifecycle=[run('pause'),run('resume'),run('describe')]
        if [row['state'] for row in lifecycle]!=['paused','running','running']:raise ValueError('VM lifecycle verification failed')
        result={'passed':True,'guest':args.guest,'architecture':architecture,'accelerator':selected_accelerator,'job':job['job'],'uuid':vm['uuid'],
                'seconds':round(time.monotonic()-began,3),'recorded_at':datetime.now(timezone.utc).isoformat(),
                'artifacts':artifacts,'native_app_executed':True,'steps':len(exits),'instance':vm,'lifecycle':lifecycle}
    finally:
        for state in files.values():state['stream'].close()
        (output/'build.log').write_bytes(log)
        (output/'events.jsonl').write_text(''.join(json.dumps(e)+'\n' for e in events))
        (output/'stopped.json').write_text(json.dumps(run('stop','--force'),indent=2)+'\n')
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
