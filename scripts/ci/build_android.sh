#!/usr/bin/env bash
set -euo pipefail

arch=${1:?architecture required}
shift
case "$arch" in
    arm64) abi=arm64-v8a ;;
    x86_64) abi=x86_64 ;;
    *) echo "Unsupported Android JIT architecture: $arch" >&2; exit 1 ;;
esac

ndk=${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT must point to an Android NDK}
test -f "$ndk/build/cmake/android.toolchain.cmake"
build_dir="build/android-$arch"
cmake --preset release -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM=android-24 \
    -DANDROID_STL=c++_static \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DGODOT_JIT_BUILD_TESTS=OFF \
    "$@"
# Android binaries cannot run in the Linux host's CTest/Godot process.
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

library="bin/addons/godot_jit/bin/libgodot-jit.android.template_release.$arch.so"
test -s "$library"
destination=".build/artifact/addons/godot_jit/bin"
mkdir -p "$destination"
cp "$library" "$destination/"
