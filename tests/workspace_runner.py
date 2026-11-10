"""Run one contract test in a disposable project resource workspace."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import uuid

_issued = set()


def run(resources, command, *, execute=subprocess.run):
    import ii_file_provider as provider
    resources = Path(resources).resolve()
    # Short paths leave room for native AF_UNIX sockets on macOS.
    state = resources / ".t"
    if state.is_symlink():
        raise ValueError("Test state cannot be a symlink")
    provider.mkdir(state, parents=True, exist_ok=True)
    for _ in range(32):
        token = uuid.uuid4().hex[:4]
        if token in _issued:
            continue
        owner = state / token
        try:
            provider.mkdir(owner, exist_ok=False)
            _issued.add(token)
            break
        except FileExistsError:
            continue
    else:
        raise FileExistsError("Cannot reserve a test workspace")
    try:
        expanded = [part.replace("@WORK@", str(owner)) for part in command]
        env = dict(os.environ, TMPDIR=str(owner), TMP=str(owner), TEMP=str(owner))
        return execute(expanded, cwd=owner, env=env, check=False).returncode
    finally:
        shutil.rmtree(owner)


def main(argv):
    if len(argv) < 3:
        raise ValueError("Expected provider directory, resources and command")
    sys.path.insert(0, argv[0])
    return run(argv[1], argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
