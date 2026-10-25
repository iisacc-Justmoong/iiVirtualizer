#!/usr/bin/env python3
"""Build the optional external QEMU engine entirely beneath build/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)

def fetch(spec, base):
    archive = base / Path(spec['url']).name
    if not archive.exists():
        temporary = archive.with_suffix(archive.suffix + '.partial')
        with urllib.request.urlopen(spec['url'], timeout=120) as response, temporary.open('wb') as out:
            shutil.copyfileobj(response, out)
        temporary.replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != spec['sha256']:
        raise RuntimeError(f'Checksum mismatch: {archive}: {digest}')
    source = base / spec['directory']
    if not source.exists():
        with tarfile.open(archive) as tar:
            if sys.version_info >= (3, 12):
                tar.extractall(base, filter='data')
            else:
                for entry in tar.getmembers():
                    target = (base / entry.name).resolve()
                    if base.resolve() not in target.parents:
                        raise RuntimeError('Archive path escapes dependency directory')
                tar.extractall(base)
    return source

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--without-network', action='store_true')
    args = parser.parse_args()
    base = ROOT / 'build' / 'dependencies'
    base.mkdir(parents=True, exist_ok=True)
    specs = json.loads((ROOT / 'tools' / 'sources.json').read_text())
    qemu = fetch(specs['qemu'], base)
    venv = base / 'python'
    if not venv.exists():
        run([sys.executable, '-m', 'venv', venv])
    python = venv / 'bin' / 'python3'
    meson = venv / 'bin' / 'meson'
    env = dict(os.environ)
    env['PIP_CACHE_DIR'] = str(base / 'pip-cache')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    if not (venv / '.iiv-ready').exists():
        run([python, '-m', 'pip', 'install', 'meson==1.9.1', 'packaging==25.0', 'tomli==2.2.1', 'pycotap==1.3.1'], env=env)
        (venv / '.iiv-ready').touch()
    env['PATH'] = str(venv / 'bin') + os.pathsep + env['PATH']
    prefix = base / 'install'
    if not args.without_network:
        slirp = fetch(specs['libslirp'], base)
        slirp_build = slirp / 'build'
        if not (slirp_build / 'build.ninja').exists():
            run([meson, 'setup', slirp_build, slirp, '--prefix', prefix, '--libdir', 'lib',
                 '-Ddefault_library=static'], env=env)
        run(['ninja', '-C', slirp_build, '-j', args.jobs, 'install'], env=env)
    env['PKG_CONFIG_PATH'] = str(prefix / 'lib' / 'pkgconfig') + os.pathsep + env.get('PKG_CONFIG_PATH', '')
    qemu_build = qemu / 'build'
    qemu_build.mkdir(exist_ok=True)
    if not (qemu_build / 'build.ninja').exists():
        flags = ['--without-default-features', '--target-list=aarch64-softmmu,x86_64-softmmu',
                 '--enable-tcg', '--enable-fdt=internal', '--disable-docs', '--disable-werror',
                 '--prefix=' + str(prefix), '--python=' + str(python),
                 '--enable-slirp' if not args.without_network else '--disable-slirp']
        if platform.system() == 'Darwin':
            flags.append('--enable-hvf')
        if platform.system() == 'Linux':
            flags.append('--enable-kvm')
        run([qemu / 'configure', *flags], cwd=qemu_build, env=env)
    run(['ninja', '-C', qemu_build, '-j', args.jobs, 'qemu-system-aarch64', 'qemu-system-x86_64'], env=env)
    print(json.dumps({'qemu_aarch64': str(qemu_build / 'qemu-system-aarch64'),
                      'qemu_x86_64': str(qemu_build / 'qemu-system-x86_64')}, indent=2))

if __name__ == '__main__':
    main()
