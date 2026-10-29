#!/usr/bin/env python3
"""Prove that one C++ Runtime controls two live Linux guests concurrently."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import time
import urllib.request
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from test_isolation import require_test_container

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accel',choices=['hvf','kvm','tcg'],required=True)
    parser.add_argument('--qemu',type=Path,required=True)
    parser.add_argument('--image',type=Path,default=ROOT/'build/images/aarch64')
    parser.add_argument('--controller',type=Path,default=ROOT/'build/iiVirtualizer_multi_boot')
    args=parser.parse_args()
    require_test_container(args.accel)
    prefix=args.accel+'-'+uuid.uuid4().hex[:6]
    state=ROOT/'build/mv'/prefix
    evidence=ROOT/'build/multi-verification'/prefix
    evidence.mkdir(parents=True)
    probes=[socket.socket(),socket.socket()]
    for probe in probes:probe.bind(('127.0.0.1',0))
    ports=[probe.getsockname()[1] for probe in probes]
    for probe in probes:probe.close()
    command=[str(args.controller.resolve()),str(state),str(args.qemu.resolve()),
             str(args.image/'Image'),str(args.image/'initramfs.cpio.gz'),args.accel,prefix,*map(str,ports)]
    events=[]
    result={'passed':False,'accelerator':args.accel,'state':str(state),'ports':ports,'events':events}
    process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    selector=selectors.DefaultSelector();selector.register(process.stdout,selectors.EVENT_READ)
    pending=bytearray()
    def receive(expected):
        deadline=time.monotonic()+65
        while b'\n' not in pending:
            remaining=deadline-time.monotonic()
            if remaining<=0 or not selector.select(remaining):raise TimeoutError('Controller response timed out')
            data=os.read(process.stdout.fileno(),65536)
            if not data:
                raise RuntimeError('Controller exited: '+process.stderr.read().decode())
            pending.extend(data)
        line,_,tail=pending.partition(b'\n');pending[:]=tail
        event=json.loads(line);events.append(event)
        assert event['event']==expected,event
        assert event['runtime_instances']==1 and event['controller_pid']==process.pid
        assert event['workers']==4
        print(expected,json.dumps({row['instance']['id']:row['instance']['state'] for row in event['instances']}),flush=True)
        return {row['instance']['id']:row for row in event['instances']}
    def send(command,expected):
        process.stdin.write((command+'\n').encode());process.stdin.flush();return receive(expected)
    def http(index,boot):
        body=urllib.request.urlopen(f'http://127.0.0.1:{ports[index]}/',timeout=5).read().decode()
        assert f'boot={boot}\n' in body,body
        return body
    a,b,c=prefix+'-a',prefix+'-b',prefix+'-c'
    try:
        ready=receive('ready')
        assert len(ready)==2 and all(row['instance']['state']=='running' for row in ready.values())
        assert ready[a]['qemu_pid']>1 and ready[b]['qemu_pid']>1
        assert ready[a]['qemu_pid']!=ready[b]['qemu_pid'] and process.pid not in [ready[a]['qemu_pid'],ready[b]['qemu_pid']]
        assert ready[a]['instance']['uuid']!=ready[b]['instance']['uuid']
        assert ready[a]['instance']['console']!=ready[b]['instance']['console']
        assert 'cpus=1 ' in ready[a]['guest'] and 'cpus=2 ' in ready[b]['guest']
        assert ready[a]['instance']['memory_mib']==256 and ready[b]['instance']['memory_mib']==384
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            result['http_initial']=list(pool.map(lambda i:http(i,1),range(2)))
        paused=send('pause-first','paused')
        assert paused[a]['instance']['state']=='paused' and paused[b]['instance']['state']=='running'
        result['http_while_a_paused']=http(1,1)
        resumed=send('resume-first','resumed');assert resumed[a]['instance']['state']=='running'
        stopped=send('stop-first','first-stopped')
        assert stopped[a]['instance']['state']=='stopped' and stopped[b]['instance']['state']=='running'
        result['http_after_a_stopped']=http(1,1)
        recovered=send('restart-with-failure','partial-failure')
        assert recovered[a]['instance']['state']=='running' and recovered[b]['instance']['state']=='running'
        assert recovered[c]['instance']['state']=='stopped'
        batch=events[-1]['batch'];assert not batch[0]['ok'] and batch[1]['ok']
        assert 'boot=2 ' in recovered[a]['guest'] and 'boot=1 ' in recovered[b]['guest']
        assert ready[a]['instance']['uuid']==recovered[a]['instance']['uuid']
        assert ready[b]['qemu_pid']==recovered[b]['qemu_pid']
        result['http_after_partial_failure']=[http(0,2),http(1,1)]
        ended=send('quit','stopped');assert all(row['instance']['state']=='stopped' for row in ended.values())
        assert process.wait(timeout=15)==0
        result.update({'passed':True,'one_runtime':True,'simultaneous_vms':2,
                       'separate_qemu_processes':True,'independent_cpu_memory':True,
                       'independent_pause_stop':True,'independent_disk_persistence':True,'partial_failure_isolated':True})
    except Exception as error:
        result['error']=str(error);raise
    finally:
        if process.poll() is None:
            try:process.stdin.write(b'quit\n');process.stdin.flush();process.wait(timeout=20)
            except Exception:process.terminate();process.wait(timeout=10)
        selector.close()
        result['controller_exit']=process.returncode
        result['stderr']=process.stderr.read().decode()
        cleanup=[]
        for identity in [a,b,c]:
            directory=state/identity
            if not directory.exists():continue
            common=['--root',str(state),'--qemu-aarch64',str(args.qemu.resolve())]
            stopped=subprocess.run([str(ROOT/'build/iivirtualizer'),'stop',identity,'--force',*common],capture_output=True,text=True,timeout=20)
            cleanup.append({'id':identity,'stop_exit':stopped.returncode,'stop_error':stopped.stderr})
            console=directory/'console.log'
            if console.exists():(evidence/(identity+'.console.log')).write_bytes(console.read_bytes())
            if result['passed'] and stopped.returncode==0:
                removed=subprocess.run([str(ROOT/'build/iivirtualizer'),'remove',identity,*common],capture_output=True,text=True,timeout=20)
                cleanup[-1]['remove_exit']=removed.returncode
        result['cleanup']=cleanup
        (evidence/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print('Evidence:',evidence,flush=True)

if __name__=='__main__':main()
