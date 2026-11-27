#!/usr/bin/env python3
"""Contract checks for cross-architecture and installer boot profiles."""
import json
from pathlib import Path
import platform
import subprocess
import sys

exe=Path(sys.argv[1]).resolve()
root=Path(sys.argv[2]).resolve()
root.mkdir(parents=True,exist_ok=True)
if any(root.iterdir()):raise ValueError('Build guest test needs an empty owned workspace')
fixture=root/'fixture';fixture.write_bytes(b'fixture')
def run(command,identifier,*args,ok=True):
    p=subprocess.run([str(exe),command,identifier,'--root',str(root/'state'),*map(str,args)],
                     capture_output=True,text=True,timeout=120)
    assert (p.returncode==0)==ok,p.stderr
    return json.loads(p.stdout) if ok else None
other='x86_64' if platform.machine().lower() in ['arm64','aarch64'] else 'aarch64'
run('create','cross','--arch',other,'--kernel',fixture,'--initramfs',fixture)
plan=run('plan','cross')
assert plan[plan.index('-accel')+1]=='tcg'
# A TCG cache limit is a runtime option, so an existing VM can use a smaller
# translation cache without rewriting its persistent manifest.
cached=run('plan','cross','--tcg-cache-mib','64')
assert cached[cached.index('-accel')+1]=='tcg,tb-size=64'
assert run('plan','cross','--tcg-cache-mib','0')==plan
run('plan','cross','--tcg-cache-mib','1025',ok=False)
native_accel='hvf' if sys.platform=='darwin' else 'kvm'
run('create','explicit','--arch',other,'--accel',native_accel,'--kernel',fixture,'--initramfs',fixture)
run('plan','explicit',ok=False)
native_arch='aarch64' if platform.machine().lower() in ['arm64','aarch64'] else 'x86_64'
run('create','native-accelerated','--arch',native_arch,'--accel',native_accel,'--kernel',fixture,'--initramfs',fixture)
run('plan','native-accelerated','--tcg-cache-mib','64',ok=False)
vm=run('create','windows','--arch','x86_64','--boot','disk','--boot-iso',fixture,
       '--payload-iso',fixture,'--disk-bus','sata','--guest-channel','serial',
       '--graphics','vga','--allow-reboot','--disk','32768','--media-copy-rate','4')
assert not vm['initialize_disk'] and vm['boot_mode']=='disk'
plan=run('plan','windows')
assert run('plan','windows','--startup-timeout','120')==plan
run('plan','windows','--startup-timeout','0',ok=False)
assert run('plan','windows','--control-timeout','120')==plan
run('plan','windows','--control-timeout','0',ok=False)
assert '-kernel' not in plan and '-initrd' not in plan and '-append' not in plan
assert '-no-reboot' not in plan
assert 'ide-hd,drive=data,bus=ide.0' in plan
assert 'ide-cd,drive=bootiso,bus=ide.1' in plan
assert 'ide-cd,drive=payload,bus=ide.2' in plan
assert plan[plan.index('-boot')+1]=='order=c,once=d,menu=off'
assert plan[plan.index('-serial')+1]=='chardev:agent'
assert 'VGA' in plan and 'virtio-serial-pci' not in plan
assert Path(vm['boot_iso']).read_bytes()==b'fixture'
assert Path(vm['payload_iso']).read_bytes()==b'fixture'
replacement=root/'next-payload.iso';replacement.write_bytes(b'next job')
updated=run('set-payload','windows','--payload-iso',replacement)
assert updated['uuid']==vm['uuid'] and updated['state']=='stopped'
assert Path(vm['payload_iso']).read_bytes()==b'next job' and fixture.read_bytes()==b'fixture'
run('set-payload','windows',ok=False)
run('set-payload','windows','--payload-iso',root/'missing.iso',ok=False)
assert Path(vm['payload_iso']).read_bytes()==b'next job'
# Cross the periodic media-copy sync boundary and compare every copied byte.
large=root/'large-boot.iso'
block=bytes(range(256))*4096
with large.open('wb') as stream:
    for _ in range(17):stream.write(block)
large_vm=run('create','large-media','--arch','x86_64','--boot','disk','--boot-iso',large,
             '--disk','16','--media-copy-rate','64')
assert Path(large_vm['boot_iso']).read_bytes()==large.read_bytes()
arm=run('create','windows-arm','--arch','aarch64','--boot','disk','--boot-iso',fixture,
        '--payload-iso',fixture,'--firmware',fixture,'--firmware-vars',fixture,
        '--disk-bus','nvme','--guest-channel','serial','--graphics','ramfb',
        '--allow-reboot','--external-tpm','--disk','65536')
assert arm['disk_bus']=='nvme' and arm['graphics']=='ramfb' and arm['external_tpm'] and not arm['boot_disk_first']
assert Path(arm['firmware_vars']).read_bytes()==b'fixture'
arm_plan=run('plan','windows-arm')
assert any(value.startswith('if=pflash,format=raw,unit=0,readonly=on,file=') for value in arm_plan)
assert any(value.startswith('if=pflash,format=raw,unit=1,file=') for value in arm_plan)
assert any(value.startswith('nvme,drive=data,serial=') for value in arm_plan)
assert 'qemu-xhci,id=usb' in arm_plan
assert 'usb-storage,bus=usb.0,drive=bootiso,bootindex=1' in arm_plan
assert 'usb-storage,bus=usb.0,drive=payload' in arm_plan
assert 'ramfb' in arm_plan and 'ide-cd,drive=bootiso,bus=ide.1' not in arm_plan
assert 'emulator,id=tpm0,chardev=tpm' in arm_plan
assert 'tpm-tis-device,tpmdev=tpm0' in arm_plan
assert any(value.startswith('socket,id=tpm,path=') and value.endswith('/windows-arm/tpm.sock') for value in arm_plan)
assert any(value.startswith('nvme,drive=data,serial=') and value.endswith(',bootindex=2') for value in arm_plan)
arm_disk_first=run('create','windows-arm-disk-first','--arch','aarch64','--boot','disk',
        '--boot-iso',fixture,'--firmware',fixture,'--disk-bus','nvme','--boot-disk-first')
