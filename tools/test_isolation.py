"""Fail closed on accidental host or hardware-accelerated VM test launches."""

import os
from pathlib import Path
import signal
import sys

STOP_SIGNALS = tuple(getattr(signal, name) for name in ("SIGINT", "SIGTERM", "SIGHUP")
                     if hasattr(signal, name))


class TestInterrupted(Exception):
    """A requested stop which I/O retry handlers must not swallow."""


def validate_test_isolation(accelerator, platform_name, marker, has_kvm):
    if platform_name != "linux" or marker != "1":
        raise RuntimeError("Real VM tests require the dedicated Linux test container")
    if has_kvm:
        raise RuntimeError("The test container must not expose /dev/kvm")
    if accelerator != "tcg":
        raise RuntimeError("Real VM tests require QEMU TCG")


def require_test_container(accelerator):
    # The marker is an accidental-launch guard, not proof of container isolation.
    validate_test_isolation(accelerator, sys.platform,
                            os.environ.get("IIV_TEST_CONTAINER"), Path("/dev/kvm").exists())


def interrupt_test(signum, _frame):
    """Unwind through the test's VM cleanup when it receives a stop signal."""
    raise TestInterrupted(f"VM test interrupted by signal {signum}")


def install_interrupt_handlers():
    for signum in STOP_SIGNALS:
        signal.signal(signum, interrupt_test)


def protect_cleanup_from_interrupts(on_stop=None):
    """Defer stop signals during cleanup, optionally recording an urgent stop."""
    for signum in STOP_SIGNALS:
        signal.signal(signum, signal.SIG_IGN if on_stop is None else on_stop)
