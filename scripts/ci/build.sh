#!/usr/bin/env bash
set -euo pipefail

platform=${1:?platform required}
arch=${2:?architecture required}
godot_version=4.6.3
case "$platform/$arch" in
    linux/x86_64|linux/arm64)
        suffix=so
        godot_asset="linux.$arch"
        godot_binary="Godot_v${godot_version}-stable_${godot_asset}"
        ;;
    macos/x86_64|macos/arm64)
        suffix=dylib
        godot_asset=macos.universal
        godot_binary=Godot.app/Contents/MacOS/Godot
        ;;
    windows/x86_64)
        suffix=dll
        godot_asset=win64.exe
        godot_binary="Godot_v${godot_version}-stable_${godot_asset}"
        ;;
    *) echo "Unsupported CI target: $platform/$arch" >&2; exit 1 ;;
esac

# Fail early when a checkout still points at the frontend before the C backend.
if [[ ! -f godot-sandbox/src/gdscript/compiler/c_abi.h ]]; then
    echo 'The godot-sandbox submodule must point at a committed C backend.' >&2
    exit 1
fi

godot_dir="$PWD/.build/godot"
mkdir -p "$godot_dir"
curl --fail --location --retry 3 \
    "https://github.com/godotengine/godot-builds/releases/download/${godot_version}-stable/Godot_v${godot_version}-stable_${godot_asset}.zip" \
    --output "$godot_dir/godot.zip"
unzip -oq "$godot_dir/godot.zip" -d "$godot_dir"
chmod +x "$godot_dir/$godot_binary"
"$godot_dir/$godot_binary" --headless --version

cmake --preset release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DGODOTCPP_USE_STATIC_CPP=ON \
    -DGODOT_JIT_BUILD_TESTS=ON \
    -DGODOT_JIT_GODOT_EXECUTABLE="$godot_dir/$godot_binary"
cmake --build --preset release --parallel 2
ctest --preset release --no-tests=error

library="bin/addons/godot_jit/bin/release/libgodot-jit.$platform.$arch.$suffix"
test -s "$library"
if [[ "$platform" == windows ]]; then
    # A developer's MSYS2 PATH can hide accidental runtime dependencies.
    imports=$(objdump -p "$library")
    if grep -Ei 'DLL Name:.*(libgcc|libstdc\+\+|libwinpthread|libssp|msys-)' <<< "$imports"; then
        echo 'The addon must load without MinGW/MSYS2 runtime DLLs.' >&2
        exit 1
    fi
fi
destination=".build/artifact/addons/godot_jit/bin/release"
mkdir -p "$destination"
cp "$library" "$destination/"
