extends SceneTree

signal tick(value)
signal empty
signal pair(a, b)
var failures := 0

const SOURCE = """
extends RefCounted
var total: int = 0
func immediate(value):
    return await value
func twice(event, held):
    var first = await event
    total += first
    var second = await event
    total += second
    return [total, held]
func child(event):
    var value = await event
    return value + 1
func nested(event):
    var first = await child(event)
    return await child(event) + first
func wait_value(event):
    return await event
func fail_after(event):
    await event
    assert(false, "async failure")
func fail_nested(event):
    return await fail_after(event)
"""

func check(ok: bool, message: String):
    if not ok:
        failures += 1
        push_error(message)

func observe(completion: Signal) -> Array:
    var values: Array = []
    completion.connect(func(value): values.append(value))
    return values

func _initialize():
    call_deferred("run_tests")

func run_tests():
    var script = UnsafeGDScript.new()
    script.source_code = SOURCE
    check(script.reload() == OK, "Compile coroutines: " + script.get_compile_error())
    if not script.can_instantiate():
        quit(1)
        return
    var instance = script.new()
    check(instance.immediate(42) == 42, "Immediate scalar")
    check(instance.immediate([1, 2]) == [1, 2], "Immediate container")
    for method in script.get_script_method_list():
        if method.name == "child":
            check(method.return.type == TYPE_NIL, "Coroutine return metadata is Variant")

    var held := RefCounted.new()
    var weak = weakref(held)
    var completion: Signal = instance.twice(tick, held)
    var state = completion.get_object()
    var results = observe(completion)
    held = null
    check(weak.get_ref() != null, "Saved frame retains arguments")
    tick.emit(3)
    check(results.is_empty() and state.is_valid(), "First resume suspends again")
    tick.emit(4)
    check(results.size() == 1 and results[0][0] == 7, "Second resume completes with locals")
    check(not state.is_valid(), "Completed frame retires")
    results.clear()
    check(weak.get_ref() == null, "Completed frame releases owned values")
    check(tick.get_connections().is_empty(), "Completion disconnects signal")

    completion = instance.nested(tick)
    results = observe(completion)
    tick.emit(10)
    check(results.is_empty(), "Nested helper suspends independently on resume")
    tick.emit(20)
    check(results == [32], "Nested await result")

    results = observe(instance.wait_value(empty))
    empty.emit()
    check(results == [null], "Zero argument signal")
    results = observe(instance.wait_value(pair))
    pair.emit(2, "two")
    check(results == [[2, "two"]], "Multiple signal arguments")
    var left = observe(instance.wait_value(tick))
    var right = observe(instance.wait_value(tick))
    tick.emit(9)
    check(left == [9] and right == [9], "Concurrent invocations")

    completion = instance.wait_value(tick)
    state = completion.get_object()
    results = observe(completion)
    state.cancel()
    tick.emit(99)
    check(state.was_cancelled() and results == [null] and tick.get_connections().is_empty(), "Explicit cancellation")

    completion = instance.wait_value(tick)
    state = completion.get_object()
    results = observe(completion)
    check(script.reload(true) == OK, "Reload suspended script")
    tick.emit(99)
    check(state.was_cancelled() and results == [null] and tick.get_connections().is_empty(), "Reload cancels old frames")

    completion = instance.wait_value(tick)
    state = completion.get_object()
    instance = null
    check(not state.is_valid() and tick.get_connections().is_empty(), "Instance teardown disconnects")

    var jit := GodotJIT.new()
    check(jit.compile_sgd(SOURCE.replace("extends RefCounted\n", "")), "Standalone compilation: " + jit.get_error())
    var pending = jit.execute_function("nested", [tick])
    if not pending is Signal:
        push_error("Standalone nested await: " + jit.get_error())
        quit(1)
        return
    completion = pending
    results = observe(completion)
    tick.emit(1)
    tick.emit(2)
    check(results == [5], "Standalone optimized nested await")
    completion = jit.execute_function("fail_nested", [tick])
    state = completion.get_object()
    results = observe(completion)
    tick.emit(1)
    check(state.has_failed() and state.get_failure_message().contains("async failure") and results == [null], "Async errors propagate and retire")
    completion = jit.execute_function("wait_value", [tick])
    state = completion.get_object()
    completion.connect(func(_value): jit.clear())
    jit.clear()
    check(state.was_cancelled() and tick.get_connections().is_empty(), "Reentrant clear cancels frames")

    check(jit.compile_sgd("func invalid(event):\n    return await event\n"), "Compile invalid signal test")
    check(jit.execute_function("invalid", [Signal()]) == null and jit.get_error().contains("Signal does not exist"), "Invalid signal reports failure")

    # Await directly from ordinary GDScript, including real timer signals.
    check(jit.compile_sgd("func timer(tree):\n    await tree.create_timer(0.001).timeout\n    return 42\n"), "Compile timer")
    var answer = await jit.execute_function("timer", [self])
    check(answer == 42, "GDScript awaits generated completion signal")
    print("Async checks: ", failures, " failures")
    quit(0 if failures == 0 else 1)
