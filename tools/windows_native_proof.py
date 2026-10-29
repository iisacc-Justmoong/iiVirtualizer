#!/usr/bin/env python3
"""Validate a Windows guest's native executable and complete serial report."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET

MACHINES = {0x8664: 'x86_64', 0xaa64: 'aarch64'}
FIELDS = {'os', 'native_arch', 'binary_arch', 'major', 'minor', 'build', 'system_drive', 'nonce'}


def pe_architecture(path):
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError('Missing or truncated PE header')
        offset = struct.unpack_from('<I', header, 60)[0]
        if offset < 64 or offset > path.stat().st_size - 6:
            raise ValueError('Invalid PE header offset')
        stream.seek(offset)
        data = stream.read(6)
        if len(data) != 6 or data[:4] != b'PE\0\0':
            raise ValueError('Missing PE signature')
        machine = struct.unpack_from('<H', data, 4)[0]
        if machine not in MACHINES:
            raise ValueError('Unsupported PE machine')
        return MACHINES[machine]


def parse_report(console, architecture, nonce, environment='installed'):
    if (environment not in ('installed', 'winpe') or architecture not in MACHINES.values() or
            not re.fullmatch(r'[0-9a-f]{32}', nonce)):
        return None
    matches = list(re.finditer(r'^IIV_WINDOWS_PROBE_BEGIN\n(.*?)^IIV_WINDOWS_PROBE_END\s*$',
                               console.replace('\r', ''), re.M | re.S))
    if not matches:
        return None
    lines = matches[-1].group(1).splitlines()
    pairs = [re.fullmatch(r'([a-z_]+)=([^\n]+)', line) for line in lines]
    if any(pair is None for pair in pairs):
        return None
    fields = {pair.group(1): pair.group(2) for pair in pairs}
    if len(fields) != len(pairs) or set(fields) != FIELDS:
        return None
    if (fields['os'] != 'Windows' or fields['native_arch'] != architecture or
            fields['binary_arch'] != architecture or fields['nonce'] != nonce):
        return None
    if not all(re.fullmatch(r'[0-9]+', fields[key]) for key in ('major', 'minor', 'build')):
        return None
    if int(fields['major']) < 10 or int(fields['build']) == 0:
        return None
    if not re.fullmatch('X' if environment == 'winpe' else '[A-WY-Z]', fields['system_drive']):
        return None
    return fields


def validate_payload(binary, answer, manifest):
    """Bind the ISO's native program and answer file to the generated manifest."""
    try:
        nonce = manifest['nonce']
        if (manifest['binary_architecture'] != 'aarch64' or not re.fullmatch(r'[0-9a-f]{32}', nonce) or
                hashlib.sha256(binary).hexdigest() != manifest['binary_sha256']):
            return False
        root = ET.fromstring(answer)
        ns = {'u': 'urn:schemas-microsoft-com:unattend'}
        components = root.findall('.//u:component', ns)
        index = root.find('.//u:OSImage/u:InstallFrom/u:MetaData/u:Value', ns)
        command = root.find('.//u:FirstLogonCommands/u:SynchronousCommand/u:CommandLine', ns)
        bootstrap = root.find(".//u:settings[@pass='specialize']/u:component[@name='Microsoft-Windows-Deployment']/u:RunSynchronous/u:RunSynchronousCommand/u:Path", ns)
        winpe = root.find(".//u:settings[@pass='windowsPE']/u:component[@name='Microsoft-Windows-Setup']/u:RunSynchronous/u:RunSynchronousCommand/u:Path", ns)
        account = root.find('.//u:UserAccounts/u:LocalAccounts/u:LocalAccount', ns)
        logon = root.find('.//u:AutoLogon', ns)
        expected_winpe = ('cmd.exe /c for %d in (D E F G H I J K L) do @if exist '
                          '%d:\\Payload\\windows-native-probe.exe %d:\\Payload\\windows-native-probe.exe '
                          + nonce + ' --winpe & exit /b 0')
        return bool(components and all(node.attrib.get('processorArchitecture') == 'arm64' for node in components) and
                    index is not None and index.text == str(manifest['image_index']) and command is not None and
                    command.text == 'C:\\iiv\\windows-native-probe.exe ' + nonce and bootstrap is not None and
                    'bootstrap-probe.cmd' in (bootstrap.text or '') and winpe is not None and
                    winpe.text == expected_winpe and account is not None and logon is not None and
                    account.findtext('u:Name', namespaces=ns) == 'IIVProbe' and
                    account.findtext('u:Group', namespaces=ns) == 'Administrators' and
                    logon.findtext('u:Username', namespaces=ns) == 'IIVProbe' and
                    logon.findtext('u:Enabled', namespaces=ns) == 'true' and
                    logon.findtext('u:LogonCount', namespaces=ns) == '1' and
                    account.findtext('u:Password/u:PlainText', namespaces=ns) == 'true' and
                    logon.findtext('u:Password/u:PlainText', namespaces=ns) == 'true' and
                    account.findtext('u:Password/u:Value', namespaces=ns) and
                    account.findtext('u:Password/u:Value', namespaces=ns) ==
                    logon.findtext('u:Password/u:Value', namespaces=ns))
    except (KeyError, TypeError, ET.ParseError, ValueError):
        return False

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--console', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--architecture', required=True, choices=['aarch64', 'x86_64'])
    parser.add_argument('--nonce', required=True)
    args = parser.parse_args()
    if pe_architecture(args.binary) != args.architecture:
        parser.error('The executable PE architecture does not match')
    report = parse_report(args.console.read_text(errors='replace'), args.architecture, args.nonce)
    if not report:
        parser.error('No complete matching native Windows execution report')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
