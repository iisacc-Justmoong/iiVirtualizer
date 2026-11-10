"""Legacy links must be repeatable and never replace existing data."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, sys.argv[2])
import ii_file_provider as provider
project = Path(__file__).resolve().parents[1]
owner = Path(sys.argv[3]).resolve()
resources = owner / "resources"
build = owner / "build"
provider.mkdir(resources, parents=True, exist_ok=False)
provider.mkdir(build, exist_ok=False)
script = project / "tests/resource_links.cmake"
helper = project / "cmake/ResourceLinks.cmake"
def run():
    return subprocess.run([sys.argv[1], "-DHELPER=" + str(helper),
        "-DRESOURCE_ROOT=" + str(resources), "-DBUILD_ROOT=" + str(build),
        "-P", str(script)], capture_output=True, text=True)
assert run().returncode == 0
assert not list(build.iterdir())
provider.mkdir(resources / "vm", exist_ok=False)
assert run().returncode == 0
link = build / "instances"
assert link.is_symlink() and link.resolve() == (resources / "vm").resolve()
assert run().returncode == 0
link.unlink()
provider.write_text(link, "preserve")
assert run().returncode != 0 and link.read_text() == "preserve"
link.unlink()
link.symlink_to(owner / "outside", target_is_directory=True)
assert run().returncode != 0 and link.is_symlink()
link.unlink()
(resources / "vm").rmdir()
(resources / "vm").symlink_to(owner / "outside", target_is_directory=True)
assert run().returncode != 0 and (resources / "vm").is_symlink()
