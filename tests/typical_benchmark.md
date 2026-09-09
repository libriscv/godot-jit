# Typical minigame measurements

The optimized C backend meets the stopping criterion for this fixture:
per-frame `think` takes **18–20% less time than GDScript**, and the complete
headless minigame is **at parity**. Optimization stopped at that point.

Measured 2026-09-09 on an AMD Ryzen 9 7950X, Linux x86_64, Godot 4.6.3 stable
(`7d41c59c4`), with the Release extension and libtcc. The same-session baseline
was 2.054× GDScript time for `think_5` and 1.218× for the minigame.
The fixture and benchmark harness were not changed.

## Results

Each invocation measures 100,000 operations per row, with one warm-up and seven
measured samples per language. Execution order alternates each repetition.
Compilation, instance creation and initial world/RNG creation are excluded.
Every warm-up and timed repetition must match the other language and earlier
repetitions in both result type and value. Simulation checksums include final
position, crystal/hazard positions, counts, score and RNG state.

The before columns are one fresh baseline invocation. The after columns are
medians of three per-invocation medians. The last column divides the two after
medians; lower is better. Times are µs/op. These are elapsed CPU workload
timings, without rendering, real-time waits or physics scheduling.

| Workload | Before GD | Before JIT | After GD | After JIT | After JIT/GD |
|---|---:|---:|---:|---:|---:|
| think_5 | 0.384 | 0.789 | 0.409 | 0.329 | 0.805 |
| minigame | 2.041 | 2.486 | 1.995 | 1.994 | 1.000 |
| call_floor | 0.113 | 0.166 | 0.114 | 0.146 | 1.285 |
| nearest_0 | 0.132 | 0.216 | 0.132 | 0.188 | 1.431 |
| nearest_5 | 0.296 | 0.665 | 0.280 | 0.247 | 0.883 |
| iterate_5 | 0.209 | 0.482 | 0.199 | 0.228 | 1.145 |
| distance_5 | 0.238 | 0.659 | 0.237 | 0.239 | 1.010 |
| normalize | 0.107 | 0.213 | 0.108 | 0.190 | 1.766 |
| batch_think | 0.343 | 0.601 | 0.338 | 0.162 | 0.478 |
| batch_nearest | 0.253 | 0.516 | 0.251 | 0.118 | 0.469 |
| batch_iterate | 0.168 | 0.350 | 0.168 | 0.106 | 0.631 |
| batch_normalize | 0.077 | 0.090 | 0.076 | 0.068 | 0.900 |
| batch_floor | 0.080 | 0.056 | 0.080 | 0.023 | 0.283 |

Paired `think_5` ratios in the optimized invocations were 0.805, 0.818 and 0.806;
minigame ratios were 1.000, 1.001 and 0.990. Absolute timings vary with machine
conditions. The full simulation is at parity, rather than showing a convincing
speedup. Standalone call/normalization diagnostics still expose entry overhead;
this result does not claim every C-backend workload beats GDScript.

[Raw baseline and optimized samples](typical_benchmark_results.json) retain
all seven samples per row, checksums, fixture hash, seed and environment metadata.

## Churn removed

The evidence comes from isolated diagnostic workloads, generated-code inspection
and measurements after each change, not a sampling profile. These costs overlap
and are not additive percentages.

1. **Array traversal:** the original five-point loop made six dynamic `size()`
   calls and five generic indexed reads. Array size now borrows the native
   storage through Godot's public internal-value getter and calls its cached
   typed method. Integer indexing uses the checked indexed API and transfers
   the returned Variant's ownership instead of copying and destroying it.
2. **Loop step fusion:** an adjacent Array size / less-than / exit-branch / read
   sequence becomes one `gj_array_next` call. Fusion requires private size and
   comparison temporaries and a proven integer index. Each step rechecks the
   current size and obtains the element through the public const Array API.
   It neither caches the loop bound nor retains an element pointer across user
   code, so appending, removing and clearing elements remain visible.
3. **Vector methods:** matching Vector2 operands compute squared distance in
   inline C using engine real precision. Normalization calls the engine's cached
   typed built-in, retaining its exact zero/non-finite behavior. Other receiver
   types, custom same-name methods, and super calls retain generic dispatch.
4. **Boxes and ownership:** comparisons keep an already-proven scalar operand
   and boolean result unboxed. Integer pairs still compare as integers beyond
   double precision. Unreferenced arguments no longer acquire local Variant
   ownership; signature and arity validation remain in place.

The first built-in specialization pass reduced `think_5` from 0.789 to 0.415
µs/op, still slower than GDScript. The subsequent ownership/comparison changes
left it slower too. Loop step fusion closed the remaining gap. In the normal
five-point `think` path, the original 17 generic helper calls are replaced by
six checked loop steps and one typed normalization; distance is inline.

## Workload and correctness

`bin/tests/typical_benchmark.gd` reads `tests/ugd/typical.ugd` copied into the
CMake test project. Both languages compile identical source, with identical
diagnostic helpers appended. The fixture SHA-256 remains
`c62dcf3d6eac8eab0dc202697af1ec1709ae37fb4d04d516ea53f925275d89ac`.

The game runs fixed 1/60-second steps with a drone moving at 120 units/second,
five crystals worth +10 each, and five hazards rotating around the spawn point.
A hazard hit costs 5 and respawns the drone. Collected crystals get replacement
positions from a private RNG seeded with 1234567. Movement, collisions, scoring
and respawns run in the same GDScript driver; `think()` is under comparison.
The supplied script ignores hazards and delta.

Correctness covers empty arrays, coincident positions, normalization, negative
coordinates, nearest-point selection and first-point selection on ties.
Independent worlds produce matching directions and world state on every one
of 4,096 frames, including RNG state. Both collect 59 crystals, hit 7 hazards
and finish with score 555. The Release CTest suite passes all nine tests.
Additional backend regressions cover mutation during iteration, negative
indexing, owned element lifetimes/reference counts, other container/vector
types, custom same-name methods, tiny/large vectors and integer comparisons
beyond double precision. Emitted benchmark C also passes strict C99 syntax checks.

`think_5` uses five fixed crystals, five hazards and 512 repeating positions.
`minigame` advances the complete deterministic game. `call_floor` returns the
position with the same four argument types as `think`; `nearest_0/5` exercise
the actual nearest function; `iterate_5` returns the last of five points;
`distance_5` sums five squared distances; `normalize` subtracts and normalizes.
The `batch_*` rows run a compiled loop with one external entry, using a fixed
position and accumulating local-call results. They isolate local execution
but are not directly subtractable from the varying-position external rows.

## Reproduce

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
godot --headless --path build/release/test-project/Release \
  --script res://tests/typical_benchmark.gd -- 100000 --dump-c
```

Use the Godot executable selected by CMake (this machine uses
`/home/gonzo/Godot_v4.6.3-stable_linux.x86_64`). Run on an otherwise idle machine
and repeat the command. `--check-only` runs correctness without benchmarking.
The benchmark prints the paths to `user://typical_benchmark.c` and
`user://typical_benchmark.json`; each invocation replaces those output files.
Performance has no pass/fail threshold in CTest.