assert arm_disk_first['boot_disk_first'] and run('describe','windows-arm-disk-first')['boot_disk_first']
disk_first_plan=run('plan','windows-arm-disk-first')
assert any(value.startswith('nvme,drive=data,serial=') and value.endswith(',bootindex=1') for value in disk_first_plan)
assert 'usb-storage,bus=usb.0,drive=bootiso,bootindex=2' in disk_first_plan
arm_manifest=Path(arm_disk_first['directory'])/'instance.json'
arm_json=json.loads(arm_manifest.read_text())
arm_manifest.write_text(json.dumps(dict(arm_json,boot_disk_first='yes')))
run('describe','windows-arm-disk-first',ok=False)
arm_manifest.write_text(json.dumps(arm_json))
x86_disk_first=run('create','windows-x86-disk-first','--arch','x86_64','--boot','disk',
        '--boot-iso',fixture,'--boot-disk-first')
assert x86_disk_first['boot_disk_first']
x86_disk_first_plan=run('plan','windows-x86-disk-first')
assert x86_disk_first_plan[x86_disk_first_plan.index('-boot')+1]=='order=cd,menu=off'
# Direct I/O is persistent and applies to every owned x86 disk medium while
# retaining flushes; the default instance keeps the existing cache policy.
direct=run('create','windows-direct','--arch','x86_64','--boot','disk',
           '--boot-iso',fixture,'--payload-iso',fixture,'--boot-disk-first','--direct-io')
assert direct['direct_io'] and run('describe','windows-direct')['direct_io']
direct_plan=run('plan','windows-direct')
nodes=[json.loads(direct_plan[i+1]) for i,v in enumerate(direct_plan) if v=='-blockdev']
assert len(nodes)==3
assert all(n['file']['cache']=={'direct':True,'no-flush':False} for n in nodes)
assert not vm['direct_io']
normal_nodes=[json.loads(plan[i+1]) for i,v in enumerate(plan) if v=='-blockdev']
assert all('cache' not in n['file'] for n in normal_nodes)
# Older persisted instances do not have the new optional field.
old_manifest=Path(vm['directory'])/'instance.json'
old_json=json.loads(old_manifest.read_text())
legacy=dict(old_json);legacy.pop('direct_io')
old_manifest.write_text(json.dumps(legacy))
assert run('describe','windows')['direct_io'] is False
old_manifest.write_text(json.dumps(old_json))
direct_manifest=Path(direct['directory'])/'instance.json'
direct_json=json.loads(direct_manifest.read_text())
direct_manifest.write_text(json.dumps(dict(direct_json,direct_io='yes')))
run('describe','windows-direct',ok=False)
direct_manifest.write_text(json.dumps(direct_json))
arm_direct=run('create','arm-direct','--arch','aarch64','--boot','disk',
               '--boot-iso',fixture,'--payload-iso',fixture,'--firmware',fixture,
               '--firmware-vars',fixture,'--direct-io')
assert arm_direct['direct_io']
arm_direct_plan=run('plan','arm-direct')
arm_nodes=[json.loads(arm_direct_plan[i+1]) for i,v in enumerate(arm_direct_plan) if v=='-blockdev']
assert arm_nodes[0]['file']['cache']=={'direct':True,'no-flush':False}
drives=[arm_direct_plan[i+1] for i,v in enumerate(arm_direct_plan) if v=='-drive']
assert len(drives)==4 and all(',cache=none' in item for item in drives)
run('describe','windows-direct','--direct-io',ok=False)
run('create','bad-kernel-priority','--kernel',fixture,'--initramfs',fixture,'--boot-disk-first',ok=False)
run('create','bad-no-media-priority','--boot','disk','--boot-disk-first',ok=False)
run('create','bad-copy-rate','--arch','x86_64','--boot','disk','--boot-iso',fixture,
    '--media-copy-rate','65',ok=False)
assert '-tpmdev' not in plan
cloned=run('create','clone','--kernel',fixture,'--initramfs',fixture,
           '--base-disk',fixture,'--clone-base','--disk','16')
clone_disk=Path(cloned['directory'])/'data.raw'
with clone_disk.open('r+b') as disk:
    assert disk.read(7)==b'fixture'
    disk.seek(0);disk.write(b'changed')
assert fixture.read_bytes()==b'fixture', 'Guest disk writes changed the source image'
run('create','clone-missing','--kernel',fixture,'--initramfs',fixture,'--clone-base',ok=False)
run('create','arm-no-code','--arch','aarch64','--boot','disk','--firmware-vars',fixture,ok=False)
run('create','vars-with-kernel','--kernel',fixture,'--initramfs',fixture,'--firmware-vars',fixture,ok=False)
for opts in [('--boot','bad'),('--guest-channel','bad'),('--disk-bus','bad'),
             ('--boot','disk','--kernel',fixture),('--kernel',fixture,'--initramfs',fixture,'--allow-reboot')]:
    run('create','invalid',*opts,ok=False)
print('Automatic x86/ARM emulation, owned boot media and Windows installer profiles passed')
