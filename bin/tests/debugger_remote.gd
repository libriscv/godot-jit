# Act as the editor to verify frame variables and the native ScriptInstance
# handle, which the local debugger does not dereference.
extends SceneTree
var server := TCPServer.new()
var stream: StreamPeerTCP
var peer := PacketPeerStream.new()
var child := -1
var deadline := 0
var thread_id := 1
var expected_count := -1
var variables := {}
var continued := false
var failures := 0

func check(ok: bool, message: String):
    if not ok:
        failures += 1
        printerr(message)

func _initialize():
    if server.listen(0, "127.0.0.1") != OK:
        printerr("Cannot listen for debugger fixture")
        quit(1)
        return
    deadline = Time.get_ticks_msec() + 15000
    child = OS.create_process(OS.get_executable_path(), ["--headless", "--path", ProjectSettings.globalize_path("res://"),
        "--remote-debug", "tcp://127.0.0.1:%d" % server.get_local_port(), "--script", "res://tests/debugger.gd", "--", "inspect"])

func send(command: String, data: Array):
    peer.put_var([command, thread_id, data])

func _process(_delta):
    if deadline == 0:
        return true
    if Time.get_ticks_msec() > deadline:
        check(false, "Remote debugger fixture timed out")
        finish()
        return false
    if stream == null:
        if server.is_connection_available():
            stream = server.take_connection()
            peer.stream_peer = stream
        return false
    stream.poll()
    while peer.get_available_packet_count() > 0:
        var packet = peer.get_var()
        if not packet is Array or packet.size() < 3:
            continue
        thread_id = packet[1]
        var data = packet[2]
        match packet[0]:
            "debug_enter":
                send("get_stack_dump", [])
                send("get_stack_frame_vars", [0])
            "stack_dump":
                check(data.size() == 7 and data[1] == "res://tests/debugger.ugd" and data[2] == 7 and data[3] == "inner" and data[6] == "outer", "Remote stack dump")
            "stack_frame_vars":
                expected_count = data[0]
            "stack_frame_var":
                variables[data[0]] = data[3]
    if not continued and expected_count >= 0 and variables.size() == expected_count:
        check(variables.get("value") == 5 and variables.get("local_value") == 7, "Remote locals")
        check(variables.get("member") == 7 and variables.get("shared") == 11 and variables.get("ANSWER") == 42, "Remote members and globals")
        check(variables.has("self") and variables.self != null, "Remote ScriptInstance owner")
        continued = true
        send("continue", [])
    if child > 0 and not OS.is_process_running(child):
        check(continued, "Child exited before debugger inspection")
        finish()
    return false

func finish():
    if child > 0 and OS.is_process_running(child):
        OS.kill(child)
    if stream:
        stream.disconnect_from_host()
    server.stop()
    print("Remote debugger checks: ", failures, " failures")
    quit(1 if failures else 0)
