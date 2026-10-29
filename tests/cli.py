#!/usr/bin/env python3
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

exe = Path(sys.argv[1]).resolve()
base = Path(sys.argv[2]).resolve() / f'{time.time_ns():x}'[-6:]
base.mkdir(parents=True)
root = base / 'state'
kernel = base / 'kernel'
initrd = base / 'initramfs'
kernel.write_bytes(b'fixture')
initrd.write_bytes(b'fixture')

def run(*args, ok=True):
    p = subprocess.run([str(exe), *map(str,args), '--root', str(root)], capture_output=True, text=True, timeout=15)
    if ok and p.returncode != 0:
        raise AssertionError(p.stderr)
    if not ok and p.returncode == 0:
        raise AssertionError('Invalid command succeeded: ' + repr(args))
    return p

def create():
    return run('create', 'demo', '--kernel', kernel, '--initramfs', initrd, ok=False)

for args in [('create', '../escape', '--kernel', kernel, '--initramfs', initrd),
             ('create','invalid','--kernel',kernel,'--initramfs',initrd,'--cpus','0'),
             ('create','invalid','--kernel',kernel,'--initramfs',initrd,'--disk','18446744073709551615'),
             ('create','invalid','--kernel',kernel,'--initramfs',initrd,'--arch','invalid'),
             ('create','invalid','--kernel',kernel,'--initramfs',initrd,'--forward','65536:80'),
             ('create','invalid','--kernel',kernel,'--initramfs',initrd,'--typo','yes'),
             ('describe','--network'),('remove','missing'),('unknown',)]:
    run(*args,ok=False)

# Separate processes race for the same durable instance name.
def race(_):
    return subprocess.run([str(exe),'create','demo','--kernel',str(kernel),'--initramfs',str(initrd),'--root',str(root)],capture_output=True,text=True,timeout=15)
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    results=list(pool.map(race,range(6)))
assert sum(p.returncode==0 for p in results)==1
vm=json.loads(run('describe','demo').stdout)
assert vm['state']=='stopped'
assert len(json.loads(run('list').stdout))==1
batch=json.loads(run('describe-many','demo','missing',ok=False).stdout)
assert len(batch)==2 and batch[0]['ok'] and not batch[1]['ok']
assert batch[0]['id']=='demo' and batch[1]['id']=='missing'
run('start-many','demo','demo',ok=False)
run('describe-many',ok=False)
run('describe-many','demo','--workers','0',ok=False)
run('stop-many','missing','--force','--timeout','1',ok=False)
assert Path(vm['kernel']).read_bytes()==b'fixture'
plan=json.loads(run('plan','demo').stdout)
assert str(root/'demo'/'data.raw') in plan[plan.index('-blockdev')+1]
assert plan[plan.index('-nic')+1]=='none'
# Foreign guests must retain their own PID 1 and command line after reopening.
foreign_args='console=ttyAMA0 root=/dev/vda1 rw cloud-init=disabled'
foreign=json.loads(run('create','debian','--kernel',kernel,'--initramfs',initrd,
    '--kernel-args',foreign_args,'--graphics','virtio-gpu','--arch','aarch64','--accel','tcg',
    '--cpu-model','max,pauth-impdef=on').stdout)
assert foreign['kernel_command_line']==foreign_args
assert foreign['graphics']=='virtio-gpu'
assert foreign['cpu_model']=='max,pauth-impdef=on'
foreign_plan=json.loads(run('plan','debian').stdout)
assert foreign_plan[foreign_plan.index('-append')+1]==foreign_args
assert foreign_plan[foreign_plan.index('-cpu')+1]=='max,pauth-impdef=on'
assert 'ii.format_data=' not in ' '.join(foreign_plan)
assert 'virtio-gpu-pci' in foreign_plan
for args in [('--kernel-args','bad\nargument'),('--kernel-args','x'*4097),
             ('--graphics','arbitrary-device'),('--cpu-model','bad\ncpu'),
             ('--cpu-model','-bad'),('--cpu-model','x'*257)]:
    run('create','invalid','--kernel',kernel,'--initramfs',initrd,*args,ok=False)
