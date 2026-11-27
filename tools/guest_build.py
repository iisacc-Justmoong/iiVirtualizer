#!/usr/bin/env python3
"""Boot an owned build VM, validate native build events, and recover artifacts."""
import argparse
import base64
from datetime import datetime,timezone
import hashlib
import json
from pathlib import Path,PurePosixPath
import re
import socket
import struct
import subprocess
import sys
import time
from prepare_os_guests import generated_path
from test_isolation import (TestInterrupted, install_interrupt_handlers,
                            protect_cleanup_from_interrupts, require_test_container)

ROOT=Path(__file__).resolve().parents[1]

def artifact_path(root,name):
    path=PurePosixPath(name)
    if path.is_absolute() or not path.parts or any(x in ['..','.'] for x in path.parts) or '\\' in name or ':' in name:
        raise ValueError('Unsafe artifact path')
    destination=root.joinpath(*path.parts)
    if root.resolve() not in destination.resolve().parents:raise ValueError('Artifact escaped output')
    return destination

def validate_windows_reuse(vm, *, require_direct_io=False):
    """Reject reused VMs that do not match the isolated Windows build profile."""
    if (vm.get('state')!='stopped' or vm.get('architecture')!='x86_64' or
            vm.get('boot_mode')!='disk' or vm.get('guest_channel')!='serial' or
            vm.get('network') is not False or not vm.get('payload_iso')):
        raise ValueError('Reused Windows VM does not match the isolated build profile')
    if vm.get('boot_iso') and not vm.get('boot_disk_first'):
        raise ValueError('Reused Windows installer VMs must prefer the installed disk')
    if require_direct_io and not vm.get('direct_io'):
        raise ValueError('Reused Windows VM must retain direct I/O')
    # Check the saved VM itself: CLI defaults do not constrain a reused VM.
    check_windows_resources(vm.get('cpus'),vm.get('memory_mib'),vm.get('disk_mib'))

def check_container_memory(guest_mib, meminfo, cgroup_limit):
    """Require headroom in both the Linux VM and any finite cgroup limit."""
    match=re.search(r'^MemTotal:\s+(\d+)\s+kB\s*$',meminfo,re.MULTILINE)
    required=(guest_mib+512)*1024**2
    if not match or int(match.group(1))*1024<required:
        raise ValueError('The container host must leave at least 512 MiB beyond guest memory')
    if cgroup_limit!='max' and (not cgroup_limit.isdecimal() or int(cgroup_limit)<required):
        raise ValueError('The test container must leave at least 512 MiB beyond guest memory')

