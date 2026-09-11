# Godot JIT

A JIT-like backend for the GS SafeGDScript compiler, executed by libtcc
inside a GDExtension.

This is a glue project that re-uses work that already exists by me. The compiler
is from Godot Sandbox, the JIT is my libtcc fork from libriscv and the backend is
more or less the AOT backend from Godot Sandbox Plus. Highly experimental.

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

Benchmark of game-like code:
```
+----------------------+------------------+------------------+-----------+
| Workload             |   GDScript us/op |  godot-jit us/op |   Speedup |
+----------------------+------------------+------------------+-----------+
| think_5              |            0.381 |            0.228 |    1.672x |
| minigame             |            1.968 |            1.863 |    1.056x |
| call_floor           |            0.113 |            0.110 |    1.029x |
| nearest_0            |            0.132 |            0.133 |    0.988x |
| nearest_5            |            0.281 |            0.187 |    1.504x |
| iterate_5            |            0.197 |            0.175 |    1.122x |
| distance_5           |            0.237 |            0.196 |    1.214x |
| normalize            |            0.107 |            0.116 |    0.928x |
| batch_think          |            0.333 |            0.129 |    2.584x |
| batch_nearest        |            0.239 |            0.093 |    2.564x |
| batch_iterate        |            0.158 |            0.087 |    1.815x |
| batch_normalize      |            0.075 |            0.032 |    2.370x |
| batch_floor          |            0.080 |            0.018 |    4.367x |
+----------------------+------------------+------------------+-----------+
```

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
