# Godot JIT

A JIT-like backend for the GS SafeGDScript compiler, executed by libtcc
inside a GDExtension.

This is a glue project that re-uses work that already exists by me. The compiler
is from Godot Sandbox, the JIT is my libtcc fork from libriscv and the backend is
more or less the AOT backend from Godot Sandbox Plus. Highly experimental, though
it does run all 118 godot demo projects for 5 seconds without errors.

## UnsafeGDScript

The extension registers `UnsafeGDScript` as a Godot ScriptLanguage. Use `.ugd`
(or `.unsafegd`) files, attach them to nodes in scenes, or construct instances
through the script resource:

```gdscript
var counter = load("res://counter.ugd").new(10)
print(counter.increment())
```

For example, `counter.ugd` can contain ordinary GDScript:

```gdscript
extends RefCounted

var total: int

func _init(start: int = 0):
    total = start

func increment(amount: int = 1) -> int:
    total += amount
    return total
```

The language uses SafeGDScript's syntax, including `await` for signals, timers,
and other coroutine calls:

```gdscript
func delayed_answer() -> int:
    await get_tree().create_timer(0.1).timeout
    return 42
```

Editor completion, documentation, and profiler integration = not yet.

Since nobody's paying for this, don't have any expectations. Thank.

## Godot JIT directly

```gdscript
var jit = GodotJIT.new()
assert(jit.compile_sgd("func answer(n: int) -> int:\n    return n * 2 + 2\n"))
assert(jit.execute_function("answer", [20]) == 42)
```

## Performance

Godot 4.6.3-stable, AMD Ryzen 9 7950X, release build, 100 000 iterations.

### math

| Benchmark | GDScript | JIT | Speedup | insn/op GD | insn/op JIT |
|---|---:|---:|---:|---:|---:|
| bench_primitives | 0.0225 µs/op | 0.0044 µs/op | **5.17x** | 518 | 52 |
| bench_engine | 0.0398 µs/op | 0.0274 µs/op | **1.45x** | 766 | 233 |

### loops and calls

| Benchmark | GDScript | JIT | Speedup | insn/op GD | insn/op JIT |
|---|---:|---:|---:|---:|---:|
| integer_loop | 1713 µs | 167 µs | **10.26x** | 400 | 35 | 
| local_calls | 21247 µs | 2666 µs | **7.97x** | 4353 | 474 |
| gameplay_loop | 3533 µs | 791 µs | **4.47x** | 737 | 133 |
| float_loop | 2081 µs | 607 µs | **3.43x** | 484 | 36 |
| vector_loop | 1098 µs | 390 µs | **2.82x** | 255 | 27 |
| object_calls | 8757 µs | 5566 µs | **1.57x** | 1800 | 1085 |
| callable_calls | 8339 µs | 5753 µs | **1.45x** | 1839 | 1207 |
| dictionary_loop | 12801 µs | 9917 µs | **1.29x** | 3004 | 2251 |
| array_loop | 6510 µs | 6230 µs | **1.04x** | 1557 | 1020 |
| **Geometric mean** | | | **2.81x** | | |

### typical game-like code

| Benchmark | GDScript | JIT | Speedup | insn/op GD | insn/op JIT |
|---|---:|---:|---:|---:|---:|
| batch_floor | 0.081 µs/op | 0.007 µs/op | **11.63x** | 1843 | 127 | 
| batch_think | 0.341 µs/op | 0.080 µs/op | **4.26x** | 7158 | 1349 | 
| batch_nearest | 0.252 µs/op | 0.063 µs/op | **3.97x** | 5215 | 1210 |
| batch_normalize | 0.075 µs/op | 0.023 µs/op | **3.28x** | 1624 | 140 |
| batch_iterate | 0.169 µs/op | 0.057 µs/op | **2.98x** | 3529 | 1072 |
| think_5 | 0.372 µs/op | 0.172 µs/op | **2.16x** | 8059 | 3290 | 
| nearest_5 | 0.293 µs/op | 0.147 µs/op | **1.99x** | 6116 | 3101 |
| distance_5 | 0.243 µs/op | 0.139 µs/op | **1.75x** | 5337 | 3004 |
| iterate_5 | 0.200 µs/op | 0.132 µs/op | **1.51x** | 4318 | 2938 |
| nearest_0 | 0.132 µs/op | 0.096 µs/op | **1.37x** | 2945 | 2131 |
| call_floor | 0.113 µs/op | 0.094 µs/op | **1.20x** | 2632 | 2065 |
| minigame | 2.015 µs/op | 1.805 µs/op | **1.12x** | 40800 | 35940 |
| normalize | 0.107 µs/op | 0.098 µs/op | **1.09x** | 2413 | 1971 |

The benchmark that is likely closest to average Godot game code is think_5, about 2x GDScript performance.

## Installing the addon

Extract `godot-jit.zip` into your Godot 4.6+ project directory, so the extension
is at `addons/godot_jit/`, then restart the editor.

Release archives include Android ARM64/x86-64 (API 24+), Linux ARM64/x86-64,
macOS Apple Silicon/Intel, and Windows x86-64 binaries.

## Building for Android

With Android NDK r24 installed:

```sh
export ANDROID_NDK_ROOT=/path/to/android-ndk-r24
bash scripts/ci/build_android.sh arm64
bash scripts/ci/build_android.sh x86_64
```

I don't know, if they're locking down Android it's dead to me.

## Game studios

Game studios will want the Godot Sandbox Plus module. See: https://plus.libriscv.no/

## License

Copyright (C) 2026 Alf-André Walla.

Godot JIT is licensed under the GNU Lesser General Public License version 3
(`LGPL-3.0-only`). See [LICENSE](LICENSE), [COPYING.LESSER](COPYING.LESSER),
and [COPYING](COPYING). Third-party components retain their respective licenses.
