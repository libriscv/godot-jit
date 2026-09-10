"""Conversion and comparison regressions; no Godot installation required."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("jit_demo_compat", Path(__file__).resolve().parents[1] / "scripts/demo_compat.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
harness = module.harness


class CompatibilityHarnessTests(unittest.TestCase):
    def test_staging_exposes_only_one_script_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            script = project / "node.gd"
            source = b"class_name CompatNode\nextends Node\n"
            script.write_bytes(source)
            resource = project / "project.godot"
            original_resource = b'[autoload]\nGlobal="*res://node.gd"\n'
            resource.write_bytes(original_resource)
            library = project / "extension.so"
            library.touch()

            def run(_godot, _project, _root, phase, *_args):
                self.assertFalse(script.exists(), "Original must not enter the global class scan")
                self.assertEqual(script.with_suffix(".ugd").read_bytes(), source)
                return harness.RunResult("fixture", phase, [], 0, False, 0, [], phase + ".log")

            with patch.object(harness, "run_godot", side_effect=run):
                harness.to_safe_mode(project, project, Path("godot"), library, 1, project)
            self.assertIn(b"node.ugd", resource.read_bytes())
            harness.to_gd_mode(project, project, Path("godot"), 1, project, reimport=False)
            self.assertEqual(script.read_bytes(), source)
            self.assertEqual(resource.read_bytes(), original_resource)
            self.assertFalse((project / harness.ADDON_REL).exists())

    def test_bootstrap_diagnostics_are_not_scored(self):
        first = harness.RunResult("fixture", "bootstrap-import", [], 0, False, 1.0, ["ERROR: duplicate class"], "bootstrap.log")
        final = harness.RunResult("fixture", "import", [], 0, False, 2.0, ["ERROR: real error"], "import.log")
        result = harness.combine_imports(first, final)
        self.assertEqual(result.diagnostics, final.diagnostics)
        self.assertIn("bootstrap.log", result.log)
        first.returncode = 1
        first.timed_out = True
        self.assertEqual(harness.combine_imports(first, final).returncode, 1)
        self.assertTrue(harness.combine_imports(first, final).timed_out)

    def test_script_identity_and_restore_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            dependency = project / "node_25d.gd"
            dependency.write_text("class_name Node25D\nextends Node2D\n")
            plugin = project / "plugin.gd"
            source = b'''extends "node_25d.gd"
const NODE = preload("node_25d.gd")
const ABSOLUTE = preload('res://node_25d.gd')
# Keep comments like "node_25d.gd" untouched.
const MESSAGE = """A message about 'node_25d.gd'."""
const UNKNOWN = "unavailable.gd"
'''
            plugin.write_bytes(source)
            scripts = [dependency, plugin]
            converted = harness.converted_script(project, plugin, scripts)
            self.assertIn(b'extends "node_25d.ugd"', converted)
            self.assertIn(b'preload("node_25d.ugd")', converted)
            self.assertIn(b"preload('res://node_25d.ugd')", converted)
            self.assertIn(b'# Keep comments like "node_25d.gd"', converted)
            self.assertIn(b"A message about 'node_25d.gd'.", converted)
            self.assertIn(b'"unavailable.gd"', converted)
            self.assertEqual(plugin.read_bytes(), source)
            peer = plugin.with_suffix(".ugd")
            peer.write_bytes(converted)
            harness.save_state(project, {
                "version": 2, "mode": "ugd", "scripts": ["plugin.gd"],
                "rewritten": [], "generated_sha256": {"plugin.gd": harness.sha256(converted)},
            })
            peer.write_bytes(converted + b"# User edit\n")
            with self.assertRaises(harness.HarnessError):
                harness.to_gd_mode(project, project, Path("godot"), 1, project, reimport=False)
            peer.write_bytes(converted)
            harness.to_gd_mode(project, project, Path("godot"), 1, project, reimport=False)
            self.assertFalse(peer.exists())
            self.assertEqual(plugin.read_bytes(), source)


if __name__ == "__main__":
    unittest.main()
