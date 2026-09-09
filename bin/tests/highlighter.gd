extends SceneTree

var failures := 0
func check(ok: bool, message: String):
    if not ok:
        failures += 1
        push_error(message)

func color_at(map: Dictionary, column: int) -> Color:
    var color := Color()
    var columns = map.keys()
    columns.sort()
    for key in columns:
        if key > column:
            break
        color = map[key].color
    return color

func _initialize():
    call_deferred("run_checks")

func run_checks():
    var edit = CodeEdit.new()
    root.add_child(edit)
    var highlighter = UnsafeGDScriptCodeHighlighter.new()
    edit.syntax_highlighter = highlighter
    edit.text = "func example(n: int):\n    var value = 42 # comment\n    var text = \"\"\"first\nstill a string # func\nend\"\"\" + value\n@export var path = $Child\nvar unique = %Child\nvar name = &\"hello\"\n## documentation\n# comment\nvar other = '''first\nstill quoted\nend'''\n"
    highlighter.update_cache()
    var first = highlighter.get_line_syntax_highlighting(0)
    check(color_at(first, 0) != color_at(first, 5), "Function definitions have their own color")
    var second = highlighter.get_line_syntax_highlighting(1)
    check(color_at(second, 16) != color_at(second, 19), "Numbers and comments differ")
    # Request a later line first to exercise multiline region reconstruction.
    var closing = highlighter.get_line_syntax_highlighting(4)
    var middle = highlighter.get_line_syntax_highlighting(3)
    check(color_at(middle, 0) == color_at(closing, 0), "Multiline string continuation")
    check(color_at(closing, 9) != color_at(closing, 0), "String closes before the operator")
    check(color_at(highlighter.get_line_syntax_highlighting(8), 0) != color_at(highlighter.get_line_syntax_highlighting(9), 0), "Documentation comments")
    check(color_at(highlighter.get_line_syntax_highlighting(11), 0) == color_at(middle, 0), "Triple single-quoted strings")
    check(color_at(highlighter.get_line_syntax_highlighting(5), 0) != color_at(highlighter.get_line_syntax_highlighting(5), 12), "Annotations")
    check(color_at(highlighter.get_line_syntax_highlighting(5), 19) != color_at(highlighter.get_line_syntax_highlighting(6), 13), "Node paths and unique node references")
    check(color_at(highlighter.get_line_syntax_highlighting(7), 11) != color_at(middle, 0), "StringName literal")
    edit.text = "var value = 5\nfunc updated():\n    return value\n"
    highlighter.clear_highlighting_cache()
    check(color_at(highlighter.get_line_syntax_highlighting(1), 0) == color_at(first, 0), "Edits invalidate multiline state")
    edit.free()
    print("Highlighter checks: ", failures, " failures")
    quit(1 if failures else 0)
