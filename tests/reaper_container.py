#!/usr/bin/env python3
"""Check that an isolated Linux container reaps an orphaned child process."""

import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from test_isolation import require_test_container


def main():
    require_test_container('tcg')
    read_end, write_end = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(read_end)
        orphan = os.fork()
        if orphan == 0:
            os.close(write_end)
            time.sleep(0.2)
            os._exit(0)
        os.write(write_end, str(orphan).encode('ascii'))
        os.close(write_end)
        os._exit(0)
    os.close(write_end)
    orphan = int(os.read(read_end, 32))
    os.close(read_end)
    os.waitpid(child, 0)
    deadline = time.monotonic() + 15
    while Path('/proc', str(orphan)).exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if Path('/proc', str(orphan)).exists():
        raise RuntimeError('Container init did not reap its orphaned test process')
    print('IIV_CONTAINER_ORPHAN_REAPED')


if __name__ == '__main__':
    main()
