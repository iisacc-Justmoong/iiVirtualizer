#!/usr/bin/env python3
"""Prepare private Windows ARM installer and native probe inputs for a test VM."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil
from windows_native_proof import pe_architecture

ROOT = Path(__file__).resolve().parents[1]


def prepare(binary, output, image_index):
    binary = Path(binary).resolve(strict=True)
    output = Path(output)
    resources = (ROOT / 'resources').resolve(strict=True)
    resolved = output.resolve(strict=False)
    if resolved == resources or not resolved.is_relative_to(resources) or output.exists() or output.is_symlink():
        raise ValueError('Choose a fresh payload directory beneath project resources/')
    if not isinstance(image_index, int) or not 1 <= image_index <= 256:
        raise ValueError('Windows image index must be between 1 and 256')
    if pe_architecture(binary) != 'aarch64':
        raise ValueError('The native Windows probe must be an ARM64 PE executable')
    nonce = secrets.token_hex(16)
    password = 'Iiv-' + secrets.token_hex(16)
    resolved.mkdir(mode=0o700, parents=True)
    payload = resolved / 'Payload'
    payload.mkdir(mode=0o700)
    shutil.copyfile(binary, payload / 'windows-native-probe.exe')
    bootstrap = '''@echo off
if not exist C:\\iiv mkdir C:\\iiv
if errorlevel 1 exit /b 1
copy /Y "%~dp0windows-native-probe.exe" C:\\iiv\\windows-native-probe.exe
if errorlevel 1 exit /b 1
exit /b 0
'''
    (payload / 'bootstrap-probe.cmd').write_bytes(bootstrap.replace('\n', '\r\n').encode('ascii'))
    template = (ROOT / 'example/guest-container/Autounattend-arm64.xml.in').read_text()
    answer = template.replace('@IMAGE_INDEX@', str(image_index)).replace('@NONCE@', nonce).replace('@PASSWORD@', password)
    answer_path = resolved / 'Autounattend.xml'
    answer_path.write_text(answer)
    answer_path.chmod(0o600)
    sha = hashlib.sha256()
    with binary.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
    result = {'payload': str(resolved), 'image_index': image_index, 'nonce': nonce,
              'binary_architecture': 'aarch64', 'binary_sha256': sha.hexdigest()}
    (resolved / 'probe-manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image-index', type=int, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.binary, args.output, args.image_index), indent=2))


if __name__ == '__main__':
    main()
