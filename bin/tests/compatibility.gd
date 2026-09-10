extends SceneTree

var failures := 0

func check(ok: bool, message: String) -> void:
    if not ok:
        failures += 1
        push_error(message)

func compile_source(source: String) -> Script:
    var script = UnsafeGDScript.new()
    script.source_code = source
    check(script.reload() == OK, "Compatibility fixture compiles: " + script.get_compile_error())
    return script

func _initialize() -> void:
    call_deferred("run_checks")

func run_checks() -> void:
    var source = """extends RefCounted
var received: String
const STATES = {&"idle": &"idle", &"walk": &"walk"}
func handler(value: String) -> void:
    received = value
func string_name(value: StringName) -> StringName:
    return value
func vector(value: Vector2) -> Vector2:
    return value
func test_types(node: Node) -> bool:
    var missing: Object = null
    return not (missing is Object) and not (node is Control) and (node is Node)
func round_vector(value):
    return [floor(value), ceil(value), round(value), abs(value), sign(value), snapped(value, Vector2.ONE)]
func singleton_names():
    return [RenderingServer.get_video_adapter_name(), DisplayServer.get_name(), OS.get_name()]
func autoload_node():
    return CompatAutoload
"""
    # Autoload names are made available by project settings at compile time;
    # lookup must work even when the caller is not a Node.
    ProjectSettings.set_setting("autoload/CompatAutoload", "*res://unused.gd")
    var script = compile_source(source)
    var autoload_node := Node.new()
    autoload_node.name = "CompatAutoload"
    root.add_child(autoload_node)
    var instance = script.new()
    check(script.STATES == {&"idle": &"idle", &"walk": &"walk"}, "Compound constants are exposed by script resources")
    check(instance.autoload_node() == autoload_node, "Resolve project autoload from RefCounted")
    var player := AnimationPlayer.new()
    player.animation_finished.connect(instance.handler)
    player.animation_finished.emit(&"walk")
    check(instance.received == "walk", "Signal StringName converts to String")
    check(instance.string_name("walk") == &"walk", "String converts to StringName")
    check(instance.vector(Vector2i(3, 4)) == Vector2(3, 4), "Vector2i converts to Vector2")
    check(instance.test_types(autoload_node), "Null and unscripted objects pass type guards")
    var value := Vector2(-1.2, 3.8)
    check(instance.round_vector(value) == [floor(value), ceil(value), round(value), abs(value), sign(value), snapped(value, Vector2.ONE)], "Generic math preserves vector components")
    check(instance.singleton_names() == [RenderingServer.get_video_adapter_name(), DisplayServer.get_name(), OS.get_name()], "Dynamic engine singleton access")
    player.free()
    autoload_node.free()
    ProjectSettings.set_setting("autoload/CompatAutoload", null)

    var enum_script = compile_source("""extends LineEdit
func flags() -> int:
    return ConnectFlags.CONNECT_DEFERRED
""")
    var field = enum_script.new()
    check(field.flags() == Object.CONNECT_DEFERRED, "Inherited native enum resolves its declaring class")
    field.free()

    var effect_script = compile_source("""extends CompositorEffect
var shader: RID
func read_shader() -> bool:
    return shader.is_valid()
func _notification(what: int) -> void:
    if what == NOTIFICATION_PREDELETE:
        Engine.set_meta("compat_predelete", [shader.is_valid(), read_shader(), self.is_class("CompositorEffect")])
""")
    var effect = effect_script.new()
    var weak = weakref(effect)
    effect = null
    check(weak.get_ref() == null, "CompositorEffect is destroyed")
    check(Engine.get_meta("compat_predelete", []) == [false, false, true], "Predelete can read members and call methods")
    Engine.remove_meta("compat_predelete")
    print("Compatibility checks: ", failures, " failures")
    quit(1 if failures else 0)
