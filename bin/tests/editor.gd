extends SceneTree
var failures := 0
func check(ok: bool, message: String):
    if not ok:
        failures += 1
        push_error(message)

func _initialize():
    call_deferred("run_checks")

func run_checks():
    check(Engine.is_editor_hint(), "Editor fixture needs --editor")
    check(ClassDB.class_exists("UnsafeGDScriptSyntaxHighlighter"), "Editor highlighter registration")
    var script = UnsafeGDScript.new()
    script.source_code = "extends Node\n@export var amount: int = 7\nfunc _init():\n    amount = 99\n"
    check(script.reload() == OK, "Placeholder source compilation")
    check(not script.can_instantiate(), "Non-tool script disabled in editor")
    var node = Node.new()
    node.set_script(script)
    check(node.get_script() == script and node.get("amount") == 7, "Placeholder exposes defaults without executing _init")
    node.set("amount", 23)
    script.source_code += "@export var added: String = \"new\"\n"
    check(script.reload(true) == OK, "Placeholder soft reload")
    check(node.get("amount") == 23 and node.get("added") == "new", "Placeholder edits survive reload; new exports get defaults")
    check(node.get_property_list().any(func(p): return p.name == "added" and p.usage & PROPERTY_USAGE_EDITOR), "Placeholder inspector metadata")
    node.free()
    # Non-tool placeholders must reconstruct constant defaults without running
    # member initializers or _init. Compare the Inspector values to GDScript.
    var defaults_source = """extends Node
@export var direction: Vector2 = Vector2(3, 4)
@export var values: Array = [1, 2, {"point": Vector2(-2, 5)}]
@export var mapping: Dictionary = {"points": [Vector2(1, 2)]}
@export var tint: Color = Color(0.25, 0.5, 0.75)
var side_effect = touch()
func touch():
    Engine.set_meta("unsafe_editor_initializer_ran", true)
    return 1
func _init():
    Engine.set_meta("unsafe_editor_initializer_ran", true)
"""
    var defaults = UnsafeGDScript.new()
    defaults.source_code = defaults_source
    check(defaults.reload() == OK, "Compound placeholder defaults compile")
    var baseline = GDScript.new()
    baseline.source_code = defaults_source
    check(baseline.reload() == OK, "Baseline placeholder defaults compile")
    var native_node = Node.new()
    var baseline_node = Node.new()
    var other_node = Node.new()
    native_node.set_script(defaults)
    baseline_node.set_script(baseline)
    other_node.set_script(defaults)
    for property in ["direction", "values", "mapping", "tint"]:
        check(native_node.get(property) == baseline_node.get(property), "Placeholder default parity: " + property)
        check(native_node.property_can_revert(property), "Compound default can revert: " + property)
        check(native_node.property_get_revert(property) == baseline_node.get(property), "Compound revert value: " + property)
    if native_node.get("values") is Array:
        native_node.get("values")[2]["point"] = Vector2(9, 9)
        check(other_node.get("values")[2]["point"] == Vector2(-2, 5), "Nested defaults are independent per placeholder")
        check(native_node.property_get_revert("values")[2]["point"] == Vector2(-2, 5), "Editing an Array leaves the revert default intact")
    native_node.set("direction", Vector2(8, 9))
    defaults.source_code += "\n@export var added_vector: Vector2 = Vector2(6, 7)\n"
    check(defaults.reload(true) == OK, "Reload compound placeholder defaults")
    check(native_node.get("direction") == Vector2(8, 9), "Reload preserves edited vector")
    check(native_node.get("added_vector") == Vector2(6, 7), "Reload exposes new vector default")
    check(not Engine.has_meta("unsafe_editor_initializer_ran"), "Default inspection never executes script code")
    native_node.free()
    baseline_node.free()
    other_node.free()
    var broken_source = "extends Node\nfunc broken(:\n"
    var file = FileAccess.open("user://broken.ugd", FileAccess.WRITE)
    file.store_string(broken_source)
    file.close()
    var broken = ResourceLoader.load("user://broken.ugd", "", ResourceLoader.CACHE_MODE_IGNORE)
    check(broken is UnsafeGDScript and broken.source_code == broken_source and not broken.get_compile_error().is_empty(), "Invalid source remains editable")
    print("Editor checks: ", failures, " failures")
    quit(1 if failures else 0)
