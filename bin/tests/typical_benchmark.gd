extends SceneTree

# Read the actual fixture; diagnostic helpers are appended identically to both languages.
const SOURCE_PATH = "res://tests/typical.ugd"
const DELTA = 1.0 / 60.0
const ORIGIN = Vector2(320, 240)
const WORKLOADS = ["think_5", "minigame", "call_floor", "nearest_0", "nearest_5",
    "iterate_5", "distance_5", "normalize", "batch_think", "batch_nearest",
    "batch_iterate", "batch_normalize", "batch_floor"]
const BATCH_DRIVER = """
func %s(count: int, points: Array, me: Vector2, hazards: Array) -> Vector2:
    var result := Vector2.ZERO
    for i in range(count):
        result += %s
    return result
"""
const DIAGNOSTICS = """
func bench_noop(delta: float, me: Vector2, crystals: Array, hazards: Array) -> Vector2:
    return me
func bench_iterate(points: Array, me: Vector2) -> Vector2:
    var result := me
    for point in points:
        var candidate: Vector2 = point
        result = candidate
    return result
func bench_distance(points: Array, me: Vector2) -> float:
    var result := 0.0
    for point in points:
        var candidate: Vector2 = point
        result += me.distance_squared_to(candidate)
    return result
func bench_normalize(me: Vector2) -> Vector2:
    return (Vector2(80, 60) - me).normalized()
"""
var failures := 0

func check(ok: bool, message: String) -> void:
    if not ok:
        failures += 1
        push_error(message)

func make_world() -> Dictionary:
    var rng := RandomNumberGenerator.new()
    rng.seed = 1234567
    return {"me": ORIGIN, "crystals": [ORIGIN + Vector2(80, 0), Vector2(50, 50),
        Vector2(590, 50), Vector2(590, 430), Vector2(50, 430)],
        "hazards": [Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO],
        "rng": rng, "collected": 0, "hits": 0, "score": 0}

func update_hazards(world: Dictionary, frame: int) -> void:
    for j in range(5):
        world.hazards[j] = ORIGIN + Vector2(40 + 28 * j, 0).rotated(frame * DELTA * 0.7 + j * TAU / 5)

func advance(world: Dictionary, direction: Vector2) -> void:
    world.me += direction * (120.0 * DELTA)
    for j in range(5):
        if world.me.distance_squared_to(world.crystals[j]) <= 12.0 * 12.0:
            world.collected += 1
            world.score += 10
            world.crystals[j] = Vector2(world.rng.randf_range(20, 620), world.rng.randf_range(20, 460))
    for hazard in world.hazards:
        if world.me.distance_squared_to(hazard) <= 10.0 * 10.0:
            world.hits += 1
            world.score -= 5
            world.me = ORIGIN
            break

func snapshot(world: Dictionary) -> Array:
    return [world.me, world.crystals.duplicate(), world.hazards.duplicate(),
        world.collected, world.hits, world.score, world.rng.state]

func verify(gd, native) -> void:
    # Independent expected answers cover empty input, overlap, ties, negatives and ordering.
    var cases = [
        [[], Vector2(2, 3), Vector2(2, 3)],
        [[Vector2(2, 3)], Vector2(2, 3), Vector2(2, 3)],
        [[Vector2(3, 4)], Vector2.ZERO, Vector2(3, 4)],
        [[Vector2(1, 0), Vector2(-1, 0)], Vector2.ZERO, Vector2(1, 0)],
        [[Vector2(100, 100), Vector2(-2, -3), Vector2(8, 9), Vector2(-1, -1), Vector2(40, 40)], Vector2.ZERO, Vector2(-1, -1)]
    ]
    for data in cases:
        for receiver in [gd, native]:
            check(receiver.nearest(data[0], data[1]) == data[2], "nearest expected answer")
            var direction: Variant = receiver.think(DELTA, data[1], data[0], make_world().hazards)
            check(typeof(direction) == TYPE_VECTOR2 and direction.is_equal_approx((data[2] - data[1]).normalized()), "think expected direction")
    var a = make_world()
    var b = make_world()
    # Compare every frame before advancing, including respawns and randomized replacement crystals.
    for frame in range(4096):
        update_hazards(a, frame)
        update_hazards(b, frame)
        var expected: Variant = gd.think(DELTA, a.me, a.crystals, a.hazards)
        var actual: Variant = native.think(DELTA, b.me, b.crystals, b.hazards)
        check(typeof(actual) == TYPE_VECTOR2 and actual == expected, "think parity at frame %d" % frame)
        if failures:
            return
        advance(a, expected)
        advance(b, actual)
        check(snapshot(a) == snapshot(b), "world parity at frame %d" % frame)
        if failures:
            return
    check(a.collected > 0 and a.hits > 0, "Simulation must exercise collection and hazard respawn")
    for method in WORKLOADS:
        var expected: Variant = run_workload(gd, method, 32, make_world())
        var actual: Variant = run_workload(native, method, 32, make_world())
        check(typeof(actual) == typeof(expected) and actual == expected, "Diagnostic parity: " + method)
    print("Correctness: 4096 matching frames; crystals=%d hazards=%d score=%d" % [a.collected, a.hits, a.score])

