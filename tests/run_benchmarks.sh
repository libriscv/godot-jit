#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tests/run_benchmarks.sh [debug|release] [iterations]

Configure, build, and compare GDScript with godot-jit in the call, typical, and math benchmark suites.
Defaults: release build, 100000 iterations per workload.
Set GODOT to a Godot 4.6+ executable; otherwise use PATH or the preset's cached path.
Set CMAKE_BUILD_PARALLEL_LEVEL to limit build jobs.
Logs are saved under build/<preset>/benchmark-logs/.
Linux also runs cc -O2 and counts user-space instructions per row.
Set GODOT_JIT_BENCH_CC=clang to select a compiler, or off to skip the twin.
Set GODOT_JIT_INSTRUCTIONS=0 to skip instruction counting.

Examples:
  tests/run_benchmarks.sh
  tests/run_benchmarks.sh release 1000000
  GODOT=/path/to/godot tests/run_benchmarks.sh debug 10000
EOF
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    usage
    exit 0
fi
preset=release
case "${1:-}" in
    debug|release) preset=$1; shift ;;
esac
iterations=${1:-100000}
if (( $# > 1 )) || [[ ! $iterations =~ ^[1-9][0-9]*$ ]]; then
    usage >&2
    exit 2
fi

# Resolve GODOT before changing directories so relative executable paths work.
godot=${GODOT:-}
if [[ -n $godot ]]; then
    godot=$(command -v -- "$godot" || true)
    if [[ -n $godot && $godot != /* ]]; then godot="$PWD/$godot"; fi
fi
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
if [[ -z ${GODOT:-} ]]; then
    godot=$(command -v godot || command -v godot4 || true)
    if [[ -z $godot && -f build/$preset/CMakeCache.txt ]]; then
        godot=$(sed -n 's/^GODOT_JIT_GODOT_EXECUTABLE:FILEPATH=//p' "build/$preset/CMakeCache.txt")
    fi
fi
if [[ -z $godot ]] || ! godot=$(command -v -- "$godot"); then
    echo 'Godot is required. Set GODOT to a Godot 4.6+ executable.' >&2
    exit 1
fi

build_dir="$PWD/build/$preset"
log_dir="$build_dir/benchmark-logs"
mkdir -p "$log_dir"
# Keep Godot data and benchmark reports local to this build, as CTest does.
export XDG_DATA_HOME="$build_dir/test-data"
export XDG_CONFIG_HOME="$build_dir/test-config"
export XDG_CACHE_HOME="$build_dir/test-cache"
export LC_ALL=C

run_logged() {
    local name=$1
    shift
    if ! "$@" >"$log_dir/$name.log" 2>&1; then
        cat "$log_dir/$name.log" >&2
        echo "$name failed; see $log_dir/$name.log" >&2
        return 1
    fi
}

echo "Building godot-jit ($preset)..."
run_logged configure cmake --preset "$preset" -DGODOT_JIT_BUILD_TESTS=ON "-DGODOT_JIT_GODOT_EXECUTABLE=$godot"
run_logged build cmake --build --preset "$preset"
config=Release
if [[ $preset == debug ]]; then config=Debug; fi
project="$build_dir/test-project/$config"
run_logged import "$godot" --headless --path "$project" --import

export GODOT_JIT_INSTRUCTIONS=${GODOT_JIT_INSTRUCTIONS:-1}
cc=${GODOT_JIT_BENCH_CC:-cc}
for suite in call typical math; do
    for compiler in tcc cc; do
        name=$suite
        compiler_env=(env -u GODOT_JIT_CC)
        if [[ $compiler == cc ]]; then
            if [[ $cc == off || $(uname -s) != Linux ]]; then
                rm -f "$log_dir/$suite-cc.log" "$log_dir/$suite-cc.json" "$log_dir/$suite-cc.c"
                continue
            fi
            name="$suite-cc"
            compiler_env=(env "GODOT_JIT_CC=$cc")
        fi
        echo "Running $suite with $compiler ($iterations iterations)..."
        if [[ $suite == math ]]; then
            run_logged "$name" "${compiler_env[@]}" "$godot" --headless --path "$project" --script res://tests/math.gd -- --benchmark "$iterations"
        else
            run_logged "$name" "${compiler_env[@]}" "$godot" --headless --path "$project" --script "res://tests/${suite}_benchmark.gd" -- "$iterations" --dump-c
        fi
        if grep -Eq 'SCRIPT ERROR:|^ERROR:' "$log_dir/$name.log"; then
            cat "$log_dir/$name.log" >&2
            exit 1
        fi
        if [[ $suite == typical ]]; then
            report=$(sed -n 's/^Results: //p' "$log_dir/$name.log")
            cp -- "$report" "$log_dir/$name.json"
        fi
        if [[ $suite != math ]]; then
            generated=$(sed -n 's/^Generated C: //p' "$log_dir/$name.log")
            cp -- "$generated" "$log_dir/$name.c"
            if [[ $compiler == cc ]]; then cmp -- "$log_dir/$suite.c" "$log_dir/$suite-cc.c"; fi
        fi
    done
done

printf '\nBuild: %s; iterations: %s\n' "$preset" "$iterations"
echo "Logs: $log_dir"
python3 tests/benchmark_report.py --logs "$log_dir" --iterations "$iterations"
