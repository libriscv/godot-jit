extends SceneTree

var failures := 0
func check(condition: bool, message: String) -> void:
    if not condition:
        failures += 1
        push_error(message)

func compile_source(source: String) -> Script:
    var script = UnsafeGDScript.new()
    script.source_code = source
    var error = script.reload()
    check(error == OK, "UnsafeGDScript compilation: " + script.get_compile_error())
    return script if error == OK else null

func _initialize() -> void:
    call_deferred("run_checks")

func run_checks() -> void:
    var source = """extends RefCounted
signal changed(value: int)
const ANSWER = 42
enum Mode { FIRST, SECOND }
var total: int = 0
var values: Array = []
static var shared: int = 0
@export var multiplier: int = 2
var bounded: int = 0:
    set(value):
        bounded = clampi(value, 0, 10)
    get:
        return bounded
func _init(start: int = 3):
    total = start
func increment(amount: int = 1) -> int:
    total += amount
    return total
func twice(amount: int) -> int:
    return increment(amount) + increment(amount)
func recurse(n: int) -> int:
    if n <= 0:
        return total
    return recurse(n - 1) + 1
func invoke_other(other, amount: int) -> int:
    return other.increment(amount)
func callback() -> Callable:
    return increment
func closure(bias: int) -> Callable:
    return func(value: int) -> int: return increment(value) + bias
func drive(cb: Callable, n: int) -> int:
    var answer = 0
    for i in range(n):
        answer += cb.call(1)
    return answer
func converted_int(value: int) -> int:
    return value
func converted_float(value: float = 2.5) -> float:
    return value
func announce(value: int):
    changed.emit(value)
static func shared_add(value: int = 1) -> int:
    shared += value
    return shared
func _to_string() -> String:
    return "unsafe:" + str(total)
"""
    var script = compile_source(source)
    if script == null:
        quit(1)
        return
    check(script.get_instance_base_type() == &"RefCounted", "Native base type")
    check(script.get_script_method_list().any(func(m): return m.name == "increment" and m.default_args == [1]), "Method defaults metadata")
    check(script.get_script_signal_list().size() == 1, "Signal metadata")
    check(script.get_script_property_list().any(func(p): return p.name == "multiplier" and p.type == TYPE_INT), "Property metadata")
    var a = script.new(10)
    var b = script.new()
    check(a != null and b != null, "Script.new()")
    if a == null or b == null:
        quit(1)
        return
    check(a.get_method_list().any(func(m): return m.name == "increment" and m.default_args == [1]), "Native method list ABI")
    check(a.get_property_list().any(func(p): return p.name == "bounded"), "Native property list ABI")
    check(a.get_script() == script, "get_script identity")
    check(a.call("converted_int", 3.75) == 3, "Float arguments retain integer conversion")
    var converted: Variant = a.call("converted_float", 3)
    check(converted == 3.0 and typeof(converted) == TYPE_FLOAT, "Integer arguments retain float conversion")
    check(a.call("converted_float") == 2.5, "Default arguments retain fallback dispatch")
    check(a.total == 10 and b.total == 3, "Constructor and instance isolation")
    a.values.append("a")
    check(b.values.is_empty(), "Mutable initializer isolation")
    check(a.increment() == 11, "Default argument from GDScript")
    check(a.twice(2) == 28, "Local calls")
    check(a.recurse(20) == 35, "Recursion")
    check(a.invoke_other(b, 7) == 10, "UnsafeGDScript to another instance")
    check(a.ANSWER == 42 and a.Mode.SECOND == 1, "Constants and enums")
    a.bounded = 30
    check(a.bounded == 10, "Property accessors")
    var cb = a.callback()
    check(cb == a.callback(), "Method Callable identity")
    check(cb.call(1) == 16, "Generated method Callable")
    check(a.closure(100).call(2) == 118, "Capturing lambda")
    check(a.drive(cb, 3) == 60, "Calls through a Callable in a loop")
    var observed = []
    a.changed.connect(func(value): observed.append(value))
    a.announce(91)
    check(observed == [91], "Synchronous signal and GDScript callback")
    check(script.shared_add() == 1 and b.shared_add(4) == 5 and a.shared == 5, "Shared statics and static methods")
    check(str(a) == "unsafe:21", "String conversion")
    var dynamic_script = compile_source("""extends RefCounted
var stored: int = 12
func _get(property: StringName):
    if property == &"dynamic_value":
        return stored
    return null
func _set(property: StringName, value) -> bool:
    if property == &"dynamic_value":
        stored = value
        return true
    return false
func _get_property_list() -> Array:
    return [{"name": "dynamic_value", "type": TYPE_INT, "usage": PROPERTY_USAGE_DEFAULT}]
func _property_can_revert(property: StringName) -> bool:
    return property == &"dynamic_value"
func _property_get_revert(property: StringName):
    return 12
""")
    if dynamic_script:
        var dynamic = dynamic_script.new()
        check(dynamic.get("dynamic_value") == 12, "Dynamic property getter")
        dynamic.set("dynamic_value", 91)
        check(dynamic.get("dynamic_value") == 91, "Dynamic property setter")
        check(dynamic.get_property_list().any(func(p): return p.name == "dynamic_value" and p.type == TYPE_INT), "Dynamic property list")
        check(dynamic.property_can_revert("dynamic_value") and dynamic.property_get_revert("dynamic_value") == 12, "Dynamic property revert")
    var defaults_script = compile_source("func defaults(a: String = \"hello\", b: Array = [], c: int = 23):\n    return a\n")
    if defaults_script:
        var defaults = defaults_script.new().get_method_list().filter(func(m): return m.name == "defaults")
        check(defaults.size() == 1 and defaults[0].default_args == ["hello", [], 23], "Contiguous mixed method defaults ABI")
    # Match the baseline through the same object call boundary.
    var gd = GDScript.new()
    gd.source_code = source
    check(gd.reload() == OK, "Baseline GDScript compilation")
    var baseline = gd.new(10)
    var native = script.new(10)
    for i in range(100):
        check(baseline.twice(2) == native.twice(2), "Call-heavy parity")
    # Failed compilation leaves the last executable usable.
    script.source_code = "func broken(:\n"
    check(script.reload(true) != OK, "Invalid source rejected")
    check(a.increment() == 22, "Failed reload preserves executable")
    script.source_code = source + "\nvar broken_initializer = divide(0)\nfunc divide(n):\n    return 1 / n\n"
    check(script.reload(true) != OK, "Runtime initializer failure rejects reload")
    check(a.total == 22 and b.total == 10, "Failed initializer restores instance state")
    var reloaded_source = source.replace("return total\nfunc twice", "return total + 1000\nfunc twice")
    check(reloaded_source != source, "Reload fixture changes the source")
    script.source_code = reloaded_source
    check(script.reload(true) == OK, "Reload preserving state")
    check(a.total == 22 and b.total == 10, "Reload preserves each object's members")
    check(a.shared == 5, "Reload preserves statics")
    check(a.increment() == 1023, "Reload switches executable")
    check(not cb.is_valid(), "Old generated callbacks invalidated on reload")
    var node_script = compile_source("""extends Node
var events: Array = []
@onready var child = get_node("Child")
func _init():
    events.append("init")
func _enter_tree():
    events.append("enter")
func _ready():
    events.append(child.name)
func _notification(what: int):
    if what == 10:
        events.append("notification")
""")
    if node_script:
        var node = node_script.new()
        var child = Node.new()
        child.name = "Child"
        node.add_child(child)
        root.add_child(node)
        check(node.events.has("init") and node.events.has("enter") and node.events.has("Child"), "Node lifecycle and onready")
        var packed = PackedScene.new()
        check(packed.pack(node) == OK, "Pack scene with built-in UnsafeGDScript")
        check(ResourceSaver.save(packed, "user://unsafe_scene.tscn") == OK, "Serialize built-in script source")
        var restored_scene = ResourceLoader.load("user://unsafe_scene.tscn", "", ResourceLoader.CACHE_MODE_IGNORE)
        var restored_node = restored_scene.instantiate()
        check(restored_node.get_script() is UnsafeGDScript and restored_node.events == ["init"], "Built-in script round trip")
        restored_node.free()
        node.free()
    var nested_script = compile_source("""extends RefCounted
class Counter:
    var count: int = 0
    func _init(start: int = 1):
        count = start
    func increment(by: int = 1) -> int:
        count += by
        return count
    func callback() -> Callable:
        return increment
    func describe() -> String:
        return super.get_class()
class Child extends Counter:
    func increment(by: int = 1) -> int:
        return super.increment(by) + 100
func check_counter(value) -> bool:
    return value is Counter and not value is Dictionary
func typed_counter(value: Counter) -> int:
    return value.increment()
func make_counter():
    return Counter.new(4)
func make_child():
    return Child.new(7)
func local_test() -> int:
    var counter = Counter.new(3)
    return counter.increment(2)
""")
    if nested_script:
        var factory = nested_script.new()
        check(factory.local_test() == 5, "Nested class local calls")
        var counter = factory.make_counter()
        check(counter is RefCounted and counter.count == 4, "Nested native class construction")
        check(counter.describe() == "RefCounted", "Native super MethodBind dispatch")
        check(counter.increment() == 5, "Nested native method from GDScript")
        check(counter.callback().call(2) == 7, "Nested method Callable")
        check(factory.check_counter(counter), "Nested runtime type checks")
        check(factory.typed_counter(counter) == 8, "Typed nested class argument")
        var derived = factory.make_child()
        check(derived.increment(2) == 109, "Nested inheritance and super")
    var base = compile_source("""extends RefCounted
var amount: int = 1
func _init(value: int = 2):
    amount = value
func step(value: int = 1) -> int:
    amount += value
    return amount
""")
    if base:
        check(ResourceSaver.save(base, "user://base.ugd") == OK, "Save base script")
        var derived_script = compile_source("""extends "user://base.ugd"
func _init(value: int = 5):
    super(value)
func step(value: int = 1) -> int:
    return super.step(value) * 2
""")
        if derived_script:
            var derived = derived_script.new()
            check(derived.step(3) == 16, "File inheritance and super")
            check(derived_script.get_base_script() != null, "Base Script resource")
    var optional_lambda = compile_source("func make():\n    return func(n: int = 4) -> int: return n * 2\n")
    if optional_lambda:
        var instance = optional_lambda.new()
        check(instance.make().call() == 8, "Lambda default arguments")
    var jit = GodotJIT.new()
    check(jit.compile_sgd("func make():\n    return func(n: int = 5): return n * 3\n"), "Standalone runtime with Callable")
    var standalone_callback = jit.execute_function("make")
    check(standalone_callback.call() == 15, "Standalone generated Callable")
    jit.clear()
    check(not standalone_callback.is_valid(), "Standalone Callable invalidation")
    var async_script = UnsafeGDScript.new()
    async_script.source_code = "func suspended(value):\n    return await value\n"
    check(async_script.reload() == OK, "Async compilation: " + async_script.get_compile_error())
    check(async_script.new().suspended(42) == 42, "Immediate await")
    # Resource loader/saver and source serialization.
    check(ResourceSaver.save(script, "user://roundtrip.ugd") == OK, "Resource saver")
    var loaded = ResourceLoader.load("user://roundtrip.ugd", "", ResourceLoader.CACHE_MODE_IGNORE)
    check(loaded != null and loaded.new(5).total == 5, "Resource loader")
    a = null
    check(not cb.is_valid(), "Callable cannot keep its instance alive")
    print("UnsafeGDScript language checks: ", failures, " failures")
    quit(0 if failures == 0 else 1)
