#!/usr/bin/env python3
"""Contract checks for cross-architecture and installer boot profiles."""
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

exe=Path(sys.argv[1]).resolve()
root=Path(sys.argv[2]).resolve()/str(time.time_ns())[-8:]
root.mkdir(parents=True)
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
native_accel='hvf' if sys.platform=='darwin' else 'kvm'
run('create','explicit','--arch',other,'--accel',native_accel,'--kernel',fixture,'--initramfs',fixture)
run('plan','explicit',ok=False)
vm=run('create','windows','--arch','x86_64','--boot','disk','--boot-iso',fixture,
       '--payload-iso',fixture,'--disk-bus','sata','--guest-channel','serial',
       '--graphics','vga','--allow-reboot','--disk','32768')
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
run('create','bad-kernel-priority','--kernel',fixture,'--initramfs',fixture,'--boot-disk-first',ok=False)
run('create','bad-no-media-priority','--boot','disk','--boot-disk-first',ok=False)
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
