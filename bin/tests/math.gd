extends SceneTree

var failures := 0

class OtherMath:
    func normalized(): return "custom normalized"
    func length(): return "custom length"
    func length_squared(): return "custom squared"
    func distance_to(_v): return "custom distance"
    func distance_squared_to(_v): return "custom squared distance"
    func dot(_v): return "custom dot"

func check(ok: bool, message: String) -> void:
    if not ok:
        failures += 1
        push_error(message)

func same(a: Variant, b: Variant) -> bool:
    if typeof(a) != typeof(b): return false
    if typeof(a) == TYPE_FLOAT:
        if is_nan(a): return is_nan(b)
        if a != b: return false
        # Check signed zero as well as numerical equality.
        return a != 0.0 or 1.0 / a == 1.0 / b
    if a is Vector2 or a is Vector3 or a is Vector4:
        var count := 2 if a is Vector2 else (3 if a is Vector3 else 4)
        for i in range(count):
            if not same(float(a[i]), float(b[i])): return false
        return true
    return a == b

func compare(gd, native, method: String, args: Array) -> void:
    var expected: Variant = gd.callv(method, args)
    var actual: Variant = native.callv(method, args)
    check(same(actual, expected), "%s(%s): expected %s (%s), got %s (%s)" %
        [method, args, expected, typeof(expected), actual, typeof(actual)])

func _initialize() -> void:
    var unary = ["sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
        "asinh", "acosh", "atanh", "exp", "log", "sqrt", "floorf", "ceilf", "roundf",
        "absf", "signf", "deg_to_rad", "rad_to_deg", "linear_to_db", "db_to_linear",
        "is_nan", "is_inf", "is_finite", "is_zero_approx", "floori", "ceili", "roundi", "step_decimals"]
    var binary = ["atan2", "pow", "fmod", "fposmod", "snappedf", "snappedi", "angle_difference",
        "pingpong", "ease", "is_equal_approx", "minf", "maxf"]
    var ternary = ["lerpf", "lerp", "inverse_lerp", "smoothstep", "move_toward", "lerp_angle", "rotate_toward", "wrapf", "clampf"]
    var wide = ["remap", "cubic_interpolate", "cubic_interpolate_angle", "bezier_interpolate", "bezier_derivative"]
    var widest = ["cubic_interpolate_in_time", "cubic_interpolate_angle_in_time"]
    var source := "extends RefCounted\n"
    var groups = [[unary, "a"], [binary, "a,b"], [ternary, "a,b,c"], [wide, "a,b,c,d,e"], [widest, "a,b,c,d,e,f,g,h"]]
    for group in groups:
        for fn in group[0]:
            source += "func m_%s(%s):\n    return %s(%s)\n" % [fn, group[1], fn, group[1]]
    for fn in ["abs", "sign", "floor", "ceil", "round", "absi", "signi"]:
        source += "func m_%s(a):\n    return %s(a)\n" % [fn, fn]
    for fn in ["min", "max", "mini", "maxi"]:
        source += "func m_%s(a,b):\n    return %s(a,b)\n" % [fn, fn]
    for fn in ["clamp", "clampi"]:
        source += "func m_%s(a,b,c):\n    return %s(a,b,c)\n" % [fn, fn]
    for fn in ["normalized", "length", "length_squared"]:
        source += "func v_%s(v):\n    v = v.%s()\n    return v\n" % [fn, fn]
    for fn in ["distance_to", "distance_squared_to", "dot"]:
        source += "func v_%s(v,w):\n    v = v.%s(w)\n    return v\n" % [fn, fn]
    source += """
func bench_primitives(count: int) -> float:
    var value: float = 0.25
    for i in range(count):
        value = clampf(absf(value) - 0.01, 0.0, 1.0)
    return value
func bench_engine(count: int) -> float:
    var value: float = 0.25
    for i in range(count):
        value = sin(value) + log(1.0 + absf(value))
    return value
func constant_engine():
    return sin(0.25) + log(2.0)
"""
    var gd_script = GDScript.new()
    gd_script.source_code = source
    check(gd_script.reload() == OK, "GDScript math fixture compiles")
    var native_script = UnsafeGDScript.new()
    native_script.source_code = source
    check(native_script.reload() == OK, "Native math fixture compiles: " + native_script.get_compile_error())
    if failures:
        quit(1)
        return
    var gd = gd_script.new()
    var native = native_script.new()
    for fn in unary:
        for x in [-3.5, -0.5, -0.0, 0.0, 0.25, 1.0, 3.5, 1e-30, 1e30]:
            compare(gd, native, "m_" + fn, [x])
        if fn not in ["floori", "ceili", "roundi", "step_decimals"]:
            for x in [INF, -INF, NAN]: compare(gd, native, "m_" + fn, [x])
    for group in [[binary, 2], [ternary, 3], [wide, 5], [widest, 8]]:
        for fn in group[0]:
            for values in [[0.25, 2.25, 0.75, -1.5, 1.25, 0.25, -0.5, 2.0], [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]]:
                compare(gd, native, "m_" + fn, values.slice(0, group[1]))
    for fn in ["abs", "sign", "floor", "ceil", "round"]:
        for x in [-3, 0, 3, -0.0, -0.5, 0.5, INF, NAN, Vector2(-0.5, 2.5)]:
            compare(gd, native, "m_" + fn, [x])
    for fn in ["absi", "signi"]:
        for x in [-9223372036854775807 - 1, -3, 0, 3, 9223372036854775807]:
            compare(gd, native, "m_" + fn, [x])
    for fn in ["min", "max", "mini", "maxi"]:
        for pair in [[9007199254740993, 9007199254740994], [-3, 2], [3, -2]]:
            compare(gd, native, "m_" + fn, pair)
    for fn in ["min", "max"]:
        for pair in [[1, 2.0], [2.0, 1], [-0.0, 0.0], [NAN, 1.0], [1.0, NAN]]:
            compare(gd, native, "m_" + fn, pair)
    for fn in ["minf", "maxf", "clampf"]:
        for x in [-0.0, 0.0, NAN, INF, -INF]:
            compare(gd, native, "m_" + fn, [x, 0.0, 1.0] if fn == "clampf" else [x, 0.0])
    for fn in ["clamp", "clampi"]:
        compare(gd, native, "m_" + fn, [9007199254740994, 9007199254740993, 9007199254740995])
    for v in [Vector2.ZERO, Vector3.ZERO, Vector4.ZERO, Vector2(3, 4), Vector3(1, -2, 3), Vector4(1, -2, 3, -4),
            Vector2(1e-30, -1e-30), Vector3(1e-30, -1e-30, 0), Vector4(1e-30, -1e-30, 0, 0),
            Vector2(1e20, -1e20), Vector3(1e20, -1e20, 0), Vector4(1e20, -1e20, 0, 0),
            Vector2(INF, NAN), Vector3(INF, NAN, 0), Vector4(INF, NAN, 0, 0)]:
        for fn in ["normalized", "length", "length_squared"]:
            compare(gd, native, "v_" + fn, [v])
        for fn in ["distance_to", "distance_squared_to", "dot"]:
            compare(gd, native, "v_" + fn, [v, -v])
    var rng = RandomNumberGenerator.new()
    rng.seed = 7091
    for sample in range(128):
        var v = Vector4(rng.randf_range(-100, 100), rng.randf_range(-100, 100), rng.randf_range(-100, 100), rng.randf_range(-100, 100))
        for value in [Vector2(v.x, v.y), Vector3(v.x, v.y, v.z), v]:
            for fn in ["normalized", "length", "length_squared"]:
                compare(gd, native, "v_" + fn, [value])
            for fn in ["distance_to", "distance_squared_to", "dot"]:
                compare(gd, native, "v_" + fn, [value, -value])
    var custom = OtherMath.new()
    for fn in ["normalized", "length", "length_squared"]:
        compare(gd, native, "v_" + fn, [custom])
    for fn in ["distance_to", "distance_squared_to", "dot"]:
        compare(gd, native, "v_" + fn, [custom, 0])
    compare(gd, native, "constant_engine", [])
    compare(gd, native, "bench_primitives", [100])
    compare(gd, native, "bench_engine", [100])
    print("Math parity: ", failures, " failures")
    if not failures and "--benchmark" in OS.get_cmdline_user_args():
        benchmark(gd, native)
    quit(1 if failures else 0)

