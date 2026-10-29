#!/usr/bin/env python3
"""Prepare an isolated container for iiVirtualizer guest boot tests."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import uuid


def bounded_int(text, minimum, maximum):
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("Expected an integer") from error
    if not minimum <= value <= maximum:
        raise argparse.ArgumentTypeError(f"Expected a value from {minimum} through {maximum}")
    return value


def container_commands(executable, image, identifier, cpus=2, memory_gib=2, disk_gib=8, backend="apple",
                       io_device=None, io_mib=None):
    """Build argv only; the volume and VM-test container have private names."""
    if not re.fullmatch(r"iiv-test-[0-9a-f]{12}", identifier):
        raise ValueError("Invalid test container identifier")
    if not 1 <= cpus <= 4 or not 1 <= memory_gib <= 12 or not 2 <= disk_gib <= 96:
        raise ValueError("Container limits exceed the allowed test profile")
    if not image or image.startswith("-") or any(ch.isspace() for ch in image):
        raise ValueError("Invalid container image reference")
    if backend not in ("apple", "nerdctl"):
        raise ValueError("Unsupported container backend")
    if io_mib is not None and (io_device is None or not 1 <= io_mib <= 64):
        raise ValueError("An I/O device and a limit of 1 through 64 MiB/s are required")
    if io_device is not None and (backend != "nerdctl" or
                                  not re.fullmatch(r"/dev/[A-Za-z0-9_-][A-Za-z0-9_.-]*", io_device)):
        raise ValueError("I/O limits require a device name in the dedicated nerdctl host")
    volume = identifier + "-work"
    # The nerdctl local volume is bounded by the separately configured Lima
    # host disk, not by a per-volume size option.
    volume_command = [str(executable), "volume", "create"]
    if backend == "apple":
        volume_command += ["-s", f"{disk_gib}G"]
    volume_command.append(volume)
    create_command = [str(executable), "create", "--name", identifier,
                      "--cpus", str(cpus), "--memory", f"{memory_gib}G",
                      "--network", "none", "--cap-drop", "ALL",
                      "--read-only", "--tmpfs", "/tmp",
                      "--mount", f"type=volume,source={volume},target=/work",
                      "--env", "IIV_TEST_CONTAINER=1", image]
    if backend == "apple":
        create_command.insert(create_command.index("--cap-drop"), "--no-dns")
    else:
        create_command[create_command.index("--tmpfs") + 1] = "/tmp:rw,nosuid,nodev,size=256m"
        create_command[create_command.index("--cap-drop"):create_command.index("--cap-drop")] = [
            "--memory-swap", f"{memory_gib}G", "--pids-limit", "256", "--init"]
        if io_device is not None:
            limit = f"{io_device}:{(io_mib or 4) * 1024 * 1024}"
            create_command[create_command.index("--cap-drop"):create_command.index("--cap-drop")] = [
                "--device-read-bps", limit, "--device-write-bps", limit]
    return volume_command, create_command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", type=Path, required=True,
                        help="Absolute path to the chosen container CLI")
    parser.add_argument("--backend", choices=["apple", "nerdctl"], default="apple")
    parser.add_argument("--image", default="iivirtualizer-vm-test:debian13")
    parser.add_argument("--cpus", type=lambda s: bounded_int(s, 1, 4), default=2)
    parser.add_argument("--memory-gib", type=lambda s: bounded_int(s, 1, 12), default=2)
    parser.add_argument("--disk-gib", type=lambda s: bounded_int(s, 2, 96), default=8,
                        help="Apple volume size; nerdctl uses the Lima host disk limit")
    parser.add_argument("--io-device", help="Optional block device in the dedicated nerdctl host")
    parser.add_argument("--io-mib", type=lambda s: bounded_int(s, 1, 64),
                        help="Read and write cap in MiB/s, default 4 when --io-device is set")
    parser.add_argument("--plan", action="store_true", help="Print commands without creating resources")
    args = parser.parse_args()
    if not args.container.is_absolute():
        parser.error("--container must be an absolute path")
    identifier = "iiv-test-" + uuid.uuid4().hex[:12]
    volume_command, create_command = container_commands(
        args.container, args.image, identifier, args.cpus, args.memory_gib, args.disk_gib, args.backend,
        args.io_device, args.io_mib)
    result = {"backend": args.backend, "container": identifier, "volume": identifier + "-work",
              "volume_command": volume_command, "create_command": create_command,
              "created": False}
    # Print ownership information before creating anything, even if a command
    # later fails or the caller interrupts it. Never remove a partial volume.
    print(json.dumps(result, indent=2), flush=True)
    if args.plan:
        return
    subprocess.run(volume_command, check=True)
    subprocess.run(create_command, check=True)
    result["created"] = True
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
