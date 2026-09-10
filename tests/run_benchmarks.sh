#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tests/run_benchmarks.sh [debug|release] [iterations]

Configure, build, and compare GDScript with godot-jit in both benchmark suites.
Defaults: release build, 100000 iterations per workload.
Set GODOT to a Godot 4.6+ executable; otherwise use PATH or the preset's cached path.
Set CMAKE_BUILD_PARALLEL_LEVEL to limit build jobs.
Logs are saved under build/<preset>/benchmark-logs/.

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

for suite in call typical; do
    echo "Running $suite benchmarks ($iterations iterations)..."
    run_logged "$suite" "$godot" --headless --path "$project" --script "res://tests/${suite}_benchmark.gd" -- "$iterations"
    # Godot can log script errors yet exit successfully. Never report those runs.
    if grep -Eq 'SCRIPT ERROR:|^ERROR:' "$log_dir/$suite.log"; then
        cat "$log_dir/$suite.log" >&2
        exit 1
    fi
done

printf '\nBuild: %s; iterations: %s; speedup = GDScript / godot-jit (>1 is faster).\n' "$preset" "$iterations"
echo 'Times are median microseconds per operation; compilation/setup excluded.'
echo "Logs: $log_dir"

# Normalize call totals to us/op; typical already reports us/op and JIT/GD.
awk -v iterations="$iterations" '
function border() {
    print "+----------------------+------------------+------------------+-----------+"
}
function header(title) {
    print "\n" title
    border()
    printf "| %-20s | %16s | %16s | %9s |\n", "Workload", "GDScript us/op", "godot-jit us/op", "Speedup"
    border()
}
function row(name, gd, jit, ratio) {
    sub(/:$/, "", name)
    speedup = ratio > 0 ? sprintf("%.3fx", 1 / ratio) : "n/a"
    printf "| %-20s | %16.3f | %16.3f | %9s |\n", name, gd / iterations, jit / iterations, speedup
}
/^Call benchmark:/ { header("Calls and loops (median of 5 after warm-up)") }
/^[a-z0-9_]+: GDScript=[0-9]+ us UnsafeGDScript=[0-9]+ us/ {
    split($2, gd, "="); split($4, jit, "=")
    row($1, gd[2], jit[2], gd[2] > 0 ? jit[2] / gd[2] : 0); calls++
}
/^Typical benchmark:/ {
    border()
    header("Typical minigame (median of 7 after warm-up; alternating order)")
}
/^Godot:/ { print $0 > "/dev/stderr" }
/^[a-z0-9_]+: GD=/ {
    split($2, gd, "="); split($4, jit, "="); split($6, ratio, "=")
    row($1, gd[2] * iterations, jit[2] * iterations, ratio[2]); typical++
}
END {
    border()
    if (calls != 9 || typical != 13) {
        print "Incomplete benchmark results; inspect the logs." > "/dev/stderr"
        exit 1
    }
}
' "$log_dir/call.log" "$log_dir/typical.log"
