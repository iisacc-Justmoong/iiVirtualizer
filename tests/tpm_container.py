#!/usr/bin/env python3
"""Start and stop the real TPM emulator in the isolated test container."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from test_isolation import require_test_container
from windows_arm_boot import tpm_command


def main():
    if sys.platform != 'linux' or os.environ.get('IIV_TEST_CONTAINER') != '1':
        print('TPM runtime check requires the isolated test container')
        return 77
    require_test_container('tcg')
    if sorted(os.listdir('/sys/class/net')) != ['lo']:
        raise RuntimeError('The TPM check requires a network-isolated test container')
    executable = Path('/usr/bin/swtpm')
    qemu = Path('/usr/bin/qemu-system-aarch64')
    if not executable.is_file() or not qemu.is_file():
        print('TPM emulator or ARM QEMU is not installed')
        return 77
    root = Path(tempfile.mkdtemp(prefix='tpm-check-', dir=ROOT / 'resources'))
    proc = None
    try:
        state = root / 'state'
        state.mkdir(mode=0o700)
        control = root / 'tpm.sock'
        proc = subprocess.Popen(tpm_command(executable, state, control),
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        until = time.monotonic() + 30
        while not control.exists() and proc.poll() is None and time.monotonic() < until:
            time.sleep(0.1)
        if not control.exists() or proc.poll() is not None:
            raise RuntimeError('TPM control socket did not start')
        commands = ''.join(json.dumps(command) + '\n' for command in [
            {'execute': 'qmp_capabilities', 'id': 'cap'},
            {'execute': 'query-status', 'id': 'status'},
            {'execute': 'quit', 'id': 'quit'},
        ])
        result = subprocess.run([str(qemu), '-machine', 'virt,gic-version=3', '-accel', 'tcg',
                                 '-cpu', 'max,pauth-impdef=on', '-m', '64', '-smp', '1',
                                 '-nodefaults', '-display', 'none', '-monitor', 'none', '-nic', 'none',
                                 '-S', '-qmp', 'stdio', '-chardev', 'socket,id=tpm,path=' + str(control),
                                 '-tpmdev', 'emulator,id=tpm0,chardev=tpm', '-device', 'tpm-tis-device,tpmdev=tpm0'],
                                input=commands, capture_output=True, text=True, timeout=300)
        if result.returncode:
            raise RuntimeError('QEMU CPU/TPM configuration failed: ' + result.stderr)
        messages = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        replies = {message.get('id'): message for message in messages if 'id' in message}
        if (not messages or 'QMP' not in messages[0] or
                any('return' not in replies.get(key, {}) for key in ('cap', 'status', 'quit')) or
                replies['status']['return'].get('running') is not False):
            raise RuntimeError('QEMU did not confirm the paused CPU/TPM configuration')
        print('ARM TCG CPU model and TPM connection started and stopped inside the isolated container')
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError('TPM process did not exit; preserved its private state at ' + str(root)) from error
        if proc is None or proc.poll() is not None:
            if proc is not None and proc.stdout is not None:
                log = proc.stdout.read().decode(errors='replace')
                if log:
                    print(log, file=sys.stderr)
            shutil.rmtree(root)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
