"""Exercise the real Godot local debugger with deterministic commands.

The local debugger is driven one command per prompt on purpose. Godot reads
stdin with fgets() on Unix (a line at a time) and with a single ReadFile()
on Windows.
"""
import queue
import re
import subprocess
import sys
import threading
import time

binary, project = sys.argv[1:]

PROMPT = "debug> "
# Fall back to sending a command when the engine has been silent this long, in
# case stdout buffering ever hides the prompt from us.
IDLE = 2.0


def drive(args, commands, timeout):
    """Run Godot, answering every "debug> " prompt with the next command."""
    proc = subprocess.Popen(
        args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    proc.stdin.reconfigure(newline="\n")  # never translate commands to CRLF
    incoming = queue.Queue()

    def reader():
        while True:
            char = proc.stdout.read(1)
            incoming.put(char)
            if not char:
                return

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    output, pending, since_prompt = [], list(commands), ""
    quiet = time.monotonic()
    deadline = quiet + timeout
    while True:
        try:
            char = incoming.get(timeout=0.1)
        except queue.Empty:
            char = ""
        else:
            if not char:
                break
            output.append(char)
            since_prompt += char
            quiet = time.monotonic()
        if time.monotonic() > deadline:
            proc.kill()
            thread.join(timeout=5)
            return None, "".join(output)
        if not pending:
            continue
        if since_prompt.endswith(PROMPT) or time.monotonic() - quiet > IDLE:
            since_prompt = ""
            quiet = time.monotonic()
            try:
                proc.stdin.write(pending.pop(0) + "\n")
                proc.stdin.flush()
            except (BrokenPipeError, OSError):
                pending.clear()
    try:
        proc.stdin.close()
    except (BrokenPipeError, OSError):
        pass
    return proc.wait(), "".join(output).replace("\r\n", "\n")


def run(mode, commands, expected, stops):
    code, output = drive(
        [binary, "--headless", "--path", project, "-d", "--script", "res://tests/debugger.gd", "--", mode],
        commands, timeout=30,
    )
    actual = re.findall(r"Debugger Break, Reason:.*?\n\*Frame 0 - (.*?)\n", output)
    if code is None:
        raise AssertionError(f"{mode} timed out:\n{output}\nStops: {actual}")
    if code or "DEBUGGER DONE" not in output or actual != stops or any(x not in output for x in expected):
        raise AssertionError(f"{mode} failed (exit {code}):\n{output}\nStops: {actual}")
    print(f"Debugger {mode}: passed")


def location(line, function):
    return f"res://tests/debugger.ugd:{line} in function '{function}'"


run("inspect", ["bt", "locals", "members", "globals", "p local_value", "fr 1", "locals", "c"],
    ["value: 5", "local_value: 7", "member: 7", "shared: 11", "ANSWER: 42", "parent_value: 5", "RESULT 16", "Frame 1 - " + location(12, "outer")],
    [location(7, "inner")])
run("step", ["n", "n", "o", "c"], ["RESULT 16"],
    [location(7, "inner"), location(8, "inner"), location(9, "inner"), location(13, "outer")])
run("loop", ["locals", "c", "locals", "c", "locals", "c"], ["i: 0", "i: 1", "i: 2", "RESULT 3", "CLEARED 3"],
    [location(17, "looping")] * 3)
run("error", ["bt", "locals", "c"], ["Integer division by zero", "before_error: 99", "RECOVERED 3"],
    [location(21, "fail")])
run("other", ["bt", "locals", "members", "fr 1", "members", "c"], ["local_value: 22", "member: 100", "member: 7", "RESULT 123"],
    [location(7, "inner")])
run("nested", ["bt", "locals", "members", "c"], ["nested_local: 32", "nested_member: 31", "RESULT 63"],
    [location(28, "@Nested.inspect")])

run("into", ["delete", "s", "c", "c"], ["RESULT 16"],
    [location(12, "outer"), location(6, "inner"), location(7, "inner")])
run("over", ["delete", "n", "c"], ["RESULT 16"], [location(12, "outer"), location(13, "outer")])
run("inherited", ["bt", "locals", "c"], ["base_local: 15", "RESULT 15", "res://tests/debugger_inherited.ugd:3 in function 'outer'"],
    ["res://tests/debugger_base.ugd:4 in function 'inherited'"])

# Exercise the full instance suite with debug C emission as well as optimized C.
code, output = drive(
    [binary, "--headless", "--path", project, "-d", "--script", "res://tests/language.gd"],
    ["c"] * 16, timeout=60,
)
if code or "UnsafeGDScript language checks: 0 failures" not in output:
    raise AssertionError(f"Debug instance suite failed (exit {code}):\n{output}")
print("Debugger instance parity: passed")
