extends SceneTree

# Identical source and inputs; compilation and instance creation are excluded.
const SOURCE = """extends RefCounted
var total: int = 0
func integer_loop(count: int) -> int:
    var result: int = 0
    for i in range(count):
        result += (i * 3) & 255
    return result
func float_loop(count: int) -> float:
    var result: float = 0.0
    for i in range(count):
        result = result * 0.5 + float(i & 255)
    return result
func array_loop(count: int) -> int:
    var values: Array = [1, 2, 3, 4, 5, 6, 7, 8]
    var result: int = 0
    for i in range(count):
        var index: int = i & 7
        values[index] = int(values[index]) + 1
        result += int(values[index])
    return result
func dictionary_loop(count: int) -> int:
    var data: Dictionary = {"health": 100, "damage": 3}
    var result: int = 0
    for i in range(count):
        data.health = int(data.health) - int(data.damage)
        result += int(data.health)
    return result
func vector_loop(count: int) -> float:
    var position: Vector2 = Vector2(1.0, 2.0)
    var velocity: Vector2 = Vector2(0.5, 0.25)
    for i in range(count):
        position += velocity
    return position.x + position.y
func gameplay_loop(count: int) -> float:
    var position: Vector2 = Vector2(1.0, 2.0)
    var velocity: Vector2 = Vector2(0.5, 0.25)
    var cooldown: float = 1.0
    var stats: Dictionary = {"hits": 0}
    var score: int = 0
    for i in range(count):
        position += velocity
        cooldown -= 0.0625
        if cooldown <= 0.0:
            stats.hits = int(stats.hits) + 1
            score += add(i & 7)
            cooldown = 1.0
    return position.x + position.y + float(score) + float(stats.hits)
func add(value: int) -> int:
    total += value
    return total
func combine(value: int) -> int:
    return add(value) + add(value + 1)
func local_calls(count: int) -> int:
    var result: int = 0
    for i in range(count):
        result += combine(i & 7)
    return result
func object_calls(other, count: int) -> int:
    var result: int = 0
    for i in range(count):
        result += other.add(i & 7)
    return result
func callable_calls(callback: Callable, count: int) -> int:
    var result: int = 0
    for i in range(count):
        result += callback.call(i & 7)
    return result
"""

func measure(script: Script, method: String, iterations: int) -> Dictionary:
    var samples: Array[int] = []
    var checksum: Variant = 0
    for repeat in range(6):
        var receiver = script.new()
        var other = script.new()
        var callback = Callable(other, "add")
        var start = Time.get_ticks_usec()
        var result: Variant
        match method:
            "integer_loop", "float_loop", "array_loop", "dictionary_loop", "vector_loop", "gameplay_loop": result = receiver.call(method, iterations)
            "local_calls": result = receiver.local_calls(iterations)
            "object_calls": result = receiver.object_calls(other, iterations)
            "callable_calls": result = receiver.callable_calls(callback, iterations)
        var elapsed = Time.get_ticks_usec() - start
        if repeat > 0:
            samples.append(elapsed)
        if repeat > 0 and (result != checksum or typeof(result) != typeof(checksum)):
            push_error("Unstable result for " + method)
            return {"microseconds": 0, "checksum": null}
        checksum = result
    samples.sort()
    return {"microseconds": samples[samples.size() / 2], "checksum": checksum}

func _initialize() -> void:
    var baseline = GDScript.new()
    baseline.source_code = SOURCE
    if baseline.reload() != OK:
        quit(1)
        return
    var native = UnsafeGDScript.new()
    native.source_code = SOURCE
    if native.reload() != OK:
        push_error(native.get_compile_error())
        quit(1)
        return
    if "--dump-c" in OS.get_cmdline_user_args():
        var output = FileAccess.open("user://benchmark.c", FileAccess.WRITE)
        output.store_string(native.get_generated_c())
        print("Generated C: ", ProjectSettings.globalize_path("user://benchmark.c"))
    var iterations = 100000
    for argument in OS.get_cmdline_user_args():
        if argument.is_valid_int():
            iterations = max(1, int(argument))
    print("Call benchmark: ", iterations, " iterations; median of 5 runs after warm-up; compilation excluded")
    for method in ["integer_loop", "float_loop", "array_loop", "dictionary_loop", "vector_loop", "gameplay_loop", "local_calls", "object_calls", "callable_calls"]:
        var gd = measure(baseline, method, iterations)
        var ugd = measure(native, method, iterations)
        if gd.checksum == null or gd.checksum != ugd.checksum or typeof(gd.checksum) != typeof(ugd.checksum):
            push_error("Checksum mismatch for " + method)
            quit(1)
            return
        print("%s: GDScript=%d us UnsafeGDScript=%d us Unsafe/GD=%.3f checksum=%s" % [method, gd.microseconds, ugd.microseconds, float(ugd.microseconds) / max(1, gd.microseconds), gd.checksum])
    quit()
