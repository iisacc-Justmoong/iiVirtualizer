"""Installation defaults must never use the module build tree."""
from pathlib import Path
import os
import subprocess
import sys

sys.path.insert(0, sys.argv[2])
import ii_file_provider as provider
project = Path(__file__).resolve().parents[1]
owner = Path(sys.argv[3]).resolve()
cmake = sys.argv[1]
home = os.environ.get("HOME") or os.environ.get("USERPROFILE")
assert home
def probe(name, override=None):
    source = owner / name
    provider.mkdir(source, parents=True, exist_ok=False)
    content = ("cmake_minimum_required(VERSION 3.31)\nproject(Prefix NONE)\n"
               + 'include("' + str(project / "cmake/InstallPrefix.cmake") + '")\n'
               + "iiv_set_install_prefix()\n")
    provider.write_text(source / "CMakeLists.txt", content)
    command = [cmake, "-S", str(source), "-B", str(source / "build"), "-G", "Ninja"]
    if override is not None: command.append("-DCMAKE_INSTALL_PREFIX=" + str(override))
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    cache = (source / "build/CMakeCache.txt").read_text()
    expected = override or Path(home) / ".local/SDK/iiVirtualizer"
    assert "CMAKE_INSTALL_PREFIX:PATH=" + str(expected) in cache
probe("default")
probe("explicit", owner / "custom prefix")

bin_dir = owner / "bin"
provider.mkdir(bin_dir, exist_ok=False)
for name in ("cmake", "ctest"):
    script = bin_dir / name
    provider.write_text(script, "#!/bin/sh\nprintf 'fake:'\nprintf ' <%s>' \"$@\"\nprintf '\\n'\n")
    script.chmod(0o700)
env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + "/usr/bin:/bin", OPENSSL_ROOT_DIR=str(owner))
def script_run(*args):
    result = subprocess.run(["/bin/sh", str(project / "install-native.sh"), *map(str, args)],
                            capture_output=True, text=True, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout
assert "<--prefix> <" + str(Path(home) / ".local/SDK/iiVirtualizer") + ">" in script_run()
assert "<--prefix> <" + str(owner / "custom prefix") + ">" in script_run(owner / "custom prefix")
