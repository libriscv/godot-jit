extends SceneTree

# CTest supplies one fixture per process, keeping failures and static state isolated.
# Test methods take no arguments and must return true; null (including runtime
# errors) and false fail even in builds where assert() is disabled.
func _initialize() -> void:
    call_deferred("run_tests")

func run_tests() -> void:
    var args := OS.get_cmdline_user_args()
    if args.size() != 1:
        push_error("Expected one .ugd test fixture")
        quit(1)
        return
    var script = load(args[0])
    if not script is UnsafeGDScript or not script.can_instantiate():
        push_error("Cannot load UnsafeGDScript fixture: " + args[0])
        quit(1)
        return
    var methods: Array[String] = []
    for method in script.get_script_method_list():
        if String(method.name).begins_with("test_"):
            methods.append(method.name)
    methods.sort()
    var failures := 0
    if methods.is_empty():
        push_error("No test_ methods in " + args[0])
        quit(1)
        return
    for method in methods:
        # Fresh members for every test; fixtures must extend RefCounted.
        var instance = script.new()
        if not instance is RefCounted:
            push_error("Fixture must construct a RefCounted: " + args[0])
            if is_instance_valid(instance):
                instance.free()
            quit(1)
            return
        var result: Variant = instance.call(method)
        if typeof(result) != TYPE_BOOL or result != true:
            failures += 1
            push_error(args[0] + "::" + method + " returned " + str(result))
        else:
            print("PASS ", args[0], "::", method)
    print(args[0], ": ", methods.size(), " tests, ", failures, " failures")
    quit(0 if failures == 0 else 1)