func run_workload(receiver, method: String, iterations: int, world: Dictionary) -> Variant:
    var result := Vector2.ZERO
    var distance := 0.0
    var points: Array = world.crystals
    var hazards: Array = world.hazards
    # Dispatch once outside each timed inner loop. All external-call rows share the GD driver.
    match method:
        "minigame":
            for frame in range(iterations):
                update_hazards(world, frame)
                var direction: Vector2 = receiver.think(DELTA, world.me, points, hazards)
                advance(world, direction)
            return snapshot(world)
        "think_5":
            for frame in range(iterations):
                result += receiver.think(DELTA, Vector2(frame & 511, (frame * 7) & 511), points, hazards)
        "call_floor":
            for frame in range(iterations):
                result += receiver.bench_noop(DELTA, Vector2(frame & 511, (frame * 7) & 511), points, hazards)
        "nearest_0", "nearest_5":
            if method == "nearest_0":
                points = []
            for frame in range(iterations):
                result += receiver.nearest(points, Vector2(frame & 511, (frame * 7) & 511))
        "iterate_5":
            for frame in range(iterations):
                result += receiver.bench_iterate(points, Vector2(frame & 511, (frame * 7) & 511))
        "distance_5":
            for frame in range(iterations):
                distance += receiver.bench_distance(points, Vector2(frame & 511, (frame * 7) & 511))
            return distance
        "normalize":
            for frame in range(iterations):
                result += receiver.bench_normalize(Vector2(frame & 511, (frame * 7) & 511))
        _:
            # Appended loops execute inside each language with just one external call.
            return receiver.call(method, iterations, points, ORIGIN, hazards)
    return result

func measure_pair(gd, native, method: String, iterations: int) -> Dictionary:
    var samples = [[], []]
    var reference: Variant
    for repeat in range(8): # One warm-up and seven measured samples; alternate order.
        for index in ([0, 1] if repeat % 2 == 0 else [1, 0]):
            var receiver = gd if index == 0 else native
            var world = make_world()
            var start = Time.get_ticks_usec()
            var result: Variant = run_workload(receiver, method, iterations, world)
            var elapsed = Time.get_ticks_usec() - start
            if repeat == 0 and index == 0:
                reference = result
            check(typeof(result) == typeof(reference) and result == reference, "Unstable/mismatched result: " + method)
            if repeat > 0:
                samples[index].append(elapsed)
    samples[0].sort()
    samples[1].sort()
    var gd_us: int = samples[0][3]
    var native_us: int = samples[1][3]
    print("%s: GD=%.3f us/op JIT=%.3f us/op JIT/GD=%.3f (GD %d..%d us; JIT %d..%d us)" % [
        method, float(gd_us) / iterations, float(native_us) / iterations, float(native_us) / max(1, gd_us),
        samples[0][0], samples[0][-1], samples[1][0], samples[1][-1]])
    return {"workload": method, "iterations": iterations, "gd_us": gd_us, "jit_us": native_us,
        "ratio": float(native_us) / max(1, gd_us), "gd_samples_us": samples[0], "jit_samples_us": samples[1], "checksum": var_to_str(reference)}

func _initialize() -> void:
    var args = OS.get_cmdline_user_args()
    var iterations := 100000
    for arg in args:
        if arg.is_valid_int():
            iterations = max(1, int(arg))
    var source = FileAccess.get_file_as_string(SOURCE_PATH)
    if source.is_empty():
        push_error("Cannot read " + SOURCE_PATH)
        quit(1)
        return
    var fixture_hash = source.sha256_text()
    source += DIAGNOSTICS.replace("    ", "\t")
    # Same drivers in each language isolate local calls from ScriptInstance entry costs.
    for entry in [["batch_think", "think(0.0166666666666667, me, points, hazards)"],
            ["batch_nearest", "nearest(points, me)"], ["batch_iterate", "bench_iterate(points, me)"],
            ["batch_normalize", "bench_normalize(me)"], ["batch_floor", "bench_noop(0.0166666666666667, me, points, hazards)"]]:
        source += BATCH_DRIVER.replace("    ", "\t") % entry
    var gd_script = GDScript.new()
    gd_script.source_code = source
    if gd_script.reload() != OK:
        quit(1)
        return
    var native_script = UnsafeGDScript.new()
    native_script.source_code = source
    if native_script.reload() != OK:
        push_error(native_script.get_compile_error())
        quit(1)
        return
    var gd = gd_script.new()
    var native = native_script.new()
    verify(gd, native)
    if failures or "--check-only" in args:
        quit(1 if failures else 0)
        return
    if "--dump-c" in args:
        var output = FileAccess.open("user://typical_benchmark.c", FileAccess.WRITE)
        output.store_string(native_script.get_generated_c())
        print("Generated C: ", ProjectSettings.globalize_path("user://typical_benchmark.c"))
    print("Typical benchmark: %d iterations, 5 crystals + 5 hazards, 60 Hz simulation; median of 7 after warm-up; alternating order; compilation/setup excluded" % iterations)
    print("Godot: ", Engine.get_version_info().string, "; CPU: ", OS.get_processor_name())
    var results: Array = []
    for method in WORKLOADS:
        results.append(measure_pair(gd, native, method, iterations))
        if failures:
            quit(1)
            return
    var report = FileAccess.open("user://typical_benchmark.json", FileAccess.WRITE)
    report.store_string(JSON.stringify({"godot": Engine.get_version_info().string,
        "cpu": OS.get_processor_name(), "fixture_sha256": fixture_hash,
        "seed": 1234567, "delta": DELTA, "results": results}, "  "))
    print("Results: ", ProjectSettings.globalize_path("user://typical_benchmark.json"))
    quit(1 if failures else 0)
