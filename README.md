# Godot JIT

A JIT-like backend for the GS SafeGDScript compiler, executed by libtcc
inside a GDExtension.

```gdscript
var jit = GodotJIT.new()
assert(jit.compile_sgd("func answer(n: int) -> int:\n    return n * 2 + 2\n"))
assert(jit.execute_function("answer", [20]) == 42)
```

This is a glue project that re-uses work that already exists by me. The compiler
is from Godot Sandbox, the JIT is my libtcc fork from libriscv and the backend is
more or less the AOT backend from Godot Sandbox Plus. Still work in progress.

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

## Installing the addon

Extract `godot-jit.zip` into your Godot 4.6+ project directory, so the extension
is at `addons/godot_jit/`, then restart the editor.

## Game studios

Game studios will want the Godot Sandbox Plus module. See: https://plus.libriscv.no/

## License

Copyright (C) 2026 Alf-André Walla.

Godot JIT is licensed under the GNU Lesser General Public License version 3
(`LGPL-3.0-only`). See [LICENSE](LICENSE), [COPYING.LESSER](COPYING.LESSER),
and [COPYING](COPYING). Third-party components retain their respective licenses.
