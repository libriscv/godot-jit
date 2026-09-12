"""Verify that a real relocated module publishes bounded, named JIT frames."""
import os
from pathlib import Path
import subprocess
import sys

env = os.environ.copy()
env.pop("GODOT_JIT_CC", None)
env["GODOT_JIT_PERF_MAP"] = "1"
process = subprocess.Popen([sys.argv[1], "--headless", "--path", sys.argv[2],
    "--script", "res://tests/typical_benchmark.gd", "--", "--check-only"],
    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
path = Path(f"/tmp/perf-{process.pid}.map")
try:
    output, _ = process.communicate(timeout=30)
    print(output)
    if process.returncode or "ERROR:" in output:
        raise RuntimeError("Profile fixture failed")
    rows = [line.split(maxsplit=2) for line in path.read_text().splitlines()]
    names = {name for _, _, name in rows}
    for suffix in (":think", ":think:typed", ":nearest:typed", ":bench_normalize:typed"):
        if not any(name.endswith(suffix) for name in names):
            raise RuntimeError(f"Missing JIT frame {suffix}")
    intervals = sorted((int(address, 16), int(size, 16)) for address, size, _ in rows)
    for index, (address, size) in enumerate(intervals):
        if address <= 0 or not 0 < size < 10000000:
            raise RuntimeError("Invalid JIT symbol interval")
        if index and intervals[index - 1][0] + intervals[index - 1][1] > address:
            raise RuntimeError("Overlapping JIT symbols")
    print(f"Verified {len(rows)} JIT frames")
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
    path.unlink(missing_ok=True)
