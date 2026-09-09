#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tests/run_tests.sh [debug|release] [ctest arguments...]

Configure, build, and run all tests (debug by default), including tests/ugd/test_*.ugd.
Set GODOT to a Godot 4.6+ executable; otherwise use PATH or the preset's cached path.
Set CMAKE_BUILD_PARALLEL_LEVEL to limit build jobs.

Examples:
  tests/run_tests.sh
  tests/run_tests.sh release -R godot_jit_ugd
EOF
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    usage
    exit 0
fi
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
preset=debug
case "${1:-}" in
    debug|release) preset=$1; shift ;;
    ''|-*) ;;
    *) usage >&2; exit 2 ;;
esac

godot=${GODOT:-}
if [[ -z $godot ]]; then
    godot=$(command -v godot || command -v godot4 || true)
    if [[ -z $godot && -f build/$preset/CMakeCache.txt ]]; then
        godot=$(sed -n 's/^GODOT_JIT_GODOT_EXECUTABLE:FILEPATH=//p' "build/$preset/CMakeCache.txt")
    fi
fi
if [[ -z $godot ]] || ! godot=$(command -v -- "$godot"); then
    echo 'Godot is required for the .ugd suite. Set GODOT to a Godot 4.6+ executable.' >&2
    exit 1
fi

cmake --preset "$preset" -DGODOT_JIT_BUILD_TESTS=ON "-DGODOT_JIT_GODOT_EXECUTABLE=$godot"
cmake --build --preset "$preset"
ctest --preset "$preset" --no-tests=error "$@"
