#!/usr/bin/env python3
"""Install the native build bootstrap into a private Debian image snapshot."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from prepare_os_guests import ROOT,debugfs,digest,generated_path,quote

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--payload',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--debugfs',type=Path,default=ROOT/'build/dependencies/e2fsprogs-1.47.2/build/debugfs/debugfs')
    args=parser.parse_args();output=generated_path(args.output)
    if output.exists():raise ValueError('Use a fresh build image output')
    recipe=json.loads(args.image.read_text())
    for key,value in recipe['artifact_sha256'].items():
        if digest(recipe[key])!=value:raise ValueError('Base image changed: '+key)
    output.mkdir(parents=True);disk=output/'root.raw'
    clone=subprocess.run(['cp','-c',recipe['base_disk'],str(disk)],capture_output=True)
    if clone.returncode:shutil.copyfile(recipe['base_disk'],disk)
    tool=args.debugfs.resolve()
    def command(text):return debugfs(tool,disk,text,True)
    for folder in ['/opt/iiv','/opt/iiv/work']:
        command('mkdir '+folder)
    for source,target in [(args.payload/'sources.tar.gz','/opt/iiv/sources.tar.gz'),
                          (args.payload/'linux-job.json','/opt/iiv/job.json'),
                          (ROOT/'example/guest-build/linux-start.sh','/usr/local/sbin/iiv-build-start')]:
        command('write '+quote(source.resolve())+' '+target)
    unit=output/'iiv-build.service'
    unit.write_text('[Unit]\nDescription=Native iiSDK guest build\nAfter=network-online.target\nWants=network-online.target\n'
                    '[Service]\nType=simple\nExecStart=/bin/sh /usr/local/sbin/iiv-build-start\nTimeoutStartSec=infinity\n'
                    '[Install]\nWantedBy=multi-user.target\n')
    network=output/'10-iiv.network';network.write_text('[Match]\nName=en* eth*\n[Network]\nDHCP=ipv4\nIPv6AcceptRA=no\n')
    command('write '+quote(unit)+' /etc/systemd/system/iiv-build.service')
    command('write '+quote(network)+' /etc/systemd/network/10-iiv.network')
    command('symlink /etc/systemd/system/multi-user.target.wants/iiv-build.service /etc/systemd/system/iiv-build.service')
    recipe['base_disk']=str(disk);recipe['artifact_sha256']['base_disk']=digest(disk)
    recipe['build_job']=json.loads((args.payload/'linux-job.json').read_text())['job']
    (output/'image.json').write_text(json.dumps(recipe,indent=2)+'\n')
    print(json.dumps({'image':str(output/'image.json'),'job':recipe['build_job']},indent=2))

if __name__=='__main__':main()
