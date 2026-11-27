"""Python adapter for the iiVirtualizer CLI and persistent VM operations."""

import json
import os
from pathlib import Path
import subprocess


class VirtualizerError(RuntimeError):
    """Expose the CLI exit code and diagnostic without hiding a failed action."""

    def __init__(self, operation, returncode, diagnostic):
        self.operation = operation
        self.returncode = returncode
        self.diagnostic = diagnostic
        super().__init__(f'{operation} failed ({returncode}): {diagnostic}')


class Virtualizer:
    """Control instances through an injected CLI executable and storage root.

    The adapter does not interpret shell commands. All VM ownership, state,
    resource and backend checks remain the responsibility of iiVirtualizer.
    """

    def __init__(self, executable, root, *, qemu_x86_64=None, qemu_aarch64=None,
                 tcg_cache_mib=0):
        """Select the CLI, VM root, QEMU executables and optional TCG cache cap."""
        if isinstance(tcg_cache_mib, bool) or not isinstance(tcg_cache_mib, int) or not 0 <= tcg_cache_mib <= 1024:
            raise ValueError('TCG cache must be 0..1024 MiB')
        self.executable = os.fspath(executable)
        self.root = Path(root)
        self.qemu_x86_64 = os.fspath(qemu_x86_64) if qemu_x86_64 is not None else None
        self.qemu_aarch64 = os.fspath(qemu_aarch64) if qemu_aarch64 is not None else None
        self.tcg_cache_mib = tcg_cache_mib

    def _call(self, operation, instance_id, *arguments):
        """Execute one CLI operation as argv and return its JSON result."""
        command = [self.executable, operation, instance_id, '--root', str(self.root)]
        if self.qemu_x86_64 is not None:
            command += ['--qemu-x86_64', self.qemu_x86_64]
        if self.qemu_aarch64 is not None:
            command += ['--qemu-aarch64', self.qemu_aarch64]
        if self.tcg_cache_mib:
            command += ['--tcg-cache-mib', str(self.tcg_cache_mib)]
        command += [str(value) for value in arguments]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode:
            raise VirtualizerError(operation, result.returncode, result.stderr.strip())
        try:
            return json.loads(result.stdout)
        except ValueError as error:
            raise VirtualizerError(operation, result.returncode, 'CLI returned invalid JSON') from error

    def create_windows(self, instance_id, boot_iso, payload_iso, *, cpus=2,
                       memory_mib=4096, disk_mib=49152, accelerator='auto', media_copy_rate=0,
                       direct_io=False):
        """Create a stopped x86-64 disk-first Windows build VM with private media."""
        return self._call('create', instance_id, '--arch', 'x86_64', '--accel', accelerator,
                          '--boot', 'disk', '--boot-iso', os.fspath(boot_iso),
                          '--payload-iso', os.fspath(payload_iso), '--disk-bus', 'sata',
                          '--guest-channel', 'serial', '--graphics', 'vga', '--allow-reboot',
                          '--boot-disk-first', '--cpus', cpus, '--memory', memory_mib,
                          '--disk', disk_mib, '--media-copy-rate', media_copy_rate,
                          *(['--direct-io'] if direct_io else []))

    def describe(self, instance_id):
        """Read the VM's durable identity and observed backend state."""
        return self._call('describe', instance_id)

    def plan(self, instance_id):
        """Return the QEMU argument vector without starting the instance."""
        return self._call('plan', instance_id)

    def replace_payload(self, instance_id, payload_iso):
        """Replace the existing job medium on a confirmed stopped instance."""
        return self._call('set-payload', instance_id, '--payload-iso', os.fspath(payload_iso))

    def start(self, instance_id):
        """Start the owned VM and report its observed state."""
        return self._call('start', instance_id)

    def pause(self, instance_id):
        """Pause a running owned VM."""
        return self._call('pause', instance_id)

    def resume(self, instance_id):
        """Resume a paused owned VM."""
        return self._call('resume', instance_id)

    def stop(self, instance_id, *, force=False, timeout_seconds=10):
        """Request shutdown, using force only when the caller explicitly asks."""
        arguments = ['--timeout', str(timeout_seconds)]
        if force:
            arguments.append('--force')
        return self._call('stop', instance_id, *arguments)

    def remove(self, instance_id):
        """Remove an owned instance only after the runtime confirms it stopped."""
        return self._call('remove', instance_id)
