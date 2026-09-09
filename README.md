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

## Installing the addon

Extract `godot-jit.zip` into your Godot 4.6+ project directory, so the extension
is at `res://addons/godot_jit/godot_jit.gdextension`, then restart the editor.
The archive includes debug and release binaries for Linux x86-64/ARM64,
macOS Intel/Apple Silicon (macOS 11+), and Windows x86-64. Linux binaries are
built on Ubuntu 24.04 and require a compatible glibc. No separate TinyCC or
MinGW installation is needed. RISC-V remains available for local builds.

## CI and draft releases

The **JIT builds** workflow runs on pull requests, pushes to `main`, and manual
dispatches. The Linux, macOS and Windows workflows can also run independently.
Each platform builds both CMake presets and runs the native and headless Godot
tests using Godot 4.6.3 before uploading its libraries.

After all platforms pass, packaging validates every required library and creates
the `godot-jit-addon` workflow artifact containing `godot-jit.zip`. The ZIP starts
at `addons/`; it excludes the test project, build files, and editor cache.
Runs on `main` also create a draft prerelease with that ZIP attached. Each draft
uses the run ID and attempt in its tag and targets the tested commit. Pull
requests and manual runs on other branches only produce workflow artifacts.

CI checks out the recorded submodule commits. Compiler changes in
`godot-sandbox` must be committed and pushed there, and its submodule reference
updated here, before a clean CI checkout can use them.

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

This is a baseline implementation using the SafeGDScript frontend's supported
syntax. Things like `await` is not yet implemented. Editor completion, documentation,
interactive debugging and profiler integration are also not yet implemented.
