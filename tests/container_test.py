#!/usr/bin/env python3
"""Check VM-test container ownership and isolation without starting a VM."""

import contextlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
sys.path.insert(0, str(ROOT / "tools"))
import guest_test_container as subject
from test_isolation import (STOP_SIGNALS, TestInterrupted, install_interrupt_handlers, interrupt_test, protect_cleanup_from_interrupts,
                            validate_test_isolation)


class ContainerPolicyTests(unittest.TestCase):
    def test_stop_signals_unwind_to_cleanup_and_are_deferred_during_cleanup(self):
        with self.assertRaises(TestInterrupted):
            interrupt_test(signal.SIGTERM, None)
        with patch("test_isolation.signal.signal") as register:
            install_interrupt_handlers()
            self.assertEqual(register.call_count, len(STOP_SIGNALS))
            self.assertTrue(all(call.args[1] is interrupt_test for call in register.call_args_list))
            register.reset_mock()
            protect_cleanup_from_interrupts()
            self.assertEqual(register.call_count, len(STOP_SIGNALS))
            self.assertTrue(all(call.args[1] == signal.SIG_IGN for call in register.call_args_list))

    def test_private_bounded_container_has_no_host_mount_or_acceleration(self):
        volume, create = subject.container_commands(
            "/local/container", "iivirtualizer-vm-test:debian13", "iiv-test-0123456789ab")
        self.assertEqual(volume[1:5], ["volume", "create", "-s", "8G"])
        self.assertEqual(create[create.index("--network") + 1], "none")
        self.assertEqual(create[create.index("--cap-drop") + 1], "ALL")
        self.assertEqual(create[create.index("--cpus") + 1], "2")
        self.assertEqual(create[create.index("--memory") + 1], "2G")
        self.assertIn("--read-only", create)
        self.assertIn("IIV_TEST_CONTAINER=1", create)
        self.assertEqual(create[create.index("--mount") + 1],
                         "type=volume,source=iiv-test-0123456789ab-work,target=/work")
        for forbidden in ("--virtualization", "--privileged", "--publish", "--ssh", "--volume"):
            self.assertNotIn(forbidden, create)

    def test_unsafe_limits_names_and_image_references_are_rejected(self):
        for keyword in ({"cpus": 0}, {"cpus": 5}, {"memory_gib": 13}, {"disk_gib": 97}):
            with self.assertRaises(ValueError):
                subject.container_commands("/local/container", "image:tag", "iiv-test-0123456789ab", **keyword)
        for identifier in ("existing-volume", "iiv-test-../x", "iiv-test-xyz"):
            with self.assertRaises(ValueError):
                subject.container_commands("/local/container", "image:tag", identifier)
        with self.assertRaises(ValueError):
            subject.container_commands("/local/container", "--privileged", "iiv-test-0123456789ab")

    def test_nerdctl_limits_use_the_private_lima_disk(self):
        volume, create = subject.container_commands(
            "/local/nerdctl", "image:tag", "iiv-test-0123456789ab", backend="nerdctl")
        self.assertEqual(volume[1:], ["volume", "create", "iiv-test-0123456789ab-work"])
        self.assertEqual(create[create.index("--memory-swap") + 1], "2G")
        self.assertEqual(create[create.index("--pids-limit") + 1], "256")
        self.assertEqual(create[create.index("--network") + 1], "none")
        self.assertIn("--init", create)
        self.assertNotIn("--no-dns", create)
        self.assertIn("/tmp:rw,nosuid,nodev,size=256m", create)
        self.assertNotIn("--privileged", create)

    def test_optional_io_limits_do_not_attach_the_host_disk(self):
        _, create = subject.container_commands(
            "/local/nerdctl", "image:tag", "iiv-test-0123456789ab", backend="nerdctl",
            io_device="/dev/vda", io_mib=4)
        self.assertEqual(create[create.index("--device-read-bps") + 1], "/dev/vda:4194304")
        self.assertEqual(create[create.index("--device-write-bps") + 1], "/dev/vda:4194304")
        self.assertNotIn("--device", create)
        for device in ("vda", "/dev/../etc/passwd", "/dev/vda:2", "/tmp/vda"):
            with self.assertRaises(ValueError):
                subject.container_commands("/local/nerdctl", "image:tag", "iiv-test-0123456789ab",
                                           backend="nerdctl", io_device=device, io_mib=4)
        with self.assertRaises(ValueError):
            subject.container_commands("/local/nerdctl", "image:tag", "iiv-test-0123456789ab",
                                       backend="nerdctl", io_mib=4)
        with self.assertRaises(ValueError):
            subject.container_commands("/local/container", "image:tag", "iiv-test-0123456789ab",
                                       backend="apple", io_device="/dev/vda")

    def test_real_vm_tests_reject_host_and_hardware_acceleration(self):
        validate_test_isolation("tcg", "linux", "1", False)
        for values in (("tcg", "darwin", "1", False), ("tcg", "linux", None, False),
                       ("tcg", "linux", "1", True), ("kvm", "linux", "1", False),
                       ("auto", "linux", "1", False)):
            with self.assertRaises(RuntimeError):
                validate_test_isolation(*values)

    def test_real_boot_entrypoints_refuse_before_reading_guest_inputs(self):
        env = os.environ.copy()
        env.pop("IIV_TEST_CONTAINER", None)
        commands = [
            ("tests/reaper_container.py", []),
            ("tests/os_boot.py", ["--runtime", "/missing", "--image", "/missing", "--root", "/missing",
                                  "--output", "/missing", "--id", "x", "--guest", "debian", "--accel", "tcg", "--qemu", "/missing"]),
            ("tests/boot.py", ["--accel", "tcg", "--qemu", "/missing"]),
            ("tests/sandbox_boot.py", ["--accel", "tcg", "--qemu", "/missing", "--image", "/missing"]),
            ("tests/multi_boot.py", ["--accel", "tcg", "--qemu", "/missing"]),
            ("tests/gateway_ssh.py", ["--gateway", "/missing", "--runtime", "/missing", "--root", "/missing", "--vms", "--accel", "tcg"]),
            ("tools/guest_build.py", ["--guest", "windows", "--qemu", "/missing", "--id", "x", "--job", "/missing", "--output", "/missing"]),
            ("tests/windows_x86_boot.py", ["--runtime", "/missing", "--qemu", "/missing", "--root", "/missing",
                "--output", "/missing", "--id", "x", "--job", "/missing", "--receipt", "/missing", "--source-receipt", "/missing"]),
            ("tests/windows_arm_boot.py", ["--runtime", "/missing", "--installer", "/missing", "--installer-sha256", "0" * 64,
                "--payload-iso", "/missing", "--payload-manifest", "/missing", "--binary", "/missing", "--firmware", "/missing",
                "--firmware-vars", "/missing", "--swtpm", "/missing", "--isoinfo", "/missing", "--root", "/missing", "--output", "/missing",
                "--id", "x", "--qemu", "/missing"]),
        ]
        for filename, arguments in commands:
            with self.subTest(filename=filename):
                command = [sys.executable, "-B", str(ROOT / filename), *arguments]
                completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("dedicated Linux test container", completed.stderr)

    @unittest.skipUnless(CONTROLLER, "A built C++ controller was not provided")
    def test_cpp_multi_boot_controller_refuses_host_execution(self):
        env = os.environ.copy()
        env.pop("IIV_TEST_CONTAINER", None)
        command = [str(CONTROLLER), "/missing", "/missing", "/missing", "/missing", "tcg", "x", "1", "2"]
        completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("Real VM tests require", completed.stderr)

    def test_plan_is_read_only_and_creation_reports_ownership_before_failure(self):
        stream = io.StringIO()
        arguments = ["guest_test_container.py", "--container", "/local/container", "--plan"]
        with patch.object(sys, "argv", arguments), patch.object(subject.subprocess, "run") as run:
            with contextlib.redirect_stdout(stream):
                subject.main()
        self.assertFalse(json.loads(stream.getvalue())["created"])
        run.assert_not_called()

        stream = io.StringIO()
        arguments.pop()
        with patch.object(sys, "argv", arguments), patch.object(
                subject.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "container")) as run:
            with contextlib.redirect_stdout(stream), self.assertRaises(subprocess.CalledProcessError):
                subject.main()
        output = json.loads(stream.getvalue())
        self.assertRegex(output["container"], r"^iiv-test-[0-9a-f]{12}$")
        self.assertEqual(output["volume"], output["container"] + "-work")
        self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
