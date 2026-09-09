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
    var broken_source = "extends Node\nfunc broken(:\n"
    var file = FileAccess.open("user://broken.ugd", FileAccess.WRITE)
    file.store_string(broken_source)
    file.close()
    var broken = ResourceLoader.load("user://broken.ugd", "", ResourceLoader.CACHE_MODE_IGNORE)
    check(broken is UnsafeGDScript and broken.source_code == broken_source and not broken.get_compile_error().is_empty(), "Invalid source remains editable")
    print("Editor checks: ", failures, " failures")
    quit(1 if failures else 0)
