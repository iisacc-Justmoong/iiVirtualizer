#!/usr/bin/env python3
"""Opt-in real VM verification; requires a built guest image and QEMU."""
import argparse
import json
from pathlib import Path
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
    parser.add_argument('--network',action='store_true')
    args=parser.parse_args()
    require_test_container(args.accel)
    identity='s-'+args.accel+'-'+uuid.uuid4().hex[:6]
    state=ROOT/'build/vms'
    evidence=ROOT/'build/verification'/identity
    evidence.mkdir(parents=True)
    log=[]
    cli=ROOT/'build/iivirtualizer'
    common=['--root',str(state),'--qemu-aarch64',str(args.qemu.resolve())]
    def run(command,*values,timeout=45):
        before=time.monotonic()
        process=subprocess.run([str(cli),command,*map(str,values),*common],capture_output=True,text=True,timeout=timeout)
        record={'command':command,'arguments':list(map(str,values)),'exit_code':process.returncode,
                'seconds':round(time.monotonic()-before,3),'stdout':process.stdout,'stderr':process.stderr}
        log.append(record)
        print(command,process.returncode,process.stdout.strip()[:300],process.stderr.strip(),flush=True)
        if process.returncode:raise RuntimeError(command+': '+process.stderr)
        return json.loads(process.stdout) if process.stdout.strip() else None
    port=None
    if args.network:
        with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    created=False
    result={'accelerator':args.accel,'architecture':'aarch64','id':identity,'hardware_virtualization':args.accel!='tcg','passed':False}
    try:
        options=['--kernel',args.image/'Image','--initramfs',args.image/'initramfs.cpio.gz',
                 '--arch','aarch64','--accel',args.accel,'--cpus','2','--memory','384','--disk','64']
        if args.network:options+=['--network','--forward',f'{port}:8080']
        run('create',identity,*options);created=True
        assert run('start',identity)['state']=='running'
        first=run('wait-ready',identity,'--timeout','45',timeout=55)['response']
        for required in ['runtime=iiSDKCore','pid=1','workers=2','tasks=4','volume=mounted','boot=1','cpus=2']:
            assert required in first,(required,first)
        if args.network:
            body=urllib.request.urlopen(f'http://127.0.0.1:{port}/',timeout=5).read().decode()
            assert 'iiOS workload via iiSDKCore; boot=1' in body
            result['http_first']=body
        assert run('pause',identity)['state']=='paused'
        assert run('describe',identity)['state']=='paused'
        assert run('resume',identity)['state']=='running'
        assert run('stop',identity,'--timeout','15')['state']=='stopped'
        assert run('start',identity)['state']=='running'
        second=run('wait-ready',identity,'--timeout','45',timeout=55)['response']
        assert 'volume=mounted' in second and 'boot=2' in second,second
        if args.network:
            body=urllib.request.urlopen(f'http://127.0.0.1:{port}/',timeout=5).read().decode()
            assert 'boot=2' in body
            result['http_second']=body
        assert run('stop',identity,'--force')['state']=='stopped'
        result.update({'passed':True,'first_guest':first,'second_guest':second,'persistent_volume':True,
                       'pause_resume':True,'graceful_shutdown':True,'force_stop':True,'network_verified':args.network})
    except Exception as error:
        result['error']=str(error)
        raise
    finally:
        if created:
            try:run('stop',identity,'--force')
            except Exception as error:result['cleanup_error']=str(error)
            console=state/identity/'console.log'
            if console.exists():(evidence/'console.log').write_bytes(console.read_bytes())
            if result['passed']:run('remove',identity)
        result['commands']=log
        (evidence/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print('Evidence:',evidence,flush=True)

if __name__=='__main__':main()
