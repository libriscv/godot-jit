extends SceneTree

const SOURCE = """extends RefCounted
var speed: float = 1.0
var saved: Array = [Vector2(3, 4)]
func typed_integer(value: int) -> int:
    return value + 7
func internal_integer(value: float) -> int:
    return typed_integer(value)
func nullable(value: Object) -> bool:
    return value == null
func vector(value: Vector2) -> Vector2:
    value.x += 2.0
    return value.normalized()
func nested(value: Vector2, values: Array) -> Vector2:
    return vector(value) + values[0]
func first(values: Array) -> Vector2:
    return values[0]
func local_typed(value):
    var vector: Vector2 = value
    return vector
func arrow_basis(direction: Vector3) -> Basis:
    var basis := Basis()
    basis.y = direction.normalized()
    var axis := Vector3(1, 0, 0)
    if abs(axis.dot(basis.y)) > 0.9:
        axis = Vector3(0, 1, 0)
    basis.x = basis.y.cross(axis).normalized()
    basis.z = basis.x.cross(basis.y).normalized()
    return basis
func select(values: Array, other: Array, flag: bool) -> Array:
    var result: Array
    if flag:
        result = values
    else:
        result = other
    return result
func loop(values: Array, count: int):
    var result = values
    for i in range(count):
        var copy = result
        if i & 1:
            result = copy
        else:
            result = values
    return result
func observed(value):
    return [value.length(), value.x]
func inline_values(a: Rect2i, b: Color, c: Quaternion, d: RID):
    var rect := a
    var color := b
    color.g += 0.25
    return [rect, color, c, d, rect.position]
func booleans(a: bool, b: bool):
    return [a and b, not (a or b), typeof(a), a is bool]
func callback(cb: Callable, value: int):
    return cb.call(value)
func callbackv(cb: Callable, values: Array):
    return cb.callv(values)
func bind_callback(cb: Callable, value: int):
    return cb.bind(value).call()
func replace():
    saved = [Vector2(50, 60)]
func reenter(values: Array, cb: Callable) -> Vector2:
    cb.call()
    return values[0]
func member_reenter() -> Vector2:
    return reenter(saved, replace)
func read_speed() -> float:
    return speed
func owner_after_callback(cb: Callable) -> bool:
    cb.call()
    return self != null
"""
var failures := 0
var retained: Variant

func check(ok: bool, message: String) -> void:
    if not ok:
        failures += 1
        push_error(message)

func release_owner() -> void:
    retained = null

func _initialize() -> void:
    var scripts: Array[Script] = [GDScript.new(), UnsafeGDScript.new()]
    for script in scripts:
        script.source_code = SOURCE
        check(script.reload() == OK, "Baseline soundness source compiles")
    if failures:
        quit(1)
        return
    for script in scripts:
        var receiver = script.new()
        check(receiver.call("typed_integer", 3.9) == 10, "External float argument coerces to int")
        check(receiver.internal_integer(3.9) == 10, "Local float argument coerces to int")
        check(receiver.nullable(null), "Null remains valid for Object parameters")
        check(not receiver.nullable(receiver), "Object argument remains non-null")
        var input := Vector2(1, 4)
        check(receiver.vector(input) == Vector2(3, 4).normalized(), "Typed vector setter and normalization")
        check(input == Vector2(1, 4), "Vector parameter is a value")
        check(receiver.nested(input, [Vector2(5, 6)]) == Vector2(3, 4).normalized() + Vector2(5, 6), "Typed local vector call")
        check(receiver.first([Vector2i(3, 4)]) == Vector2(3, 4), "Unknown return is coerced before typed ABI")
        check(receiver.local_typed(Vector2i(3, 4)) == Vector2(3, 4), "Declared local produces a proven Vector2")
        check(receiver.arrow_basis(Vector3(0, -1, 0)) == Basis(Vector3(0, 0, 1), Vector3(0, -1, 0), Vector3(1, 0, 0)), "Mixed parameter/return storage preserves normalized vectors")
        var watched := RefCounted.new()
        var before := watched.get_reference_count()
        var values = [watched]
        for flag in [false, true]:
            check(receiver.select(values, values, flag) == values, "Branch join preserves owned Array")
        check(receiver.loop(values, 10) == values, "Loop backedge preserves owned Array")
        values.clear()
        check(watched.get_reference_count() == before, "Array moves balance reference counts")
        check(receiver.observed(Vector2(3, 4)) == [5.0, 3.0], "Observed Vector2 cached method/getter")
        check(receiver.observed(Vector3(0, 0, 2)) == [2.0, 0.0], "Same call site changes built-in type")
        check(receiver.inline_values(Rect2i(1, 2, 3, 4), Color(0.25, 0.25, 0.5), Quaternion.IDENTITY, RID()) == [Rect2i(1, 2, 3, 4), Color(0.25, 0.5, 0.5), Quaternion.IDENTITY, RID(), Vector2i(1, 2)], "Inline integer, color, quaternion and RID payloads survive boxing")
        for a in [false, true]:
            for b in [false, true]:
                check(receiver.booleans(a, b) == [a and b, not (a or b), TYPE_BOOL, true], "Boolean branches retain proven values")
        check(receiver.callback(receiver.typed_integer, 3) == 10, "Callable ptrcall result")
        check(receiver.callbackv(receiver.typed_integer, [3]) == 10, "Callable callv ptrcall")
        check(receiver.bind_callback(receiver.typed_integer, 3) == 10, "Bound Callable ownership")
        check(receiver.member_reenter() == Vector2(3, 4), "Reentry cannot invalidate argument snapshot")
        receiver.set("speed", 8)
        check(receiver.read_speed() == 8.0 and typeof(receiver.read_speed()) == TYPE_FLOAT, "Object.set validates typed member")
        retained = script.new()
        check(retained.owner_after_callback(release_owner), "RefCounted self survives callback dropping last external owner")
    var host := GodotJIT.new()
    check(host.compile_sgd(SOURCE), "Compile standalone soundness host")
    host.execute_function("first", [["invalid vector"]])
    check(not host.get_error().is_empty(), "Invalid dynamic typed return is rejected")
    print("Baseline soundness: %d failures" % failures)
    quit(1 if failures else 0)
