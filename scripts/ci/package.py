#!/usr/bin/env python3
"""Validate collected CI binaries and write an addon ZIP rooted at addons/."""

import argparse
import configparser
import io
from pathlib import Path
import zipfile


REPOSITORY = Path(__file__).resolve().parents[2]
ADDON = Path("addons/godot_jit")
TARGETS = {
    "linux": ("so", ("x86_64", "arm64")),
    "macos": ("dylib", ("x86_64", "arm64")),
    "windows": ("dll", ("x86_64",)),
}


def package(root: Path, output: Path) -> None:
    descriptor = configparser.ConfigParser(interpolation=None)
    descriptor.read(REPOSITORY / "bin" / ADDON / "godot_jit.gdextension")
    libraries = {}
    files = []
    for platform, (suffix, architectures) in TARGETS.items():
        for arch in architectures:
            for variant in ("debug", "release"):
                key = f"{platform}.{variant}.{arch}"
                path = ADDON / "bin" / variant / f"libgodot-jit.{platform}.{arch}.{suffix}"
                expected = f'"res://{path.as_posix()}"'
                if descriptor["libraries"].get(key) != expected:
                    raise ValueError(f"Unexpected or missing library mapping: {key}")
                binary = root / path
                if not binary.is_file() or binary.stat().st_size == 0:
                    raise ValueError(f"Missing or empty library: {binary}")
                libraries[key] = expected
                files.append(path)

    # The source descriptor also supports locally built RISC-V. Release ZIPs
    # declare only the architectures actually built and tested by CI.
    descriptor["libraries"] = libraries
    manifest = io.StringIO()
    descriptor.write(manifest)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr((ADDON / "godot_jit.gdextension").as_posix(), manifest.getvalue())
        archive.write(REPOSITORY / "README.md", (ADDON / "README.md").as_posix())
        for path in sorted(files):
            archive.write(root / path, path.as_posix())
    print(f"Created {output} with {len(files)} libraries under {ADDON}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="directory containing collected addons/")
    parser.add_argument("output", type=Path, help="destination ZIP")
    args = parser.parse_args()
    package(args.root, args.output)