func benchmark(gd, native) -> void:
    var iterations := 1000000
    for arg in OS.get_cmdline_user_args():
        if arg.is_valid_int(): iterations = max(1, int(arg))
    for method in ["bench_primitives", "bench_engine"]:
        var samples = [[], []]
        var instructions = [[], []]
        var count_instructions = OS.get_environment("GODOT_JIT_INSTRUCTIONS") == "1"
        gd.call(method, 1000)
        native.call(method, 1000)
        for trial in range(7):
            for k in range(2):
                var which := (trial + k) % 2
                var receiver = gd if which == 0 else native
                var before = GodotJIT.get_instruction_count() if count_instructions else -1
                var start := Time.get_ticks_usec()
                receiver.call(method, iterations)
                samples[which].append(Time.get_ticks_usec() - start)
                var after = GodotJIT.get_instruction_count() if count_instructions else -1
                instructions[which].append(after - before if before >= 0 and after >= before else -1)
        samples[0].sort()
        samples[1].sort()
        print("%s: GD=%.6f us/op JIT=%.6f us/op speedup=%.2fx" % [method,
            float(samples[0][3]) / iterations, float(samples[1][3]) / iterations, float(samples[0][3]) / samples[1][3]])
        instructions[0].sort()
        instructions[1].sort()
        if count_instructions:
            print("%s instructions: GD=%.3f JIT=%.3f insn/op" % [method,
                float(instructions[0][3]) / iterations if instructions[0][0] >= 0 else -1.0,
                float(instructions[1][3]) / iterations if instructions[1][0] >= 0 else -1.0])
