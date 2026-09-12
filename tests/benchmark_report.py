"""Strict benchmark parsing, cc comparison and the opt-in local regression gate."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKLOADS = {
    "call": "integer_loop float_loop array_loop dictionary_loop vector_loop gameplay_loop local_calls object_calls callable_calls".split(),
    "typical": "think_5 minigame call_floor nearest_0 nearest_5 iterate_5 distance_5 normalize batch_think batch_nearest batch_iterate batch_normalize batch_floor".split(),
    "math": ["bench_primitives", "bench_engine"],
}


def parse_log(text, suite, iterations):
    if re.search(r"SCRIPT ERROR:|^ERROR:", text, re.M):
        raise ValueError(f"{suite}: Godot reported an error")
    rows = {}
    for line in text.splitlines():
        if suite == "call":
            match = re.match(r"(\w+): GDScript=(\d+) us UnsafeGDScript=(\d+) us .* checksum=(.*)", line)
        else:
            match = re.match(r"(\w+): GD=([\d.]+) us/op JIT=([\d.]+) us/op", line)
        if match:
            name, gd, jit = match.group(1, 2, 3)
            if name in rows:
                raise ValueError(f"Duplicate benchmark: {name}")
            scale = iterations if suite == "call" else 1
            rows[name] = {"gd": float(gd) / scale, "jit": float(jit) / scale}
            if min(rows[name].values()) <= 0:
                raise ValueError(f"{name}: timer resolution insufficient; increase iterations")
            if suite == "call":
                rows[name]["checksum"] = match[4]
    if set(rows) != set(WORKLOADS[suite]):
        raise ValueError(f"{suite}: incomplete/unexpected rows: {set(rows) ^ set(WORKLOADS[suite])}")
    for name, gd, jit in re.findall(r"(\w+) instructions: GD=([-\d.]+) JIT=([-\d.]+) insn/op", text):
        rows[name].update(gd_instructions=float(gd), jit_instructions=float(jit))
    return rows


def check_gate(report, baseline):
    gate = baseline["regression_gate"]
    if report["fixture_sha256"] != gate["fixture_sha256"]:
        raise ValueError("Benchmark fixture changed; explicitly rebaseline the regression gate")
    rows = report["results"]
    if len(rows) != len(gate["max_jit_gd"]) or {r["workload"] for r in rows} != set(gate["max_jit_gd"]):
        raise ValueError("Incomplete or duplicate regression results")
    failures = []
    for row in rows:
        for key in ("gd_samples_us", "jit_samples_us"):
            samples = row[key]
            if len(samples) != 7 or any(not math.isfinite(x) or x <= 0 for x in samples):
                raise ValueError(f"Invalid timing samples: {row['workload']}")
        ratio = sorted(row["jit_samples_us"])[3] / sorted(row["gd_samples_us"])[3]
        limit = gate["max_jit_gd"][row["workload"]]
        print(f"{row['workload']}: JIT/GD={ratio:.3f}, limit={limit:.3f}")
        if ratio > limit:
            failures.append(row["workload"])
    if failures:
        raise ValueError("Performance regression: " + ", ".join(failures))


def precise_typical(path, rows):
    """Use raw integer timers instead of rounded console microseconds/op."""
    report = json.loads(path.read_text())
    results = report["results"]
    if len(results) != len(rows) or {r["workload"] for r in results} != set(rows):
        raise ValueError("Incomplete or duplicate typical JSON results")
    for row in results:
        iterations = row["iterations"]
        if iterations <= 0:
            raise ValueError("Invalid typical iteration count")
        for language in ("gd", "jit"):
            samples = row[language + "_samples_us"]
            if len(samples) != 7 or any(not math.isfinite(x) or x <= 0 for x in samples):
                raise ValueError("Invalid typical timing samples")
            rows[row["workload"]][language] = sorted(samples)[3] / iterations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs", type=Path)
    parser.add_argument("--iterations", type=int, default=20000)
    parser.add_argument("--gate", nargs=2, metavar=("GODOT", "PROJECT"))
    args = parser.parse_args()
    if args.gate:
        env = os.environ.copy()
        for key in ("GODOT_JIT_CC", "GODOT_JIT_BENCH_ROW"):
            env.pop(key, None)
        run = subprocess.run([args.gate[0], "--headless", "--path", args.gate[1],
                              "--script", "res://tests/typical_benchmark.gd", "--", str(args.iterations)],
                             env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        print(run.stdout)
        if run.returncode:
            raise ValueError(f"Benchmark exited {run.returncode}")
        parse_log(run.stdout, "typical", args.iterations)
        reports = re.findall(r"^Results: (.+)$", run.stdout, re.M)
        if len(reports) != 1:
            raise ValueError("Missing benchmark report")
        check_gate(json.loads(Path(reports[0]).read_text()), json.loads((ROOT / "tests/typical_benchmark_results.json").read_text()))
        return
    if not args.logs:
        parser.error("--logs or --gate is required")
    if (args.logs / "typical-cc.log").exists():
        native = json.loads((args.logs / "typical.json").read_text())
        twin = json.loads((args.logs / "typical-cc.json").read_text())
        for key in ("fixture_sha256", "seed", "delta"):
            if native[key] != twin[key]:
                raise ValueError(f"Compiler comparison changed {key}")
        def checksums(report):
            return [(r["workload"], r["iterations"], r["checksum"]) for r in report["results"]]
        if checksums(native) != checksums(twin):
            raise ValueError("Compiler comparison changed typical checksums")
    for suite in WORKLOADS:
        rows = parse_log((args.logs / f"{suite}.log").read_text(), suite, args.iterations)
        cc_path = args.logs / f"{suite}-cc.log"
        twin = parse_log(cc_path.read_text(), suite, args.iterations) if cc_path.exists() else None
        if suite == "typical":
            precise_typical(args.logs / "typical.json", rows)
            if twin:
                precise_typical(args.logs / "typical-cc.json", twin)
        print(f"\n{suite}: median microseconds per operation (compilation excluded)")
        print(f"{'Workload':22} {'GD':>11} {'tcc':>11} {'cc -O2':>11} {'JIT/GD':>9} {'tcc/cc':>9}")
        for name in WORKLOADS[suite]:
            row = rows[name]
            cc = twin[name]["jit"] if twin else None
            if twin and suite == "call" and row["checksum"] != twin[name]["checksum"]:
                raise ValueError(f"{name}: compiler checksum mismatch")
            cc_text = f"{cc:.6f}" if cc is not None else "n/a"
            factor = f"{row['jit']/cc:.3f}" if cc else "n/a"
            print(f"{name:22} {row['gd']:11.6f} {row['jit']:11.6f} {cc_text:>11} {row['jit']/row['gd']:9.3f} {factor:>9}")
            if "gd_instructions" in row:
                def insn(value):
                    return f"{value:.1f}" if value >= 0 else "unavailable"
                values = [insn(row[k]) for k in ("gd_instructions", "jit_instructions")]
                if twin:
                    values.append(insn(twin[name].get("jit_instructions", -1)))
                print("  instructions/op (GD, tcc, cc): " + ", ".join(values))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
