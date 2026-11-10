"""VM data and test workspaces must stay in the project resource tree."""
from pathlib import Path
import shutil
import sys
import unittest
import uuid
import ii_file_provider as provider

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from resource_paths import resource_path, image_root, instance_root, dependency_root

class ResourcePathsTest(unittest.TestCase):
    def test_defaults_keep_persistent_vm_data_out_of_build(self):
        self.assertEqual(image_root(ROOT), ROOT / "resources/images")
        self.assertEqual(instance_root(ROOT), ROOT / "resources/vm")
        self.assertEqual(dependency_root(ROOT), ROOT / "resources/dependencies")

    def test_rejects_build_root_and_path_traversal(self):
        for path in (ROOT / "build/outside-resources", ROOT / "resources", ROOT / "resources/../build/file"):
            with self.assertRaises(ValueError):
                resource_path(ROOT, path)
        self.assertEqual(resource_path(ROOT, ROOT / "resources/images/example"),
                         (ROOT / "resources/images/example").resolve())
        legacy = ROOT / "build/images"
        if legacy.is_symlink():
            self.assertEqual(resource_path(ROOT, legacy / "example"),
                             (ROOT / "resources/images/example").resolve())

    def test_rejects_symlink_to_an_outside_directory(self):
        owner = ROOT / "resources/.state" / ("path-" + uuid.uuid4().hex[:8])
        provider.mkdir(owner, parents=True, exist_ok=False)
        try:
            link = owner / "outside"
            try:
                link.symlink_to(ROOT / "build", target_is_directory=True)
            except OSError as error:
                self.skipTest("Symbolic links are unavailable: " + str(error))
            with self.assertRaises(ValueError):
                resource_path(ROOT, link / "example")
        finally:
            shutil.rmtree(owner)

if __name__ == "__main__":
    unittest.main()
