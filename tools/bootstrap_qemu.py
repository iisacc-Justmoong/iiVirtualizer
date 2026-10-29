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
    parser.add_argument('--with-display', action='store_true', help='Enable pixman/VNC diagnostics in a separate engine build')
    parser.add_argument('--with-tpm', action='store_true', help='Enable QEMU TPM device and emulator support')
    parser.add_argument('--output', type=Path, help='Optional build directory beneath this project build/')
    parser.add_argument('--targets', nargs='+', choices=['aarch64','x86_64'], default=['aarch64','x86_64'])
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
    qemu_build = args.output.resolve() if args.output else qemu / 'build'
    if ROOT / 'build' not in qemu_build.parents:
        raise ValueError('QEMU build output must stay beneath this project build/')
    qemu_build.mkdir(parents=True, exist_ok=True)
    if (args.with_display or args.with_tpm) and (qemu_build / 'build.ninja').exists():
        config=(qemu_build/'config-host.h').read_text()
        definitions={line.split()[1] for line in config.splitlines() if line.startswith('#define ')}
        required=({'CONFIG_PIXMAN','CONFIG_VNC'} if args.with_display else set()) | ({'CONFIG_TPM'} if args.with_tpm else set())
        if not required <= definitions:
            raise ValueError('Requested QEMU features require a fresh --output directory')
    if not (qemu_build / 'build.ninja').exists():
        flags = ['--without-default-features', '--target-list='+','.join(target+'-softmmu' for target in args.targets),
                 '--enable-tcg', '--enable-fdt=internal', '--disable-docs', '--disable-werror',
                 '--prefix=' + str(prefix), '--python=' + str(python),
                 '--enable-slirp' if not args.without_network else '--disable-slirp']
        if args.with_display:
            flags.extend(['--enable-pixman','--enable-vnc'])
        if args.with_tpm:
            flags.append('--enable-tpm')
        if platform.system() == 'Darwin':
            flags.append('--enable-hvf')
        if platform.system() == 'Linux':
            flags.append('--enable-kvm')
        run([qemu / 'configure', *flags], cwd=qemu_build, env=env)
    run(['ninja', '-C', qemu_build, '-j', args.jobs, *['qemu-system-'+target for target in args.targets]], env=env)
    print(json.dumps({'qemu_'+target: str(qemu_build / ('qemu-system-'+target)) for target in args.targets}, indent=2))

if __name__ == '__main__':
    main()
