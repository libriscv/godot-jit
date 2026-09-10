#!/usr/bin/env python3
"""Run the shared demo compatibility harness with UnsafeGDScript."""

import importlib.util
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "shared_demo_compat", ROOT / "godot-sandbox/scripts/demo_compat.py"
)
harness = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = harness
spec.loader.exec_module(harness)
harness.SCRIPT_MODE = "ugd"
harness.SCRIPT_SUFFIX = ".ugd"
harness.LANGUAGE_NAME = "UnsafeGDScript"
harness.LIBRARY_NAME = "libgodot-jit.so"
harness.ENTRY_SYMBOL = "godot_jit_library_init"
harness.MINIMUM_VERSION = "4.6"
harness.DESCRIPTOR_NAME = "unsafegdscript.gdextension"
harness.STATE_NAME = ".unsafegdscript-compat.json"
harness.ADDON_REL = Path("addons/unsafegdscript_compat_harness")
harness.EXTENSION_RESOURCE_PATH = "res://addons/unsafegdscript_compat_harness/unsafegdscript.gdextension"
harness.DIAGNOSTIC_MARKERS += ("UnsafeGDScript:",)

def find_extension(explicit):
    path = Path(explicit or os.environ.get("UNSAFEGDSCRIPT_EXTENSION") or
                ROOT / "bin/addons/godot_jit/bin/libgodot-jit.linux.template_debug.x86_64.so").expanduser()
    if not path.is_file():
        raise harness.HarnessError("UnsafeGDScript extension not found; build the debug preset or pass --extension")
    return path.resolve()


harness.find_extension = find_extension

if __name__ == "__main__":
    try:
        raise SystemExit(harness.main())
    except harness.HarnessError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
