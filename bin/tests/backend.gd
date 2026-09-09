extends SceneTree

var failures := 0

class BuiltinNames:
	extends RefCounted
	func size():
		return "custom size"
	func normalized():
		return "custom normalized"
	func distance_squared_to(_v):
		return "custom distance"

func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		push_error(message)

func run(jit: GodotJIT, name: String, args: Array, expected: Variant) -> void:
	var actual: Variant = jit.execute_function(name, args)
	check(jit.get_error().is_empty(), name + ": " + jit.get_error())
	check(actual == expected, name + ": expected " + str(expected) + ", got " + str(actual))

func check_walk_ownership(jit: GodotJIT, watched: RefCounted) -> void:
	# Keep GDScript's temporary nested Array in this frame, so it is released
	# before the caller checks reference counts.
	run(jit, "walk_last", [[watched]], watched)

func _initialize() -> void:
	var jit := GodotJIT.new()
	var receiver := Node.new()
	receiver.name = "Receiver"
	root.add_child(receiver)
	var child := Node.new()
	child.name = "Child"
	receiver.add_child(child)
	var source := """
var counter: int = 7
var greeting: String = "hello"
var shared: Array = [1, 2]
func answer(value: int) -> int:
    return value * 2 + 2
func fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
func loop(n: int) -> int:
    var total: int = 0
    for i in range(n):
        if i == 3:
            continue
        total += i
        if i == 6:
            break
    return total
func dynamic(a, b):
    return a + b
func change_type():
    var value: Variant = "owned string"
    value = 42
    return value
func logic(a, b):
    return not a or b
func kinds(value):
    return [typeof(value), value is int, value is String]
func arrays(a: Array):
    a.append(3)
    a[0] = 9
    var sum: int = 0
    for v in a:
        sum += int(v)
    return sum
func strings(s: String):
    var result: String = ""
    for c in s:
        result += c
    return [result, s.length(), s[-1], s.to_upper()]
func dictionaries(d: Dictionary):
    d["answer"] = 42
    var keys = d.keys()
    return [d["answer"], d.has("answer"), d.get("missing", 7), keys.size()]
func dict_literal():
    var d = {"x": 2, "y": 3}
    var sum: int = 0
    for key in d:
        sum += int(d[key])
    return sum
func vector():
    var v = Vector3(1.0, 2.0, 3.0)
    v.x = 4.0
    return v + Vector3(1.0, 1.0, 1.0)
func packed():
    var a = PackedInt32Array([1, 2, 3])
    a[1] = 7
    return a
func globals():
    counter += 1
    shared.append(counter)
    return [counter, greeting, shared.size()]
func object_arg(obj):
    obj.name = "Changed"
    obj.set_meta("answer", 42)
    return obj.get_meta("answer")
func node_lookup():
    return get_node("Child").name
func make_object():
    return RefCounted.new()
func math(x: float):
    return [sqrt(x), sin(0.0), abs(-3), clamp(12, 1, 8), int(3.9), str("v", 2)]
func random_repeat():
    seed(123)
    var a = randi()
    seed(123)
    return a == randi()
func big(value: int):
    return value + 1
func quotient(a: int, b: int):
    return a / b
func remainder(a: int, b: int):
    return a % b
func many(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int, i: int):
    return a + b + c + d + e + f + g + h + i
func many_internal():
    return many(1, 2, 3, 4, 5, 6, 7, 8, 9)
func scalar_join(flag: bool):
    var value: Variant = 4
    if flag:
        value = 2.5
    return value + 1
func scalar_backedge(n: int):
    var value: Variant = 1
    for i in range(n):
        value = value + 0.5
    return value
func scalar_branches(n: int):
    var total: int = 0
    for i in range(n):
        if i & 1:
            total += i
        else:
            total -= i
    return [total, not total, typeof(total), total is int]
func scalar_unary(n: int):
    var value: int = n
    return [-value, ~value, value << 65, value >> 65, float(value), bool(value)]
func changing_loop(n: int):
    var value: Variant = "start"
    for i in range(n):
        if i & 1:
            value = i
        else:
            value = str(i)
    return [value, typeof(value)]
func vector_snapshot(value):
    var before = value
    value.x = 9.0
    return [before, value]
func vector_alias(value):
    value += value
    return value
func ownership_transitions(obj):
    var value: Variant = obj
    value = Vector2(1.0, 2.0)
    value = "owned"
    value = Vector3(3.0, 4.0, 5.0)
    value = [obj]
    value = Vector4(6.0, 7.0, 8.0, 9.0)
    return value
func unused_error(n: int):
    var unused = 7 / n
    return 1
func ref_error(v):
    var owned = [v]
    var empty: Array = []
    return empty[99]
func invalid_index(a: Array):
    var owned = ["temporary", "values"]
    return a[99]
func builtin_size(v):
    return v.size()
func scalar_comparisons(v):
    var k: int = 9007199254740993
    return [v < k, v == k, v > k, k < v, k == v, k > v]
func builtin_distance(v, w):
    return v.distance_squared_to(w)
func builtin_normalized(v):
    v = v.normalized()
    return v
func last_element(v):
    v = v[-1]
    return v
func mutate_walk(a: Array, grow: bool):
    var visited: Array = []
    for v in a:
        visited.append(v)
        if visited.size() == 1:
            if grow:
                a.append(3)
            else:
                a.pop_back()
    return visited
func walk_last(a: Array):
    var last: Variant = null
    for v in a:
        last = v
    return last
func nested_error():
    var owned = ["caller", "values"]
    return invalid_index([])
"""
	if not jit.compile_sgd(source, receiver):
		push_error(jit.get_error())
		quit(1)
		return
	FileAccess.open("res://generated_backend.c", FileAccess.WRITE).store_string(jit.get_generated_c())
	check(jit.get_generated_c().contains("GJVariant r"), "Generated code must use C locals")
	run(jit, "answer", [20], 42)
	run(jit, "fib", [10], 55)
	run(jit, "loop", [10], 18)
	run(jit, "dynamic", ["a", "b"], "ab")
	run(jit, "dynamic", [2, 3.5], 5.5)
	run(jit, "change_type", [], 42)
	run(jit, "logic", [0, false], true)
	run(jit, "kinds", [42], [TYPE_INT, true, false])
	var a := [1, 2]
	run(jit, "arrays", [a], 14)
	check(a == [9, 2, 3], "Array mutation must affect the original native Array")
	run(jit, "strings", ["hé🙂"], ["hé🙂", 3, "🙂", "HÉ🙂"])
	var d := {}
	run(jit, "dictionaries", [d], [42, true, 7, 1])
	check(d == {"answer": 42}, "Dictionary mutation must affect the original native Dictionary")
	run(jit, "dict_literal", [], 5)
	run(jit, "vector", [], Vector3(5, 3, 4))
	run(jit, "packed", [], PackedInt32Array([1, 7, 3]))
	for container in [[], [1, 2], {"a": 1}, PackedInt32Array([1, 2, 3])]:
		run(jit, "builtin_size", [container], container.size())
	var custom := BuiltinNames.new()
	run(jit, "builtin_size", [custom], "custom size")
	run(jit, "builtin_distance", [custom, 0], "custom distance")
	run(jit, "builtin_normalized", [custom], "custom normalized")
	for v in [9007199254740992, 9007199254740993, 9007199254740994, 0.5]:
		var k: int = 9007199254740993
		run(jit, "scalar_comparisons", [v], [v < k, v == k, v > k, k < v, k == v, k > v])
	for v in [Vector2.ZERO, Vector2(3, 4), Vector2(-0.25, 1.75), Vector2(1e-30, -1e-30), Vector2(1e20, -1e20), Vector3(1, 2, 3)]:
		run(jit, "builtin_distance", [v, -v], v.distance_squared_to(-v))
		run(jit, "builtin_normalized", [v], v.normalized())
	for v in ["owned", [1, 2], {"a": 3}, Vector2(1, 2), RefCounted.new()]:
		run(jit, "last_element", [[0, v]], v)
		run(jit, "walk_last", [[0, v]], v)
	run(jit, "last_element", [PackedInt32Array([1, 2])], 2)
	run(jit, "mutate_walk", [[1, 2], true], [1, 2, 3])
	run(jit, "mutate_walk", [[1, 2], false], [1])
	run(jit, "globals", [], [8, "hello", 3])
	run(jit, "globals", [], [9, "hello", 4])
	run(jit, "object_arg", [receiver], 42)
	check(receiver.name == "Changed" and receiver.get_meta("answer") == 42, "Object pointer must be direct")
	run(jit, "node_lookup", [], &"Child")
	var obj: Variant = jit.execute_function("make_object")
	check(obj is RefCounted, "Native object construction")
	run(jit, "math", [9.0], [3.0, 0.0, 3, 8, 3, "v2"])
	run(jit, "random_repeat", [], true)
	run(jit, "big", [9223372036854775807], -9223372036854775808)
	run(jit, "quotient", [-9223372036854775808, -1], -9223372036854775808)
	run(jit, "remainder", [-9223372036854775808, -1], 0)
	run(jit, "many", [1, 2, 3, 4, 5, 6, 7, 8, 9], 45)
	run(jit, "many_internal", [], 45)
	run(jit, "scalar_join", [false], 5)
	run(jit, "scalar_join", [true], 3.5)
	run(jit, "scalar_backedge", [0], 1)
	run(jit, "scalar_backedge", [5], 3.5)
	run(jit, "scalar_branches", [6], [3, false, TYPE_INT, true])
	run(jit, "scalar_unary", [3], [-3, -4, 6, 1, 3.0, true])
	run(jit, "changing_loop", [5], ["4", TYPE_STRING])
	run(jit, "changing_loop", [6], [5, TYPE_INT])
	run(jit, "vector_snapshot", [Vector2(1, 2)], [Vector2(1, 2), Vector2(9, 2)])
	for value in [Vector2(1, 2), Vector3(1, 2, 3), Vector4(1, 2, 3, 4)]:
		run(jit, "vector_alias", [value], value + value)
	var watched := RefCounted.new()
	var reference_count := watched.get_reference_count()
	for iteration in range(32):
		check_walk_ownership(jit, watched)
		run(jit, "ownership_transitions", [watched], Vector4(6, 7, 8, 9))
		jit.execute_function("ref_error", [watched])
		check(not jit.get_error().is_empty(), "Reference error path must fail")
	check(watched.get_reference_count() == reference_count, "Error cleanup leaked a RefCounted reference")
	jit.execute_function("unused_error", [0])
	check(not jit.get_error().is_empty(), "Unused division must retain its diagnostic")
	jit.execute_function("quotient", [7, 0])
	check(not jit.get_error().is_empty(), "Division by zero must be diagnosed")
	jit.execute_function("nested_error")
	check(not jit.get_error().is_empty(), "Nested host errors must propagate")
	jit.execute_function("answer", ["bad"])
	check(not jit.get_error().is_empty(), "Typed argument rejection must survive exact-call fast path")
	jit.execute_function("answer", [])
	check(not jit.get_error().is_empty(), "Wrong arity must be diagnosed")
	jit.execute_function("missing")
	check(not jit.get_error().is_empty(), "Missing function must be diagnosed")
	check(not jit.compile_sgd("func broken("), "Invalid SGD must fail")
	run(jit, "answer", [20], 42)
	# Owned return values survive code replacement and module destruction.
	var retained: Variant = jit.execute_function("packed")
	jit.clear()
	check(retained == PackedInt32Array([1, 7, 3]), "Returned Variant lifetime")
	check(is_instance_valid(obj), "Returned RefCounted lifetime")
	receiver.free()
	print("C backend and native ABI tests: ", failures, " failures")
	quit(1 if failures else 0)
