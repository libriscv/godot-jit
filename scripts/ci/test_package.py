import configparser
from pathlib import Path
import tempfile
import unittest
import zipfile

from package import package


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "godot-jit.zip"
        self.binaries = []
        for target in (
            "linux.x86_64.so", "linux.arm64.so", "macos.x86_64.dylib",
            "macos.arm64.dylib", "windows.x86_64.dll",
        ):
            for variant in ("debug", "release"):
                path = self.root / "addons/godot_jit/bin" / variant / f"libgodot-jit.{target}"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"test fixture")
                self.binaries.append(path)

    def test_drop_in_layout_and_library_paths(self):
        (self.root / "project.godot").write_text("must not ship")
        junk = self.root / "addons/godot_jit/bin/debug/unexpected.dll"
        junk.write_bytes(b"must not ship")
        package(self.root, self.output)
        with zipfile.ZipFile(self.output) as archive:
            names = set(archive.namelist())
            self.assertEqual(names, {
                *(path.relative_to(self.root).as_posix() for path in self.binaries),
                "addons/godot_jit/godot_jit.gdextension", "addons/godot_jit/README.md",
            })
            descriptor = configparser.ConfigParser(interpolation=None)
            descriptor.read_string(archive.read("addons/godot_jit/godot_jit.gdextension").decode())
            self.assertEqual(len(descriptor["libraries"]), 10)
            self.assertEqual(descriptor["configuration"]["entry_symbol"], '"godot_jit_library_init"')
            for value in descriptor["libraries"].values():
                self.assertIn(value.strip('"').removeprefix("res://"), names)

    def test_missing_platform_binary_fails_before_archive(self):
        self.binaries[-1].unlink()
        with self.assertRaisesRegex(ValueError, "Missing or empty library"):
            package(self.root, self.output)
        self.assertFalse(self.output.exists())

    def test_empty_binary_fails_before_archive(self):
        self.binaries[0].write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "Missing or empty library"):
            package(self.root, self.output)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
