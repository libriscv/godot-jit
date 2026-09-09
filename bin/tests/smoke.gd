extends SceneTree

func _initialize() -> void:
	var jit := GodotJIT.new()
	var source := """
GodotJitInt godot_jit_entry(const GodotJitHost *host, GodotJitInt argument) {
    host->log(host->userdata, "Godot JIT native callback");
    return argument * 2 + 2;
}
"""
	if not jit.compile_c(source):
		push_error(jit.get_error())
		quit(1)
		return
	if jit.execute(20) != 42:
		quit(2)
		return
	if jit.compile_c("invalid C") or jit.get_error().is_empty():
		quit(3)
		return
	if jit.execute(20) != 42:
		quit(4)
		return
	jit.clear()
	if jit.is_compiled():
		quit(5)
		return
	print("Godot JIT extension smoke test passed")
	quit(0)
