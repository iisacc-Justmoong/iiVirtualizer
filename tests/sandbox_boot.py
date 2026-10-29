#!/usr/bin/env python3
"""Opt-in Linux kernel enforcement tests in disposable VM instances."""
import argparse
import gzip
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_guest import cpio_bytes, unpack_cpio
from test_isolation import require_test_container


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accel', choices=['hvf', 'kvm', 'tcg'], required=True)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True, help='Image containing ii-guest-sandbox-probe')
    parser.add_argument('--runtime', type=Path, default=ROOT / 'build/iivirtualizer')
    args = parser.parse_args()
    require_test_container(args.accel)
    token = args.accel + '-' + uuid.uuid4().hex[:6]
    evidence = ROOT / 'build/fs-verification' / token
    evidence.mkdir(parents=True)
    state = ROOT / 'build/fsv'
    common = ['--root', str(state), '--qemu-aarch64', str(args.qemu.resolve())]
    records, created = [], []
    report = {'passed': False, 'accelerator': args.accel, 'hardware_virtualization': args.accel != 'tcg', 'filesystem_profile': 'society-v1'}

    def run(command, *values, timeout=60):
        process = subprocess.run([str(args.runtime.resolve()), command, *map(str, values), *common], capture_output=True, text=True, timeout=timeout)
        records.append({'command': command, 'arguments': list(map(str, values)), 'exit': process.returncode, 'stdout': process.stdout, 'stderr': process.stderr})
        if process.returncode:
            raise AssertionError(command + ': ' + process.stderr)
        return json.loads(process.stdout) if process.stdout.strip() else None

    def create(identity, image, extra=()):
        run('create', identity, '--kernel', image / 'Image', '--initramfs', image / 'initramfs.cpio.gz', '--arch', 'aarch64',
            '--accel', args.accel, '--cpus', '2', '--memory', '384', '--disk', '64', *extra)
        created.append(identity)

    def console(identity):
        path = state / identity / 'console.log'
        return path.read_text(errors='replace') if path.exists() else ''

    def wait_text(identity, text, offset=0):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            value = console(identity)[offset:]
            if text in value:
                return value
            time.sleep(0.1)
        raise AssertionError('Guest marker missing: ' + text + '\n' + console(identity)[-4000:])

    def start(identity):
        # QEMU's file chardev truncates this log on each launch. Preserve each
        # boot below, and always inspect the new log from its beginning.
        offset = 0
        assert run('start', identity)['state'] == 'running'
        ready = run('wait-ready', identity, '--timeout', '45')['response']
        assert 'filesystem_profile=society-v1' in ready, ready
        return offset, ready

    try:
        identity = 'fs-' + token
        create(identity, args.image)
        boots = []
        for boot in (1, 2):
            offset, ready = start(identity)
            text = wait_text(identity, 'IIV_SANDBOX_TEST ', offset)
            # Wait for the complete JSON line and optional tamper marker.
            deadline = time.monotonic() + 10
            while not re.search(r'IIV_SANDBOX_TEST (\{[^\n]*\})', text):
                assert time.monotonic() < deadline
                time.sleep(0.1);text = console(identity)[offset:]
            result = json.loads(re.search(r'IIV_SANDBOX_TEST (\{[^\n]*\})', text).group(1))
            assert result['passed'] and result['boot'] == str(boot) and result['checks'] >= 50, result
            assert 'IIVIRTUALIZER_SANDBOX_READY profile=society-v1 uid=1000' in text, text
            boots.append(result)
            if boot == 2:
                wait_text(identity, 'IIV_SANDBOX_TAMPER_TEST completed=1', offset)
            (evidence / (identity + '.boot' + str(boot) + '.console.log')).write_text(console(identity))
            assert run('stop', identity)['state'] == 'stopped'
        offset, ready = start(identity)
        text = wait_text(identity, 'workload blocked:', offset)
        assert 'workspace=unavailable' in ready and 'workload=none' in ready, ready
        assert 'IIV_SANDBOX_TEST ' not in text and 'IIVIRTUALIZER_SANDBOX_READY' not in text, text
        (evidence / (identity + '.blocked.console.log')).write_text(console(identity))
        run('stop', identity)
        report.update({'boots': boots, 'redirected_workspace_fail_closed': True})

        missing = evidence / 'missing-launcher';missing.mkdir()
        shutil.copyfile(args.image / 'Image', missing / 'Image')
        entries = unpack_cpio(gzip.decompress((args.image / 'initramfs.cpio.gz').read_bytes()))
        del entries['sbin/ii-workload']
        (missing / 'initramfs.cpio.gz').write_bytes(gzip.compress(cpio_bytes([(name, *entry) for name, entry in entries.items()]), mtime=0))
        identity = 'no-' + token;create(identity, missing)
        offset, ready = start(identity)
        text = wait_text(identity, 'workload blocked:', offset)
        assert 'workload=none' in ready and 'IIV_SANDBOX_TEST ' not in text, ready
        run('stop', identity);report['missing_launcher_fail_closed'] = True

        invalid = evidence / 'invalid.raw'
        with invalid.open('wb') as volume:
            volume.write(b'invalid filesystem fixture');volume.truncate(64 * 1024 * 1024)
        identity = 'bad-' + token;create(identity, args.image, ['--base-disk', invalid])
        offset, ready = start(identity)
        text = wait_text(identity, 'workload blocked:', offset)
        assert 'volume=unavailable' in ready and 'workspace=unavailable' in ready and 'workload=none' in ready, ready
        assert 'IIV_SANDBOX_TEST ' not in text, text
        run('stop', identity);report['missing_volume_fail_closed'] = True
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        for identity in created:
            try:
                run('stop', identity, '--force')
                (evidence / (identity + '.console.log')).write_text(console(identity))
                run('remove', identity)
            except Exception as error:
                report.setdefault('cleanup_errors', []).append(str(error))
        report['commands'] = records
        (evidence / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': report['passed'], 'evidence': str(evidence / 'result.json')}), flush=True)


if __name__ == '__main__':
    main()