# Version-one manifests remain readable with the original iiOS defaults.
legacy=root/'demo'/'instance.json'
legacy_json=json.loads(legacy.read_text())
legacy_json.pop('kernel_command_line',None)
legacy_json.pop('graphics',None)
legacy_json.pop('cpu_model',None)
legacy_json.pop('boot_disk_first',None)
legacy.write_text(json.dumps(legacy_json))
legacy_plan=json.loads(run('plan','demo').stdout)
assert 'rdinit=/init' in legacy_plan[legacy_plan.index('-append')+1]
assert legacy_plan[legacy_plan.index('-cpu')+1]==plan[plan.index('-cpu')+1]
run('remove','debian')

def server(mode):
    path=root/'demo'/'qmp.sock'
    ready=threading.Event()
    errors=[]
    def worker():
        try:
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
                s.bind(str(path));s.listen(1);ready.set()
                connection,_=s.accept()
                with connection:
                    f=connection.makefile('rwb',buffering=0)
                    f.write(b'{"QMP":{"version":{},"capabilities":[]}}\r\n')
                    for n in (1,2,3):
                        command=json.loads(f.readline())
                        assert command['id']==n
                        f.write(b'{"event":"TEST"}\r\n')
                        if mode=='wrong-id':
                            f.write(b'{"return":{},"id":999}\r\n');break
                        data={} if n==1 else {'UUID':vm['uuid'] if mode!='wrong-uuid' else '00000000-0000-0000-0000-000000000000'} if n==2 else {'running':True,'status':'running'}
                        payload=json.dumps({'return':data,'id':n}).encode()+b'\r\n'
                        # Fragmented frames exercise the streaming parser.
                        f.write(payload[:7]);f.write(payload[7:])
                        if mode=='wrong-uuid' and n==2:
                            break
        except Exception as error:
            errors.append(error)
        finally:
            ready.set()
    thread=threading.Thread(target=worker,daemon=True);thread.start();assert ready.wait(5)
    result=run('describe','demo',ok=mode=='valid')
    thread.join(5)
    path.unlink(missing_ok=True)
    assert not thread.is_alive() and not errors,errors
    if mode=='valid':assert json.loads(result.stdout)['state']=='running'
    else:assert 'mismatch' in result.stderr

server('valid');server('wrong-id');server('wrong-uuid')
def quit_without_reply():
    path=root/'demo'/'qmp.sock'
    ready=threading.Event()
    errors=[]
    def worker():
        try:
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
                s.bind(str(path));s.listen(2);ready.set()
                for operation in ['query-status','quit']:
                    c,_=s.accept()
                    with c:
                        f=c.makefile('rwb',buffering=0)
                        f.write(b'{"QMP":{}}\r\n')
                        for n in [1,2,3]:
                            command=json.loads(f.readline())
                            if n==3:
                                assert command['execute']==operation
                                if operation=='quit':break
                            value={} if n==1 else {'UUID':vm['uuid']} if n==2 else {'running':True}
                            f.write(json.dumps({'return':value,'id':n}).encode()+b'\r\n')
        except Exception as error:errors.append(error)
        finally:ready.set()
    thread=threading.Thread(target=worker,daemon=True);thread.start();assert ready.wait(5)
    result=run('stop','demo','--force')
    thread.join(5);path.unlink(missing_ok=True)
    assert not thread.is_alive() and not errors,errors
    assert json.loads(result.stdout)['state']=='stopped'
quit_without_reply()
# Lost QMP must not turn a possibly live owner into a deletable instance.
pidfile=root/'demo'/'qemu.pid'
pidfile.write_text(str(os.getpid()))
run('describe','demo',ok=False)
run('remove','demo',ok=False)
assert (root/'demo'/'data.raw').exists()
pidfile.unlink()
run('remove','demo')
assert kernel.exists() and initrd.exists()
assert not (root/'demo').exists()
print('CLI, process race, fragmented QMP, event and identity contracts passed')
