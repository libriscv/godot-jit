extends SceneTree

func _initialize():
    call_deferred("run_checks")

func run_checks():
    var script = load("res://tests/debugger.ugd")
    var mode = OS.get_cmdline_user_args()[0]
    if mode == "inherited":
        script = load("res://tests/debugger_inherited.ugd")
    if mode == "over":
        script.source_code = script.source_code.replace("    breakpoint\n", "    pass\n")
        script.reload()
    var instance = script.new()
    if mode == "inherited":
        print("RESULT ", instance.outer())
    elif mode == "into" or mode == "over":
        EngineDebugger.insert_breakpoint(12, script.resource_path)
        print("RESULT ", instance.outer(5))
    elif mode == "inspect" or mode == "step":
        print("RESULT ", instance.outer(5))
    elif mode == "loop":
        EngineDebugger.insert_breakpoint(17, script.resource_path)
        print("RESULT ", instance.looping())
        EngineDebugger.remove_breakpoint(17, script.resource_path)
        print("CLEARED ", instance.looping())
    elif mode == "error":
        instance.fail(0)
        print("RECOVERED ", instance.looping())
    elif mode == "other":
        var other = script.new()
        other.member = 100
        print("RESULT ", instance.call_other(other))
    elif mode == "nested":
        print("RESULT ", instance.nested())
    print("DEBUGGER DONE")
    quit()