def check_windows_resources(cpus, memory_mib, disk_mib):
    """Enforce the isolated test range; OS and workloads may require more."""
    if (any(isinstance(value,bool) or not isinstance(value,int) for value in (cpus,memory_mib,disk_mib)) or
            not 1<=cpus<=4 or not 1024<=memory_mib<=8192 or not 32768<=disk_mib<=98304):
        raise ValueError('VM resource limits are outside the test profile')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest',choices=['linux','windows'],required=True)
    parser.add_argument('--runtime',type=Path,default=ROOT/'build/iivirtualizer')
    parser.add_argument('--qemu',type=Path,required=True)
    parser.add_argument('--root',type=Path,default=ROOT/'resources/gb')
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
    parser.add_argument('--cpus',type=int,default=2)
    parser.add_argument('--memory-mib',type=int,default=4096)
    parser.add_argument('--disk-mib',type=int,default=49152)
    parser.add_argument('--media-copy-rate',type=int,default=4)
    parser.add_argument('--direct-io',action='store_true',help='Bypass the QEMU host file cache for VM media')
    parser.add_argument('--tcg-cache-mib',type=int,default=0,help='Limit the TCG translation cache in MiB; 0 uses the QEMU default')
    parser.add_argument('--expect-output',help='Require this text in the guest job output')
    parser.add_argument('--verify-pe',help='Require this declared artifact to be x86-64 PE')
    args=parser.parse_args();require_test_container('tcg')
    install_interrupt_handlers()
    if sorted(Path('/sys/class/net').iterdir(),key=lambda p:p.name)!=[Path('/sys/class/net/lo')]:
        raise RuntimeError('The VM test container must have no network')
    if args.guest=='windows':check_windows_resources(args.cpus,args.memory_mib,args.disk_mib)
    if not 1<=args.media_copy_rate<=16:raise ValueError('Test media copy rate must be 1..16 MiB/s')
    if not 0<=args.tcg_cache_mib<=1024:raise ValueError('TCG cache must be 0..1024 MiB')
    output=generated_path(args.output);root=generated_path(args.root)
    if output.exists():
        raise ValueError('Choose a fresh evidence directory beneath resources/')
    output.mkdir(parents=True);(output/'artifacts').mkdir();job=json.loads(args.job.read_text())
    if args.verify_pe and args.verify_pe not in job['artifacts']:
        raise ValueError('The PE artifact must be declared by the job')
    commands=[]
    architecture='x86_64' if args.guest=='windows' else json.loads(args.image.read_text())['architecture']
    def run(command,*options):
        argv=[str(args.runtime.resolve()),command,args.id,'--root',str(root),
              '--qemu-'+architecture,str(args.qemu.resolve()),'--startup-timeout',str(args.startup_timeout),
              '--control-timeout',str(args.control_timeout),
              *(['--tcg-cache-mib',str(args.tcg_cache_mib)] if args.tcg_cache_mib else []),*map(str,options)]
        done=subprocess.run(argv,capture_output=True,text=True,timeout=7200 if command=='create' else 3600)
        commands.append({'argv':argv,'returncode':done.returncode,'stdout':done.stdout,'stderr':done.stderr})
        (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if done.returncode:raise RuntimeError(done.stderr)
        return json.loads(done.stdout)
    if args.reuse:
        vm=run('describe')
        if vm['state']!='stopped':raise ValueError('Only a stopped VM can be reused')
        if args.guest=='windows':validate_windows_reuse(vm,require_direct_io=args.direct_io)
        elif args.direct_io and not vm.get('direct_io'):
            raise ValueError('Reused VM must retain direct I/O')
    elif args.guest=='windows':
        if not args.boot_iso or not args.payload_iso:
            raise ValueError('A new Windows VM requires installer and payload ISOs')
        vm=run('create','--arch','x86_64','--accel','tcg','--boot','disk','--boot-iso',args.boot_iso.resolve(),
               '--payload-iso',args.payload_iso.resolve(),'--disk-bus','sata','--guest-channel','serial',
               '--graphics','vga','--allow-reboot','--boot-disk-first','--cpus',str(args.cpus),
               '--memory',str(args.memory_mib),'--disk',str(args.disk_mib),
               '--media-copy-rate',str(args.media_copy_rate),*(['--direct-io'] if args.direct_io else []))
    else:
        image=json.loads(args.image.read_text())
        vm=run('create','--arch',architecture,'--accel','tcg','--kernel',image['kernel'],'--initramfs',image['initramfs'],
               '--base-disk',image['base_disk'],'--kernel-args',image['kernel_command_line'],
               '--cpus','3','--memory','3072','--disk','16384','--network',
               *(['--direct-io'] if args.direct_io else []),*(['--clone-base'] if args.clone_base else []))
    memory_limit=Path('/sys/fs/cgroup/memory.max')
    check_container_memory(vm['memory_mib'],Path('/proc/meminfo').read_text(),
                           memory_limit.read_text().strip() if memory_limit.exists() else 'max')
    plan=run('plan');(output/'plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    selected_accelerator=plan[plan.index('-accel')+1]
    expected_accelerator='tcg'+(f',tb-size={args.tcg_cache_mib}' if args.tcg_cache_mib else '')
    if selected_accelerator!=expected_accelerator:raise ValueError('Test VM must select the requested TCG configuration')
    selected_accelerator='tcg'
    if args.guest=='windows' and plan[plan.index('-nic')+1]!='none':
        raise ValueError('Windows test VM must have no network')
    if args.reuse and args.payload_iso:
        vm=run('set-payload','--payload-iso',args.payload_iso.resolve())
    began=time.monotonic();completed=False;started=False;forced=False;exits={};files={};artifacts={};log=bytearray();events=[]
    try:
        vm=run('start')
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
                    elif kind=='log':
                        log.extend(base64.b64decode(event['data'],validate=True))
                        if len(log)>32*1024*1024:raise ValueError('Guest build log exceeds 32 MiB')
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
                        if args.expect_output and args.expect_output not in log.decode(errors='replace'):
                            raise ValueError('Expected guest output was not observed')
                        completed=True
                    if kind!='log' or time.monotonic()-last_progress>5:
                        (output/'progress.json').write_text(json.dumps({'seconds':round(time.monotonic()-began,1),'event':{k:v for k,v in event.items() if k!='data'}},indent=2)+'\n');last_progress=time.monotonic()
                if len(buffer)>65536:raise ValueError('Unbounded build channel line')
        if not completed:raise TimeoutError('Guest native build timed out')
        if args.verify_pe:
            data=artifact_path(output/'artifacts',args.verify_pe).read_bytes()
            if data[:2]!=b'MZ':raise ValueError('Missing Windows PE artifact')
            offset=struct.unpack_from('<I',data,60)[0]
            if data[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',data,offset+4)[0]!=0x8664:raise ValueError('Unexpected PE machine')
        lifecycle=[run('pause'),run('resume'),run('describe')]
        if [row['state'] for row in lifecycle]!=['paused','running','running']:raise ValueError('VM lifecycle verification failed')
        result={'passed':True,'guest':args.guest,'architecture':architecture,'accelerator':selected_accelerator,'job':job['job'],'uuid':vm['uuid'],
                'seconds':round(time.monotonic()-began,3),'recorded_at':datetime.now(timezone.utc).isoformat(),
                'artifacts':artifacts,'expected_output_verified':bool(args.expect_output),
                'verified_pe':args.verify_pe,'steps':len(exits),'instance':vm,'lifecycle':lifecycle}
    finally:
        interrupted=isinstance(sys.exc_info()[1],(TestInterrupted,KeyboardInterrupt))
        protect_cleanup_from_interrupts()
        for state in files.values():state['stream'].close()
        (output/'build.log').write_bytes(log)
        (output/'events.jsonl').write_text(''.join(json.dumps(e)+'\n' for e in events))
        try:
            stopped=run('stop','--timeout','30' if interrupted else '300')
        except RuntimeError:
            forced=True
            stopped=run('stop','--force')
        stopped['forced_stop']=forced
        (output/'stopped.json').write_text(json.dumps(stopped,indent=2)+'\n')
    result['forced_stop']=forced
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
