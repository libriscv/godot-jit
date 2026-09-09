# Godot JIT

A JIT-like backend for the GS SafeGDScript compiler, executed by libtcc
inside a GDExtension. This is trusted native code. Values and engine calls use
ordinary C++ Godot Variants, Arrays, Dictionaries and Objects directly.

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

Instances support typed properties, accessors, dynamic property and revert hooks,
signals, default arguments, Callables, nested classes, inheritance, scene
serialization, and state-preserving reloads. Non-tool scripts use editor
placeholders so exported values can be edited without executing the script.

The editor automatically selects UnsafeGDScript syntax highlighting for both
extensions. It follows the editor theme and handles annotations, node references,
StringName literals, documentation comments, and multiline strings.

Running with Godot's debugger enables native debug instrumentation. Editor
breakpoints, `breakpoint` statements, step into/over/out, stack frames,
locals, members, statics, and runtime-error breaks are supported. The debugger
blocks execution while inspecting live native values. Expression inspection
supports names and member paths. Builds compiled without a debugger retain the
optimized C backend without debug hooks.

The language uses the SafeGDScript frontend's supported syntax. `await`, editor
completion, documentation, and profiler integration are not yet implemented.

## Installing the addon

Extract `godot-jit.zip` into your Godot 4.6+ project directory, so the extension
is at `addons/godot_jit/`, then restart the editor.

## Tests

Run `tests/run_tests.sh` to configure and build the debug preset and run the full
CTest suite, including the `.ugd` fixtures, backend, language, editor, debugger,
and benchmark correctness checks. Godot 4.6+ must be on `PATH`, configured in the
preset's existing CMake cache, or specified with `GODOT=/path/to/godot`.
Use `tests/run_tests.sh release` for release builds. Additional arguments go to
CTest, for example `tests/run_tests.sh debug -R godot_jit_ugd -V`.

Add `tests/ugd/test_*.ugd` files to expand the native script suite. CMake discovers
them automatically and registers each file as a separate test. Fixtures extend
`RefCounted`; each zero-argument `test_` method must return a boolean, with `true`
meaning success. Every method gets a fresh instance. Helpers may live alongside
the tests without the `test_` prefix and load via `res://tests/ugd/`. Empty suites,
load failures, false results, and runtime errors fail the test process. Use return
values instead of `assert()` so release builds exercise the same checks.

## License

Copyright (C) 2026 Alf-André Walla.

Godot JIT is licensed under the GNU Lesser General Public License version 3
(`LGPL-3.0-only`). See [LICENSE](LICENSE), [COPYING.LESSER](COPYING.LESSER),
and [COPYING](COPYING). Third-party components retain their respective licenses.
