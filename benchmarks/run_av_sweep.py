#!/usr/bin/env python3
"""Plan or explicitly execute isolated GraphEngine AV load sweeps (stdlib only)."""
import argparse
import csv
import json
import math
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


SCENARIOS = {
    "realtime": {"mode": "realtime", "payload": "shared"},
    "metadata": {"mode": "saturation", "payload": "metadata"},
    "shared": {"mode": "saturation", "payload": "shared"},
    "copy": {"mode": "realtime", "payload": "copy"},
    "fanout": {"mode": "realtime", "payload": "shared", "fanout": 8},
    "deep": {"mode": "realtime", "payload": "shared", "stages": 16},
    "work": {"mode": "realtime", "payload": "shared", "work_iterations": 10000},
    "4k": {"mode": "realtime", "payload": "shared", "width": 3840, "height": 2160},
}


def positive_ints(text):
    values = [int(v) for v in text.split(",")]
    if not values or any(v < 1 or v > 256 for v in values) or values != sorted(set(values)):
        raise argparse.ArgumentTypeError("use increasing unique integers in [1, 256]")
    return values


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    parser.add_argument("--scenario", choices=[*SCENARIOS, "all"], default="realtime")
    parser.add_argument("--levels", type=positive_ints, default=positive_ints("1,2,4,8,16,32,64"))
    parser.add_argument("--workers", type=positive_ints, default=positive_ints("1,4,8"))
    parser.add_argument("--dispatch", choices=["sync", "async"], default="async")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--base-config", type=Path, help="JSON overrides for duration, producers, budgets, etc.")
    parser.add_argument("--timeout", type=float, default=120, help="hard wall-clock limit per process, seconds")
    parser.add_argument("--run", action="store_true", help="execute; otherwise only write the plan and configs")
    args = parser.parse_args()
    if args.repeat < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("repeat and timeout must be positive")
    exe = args.exe.resolve()
    if args.run and not exe.is_file():
        parser.error(f"executable not found: {exe}")
    base = json.loads(args.base_config.read_text(encoding="utf-8-sig")) if args.base_config else {}
    if not isinstance(base, dict):
        parser.error("base config must be a JSON object")
    # New directory prevents stale reports from being mistaken for this run.
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    scenarios = list(SCENARIOS) if args.scenario == "all" else [args.scenario]
    plan = []
    for scenario in scenarios:
        for workers in args.workers:
            for pairs in args.levels:
                for repeat in range(args.repeat):
                    name = f"{scenario}-{args.dispatch}-w{workers}-p{pairs}-r{repeat + 1}"
                    config = {**SCENARIOS[scenario], **base, "pairs": pairs,
                              "workers": workers, "dispatch": args.dispatch}
                    write_json(output / f"{name}.config.json", config)
                    plan.append({"name": name, "scenario": scenario, "workers": workers,
                                 "pairs": pairs, "repeat": repeat + 1, "mode": config.get("mode", "realtime")})
    write_json(output / "plan.json", {"created_utc": datetime.now(timezone.utc).isoformat(),
               "host": platform.platform(), "python": sys.version, "executable": str(exe),
               "timeout_s": args.timeout, "executed": args.run, "cases": plan})
    if not args.run:
        print(f"Wrote {len(plan)} cases to {output}; no benchmark was executed. Use --run with a new output directory to execute.")
        return 0

    rows = []
    failed_groups = set()
    for case in plan:
        name = case["name"]
        group = (case["scenario"], case["workers"])
        if group in failed_groups:
            continue
        result_path = output / f"{name}.result.json"
        status, result, returncode = "error", {}, None
        with (output / f"{name}.stdout.log").open("w", encoding="utf-8") as stdout, \
             (output / f"{name}.stderr.log").open("w", encoding="utf-8") as stderr:
            try:
                process = subprocess.run([str(exe), "--config", str(output / f"{name}.config.json"),
                                          "--output", str(result_path)], stdout=stdout, stderr=stderr,
                                         timeout=args.timeout, check=False)
                returncode = process.returncode
                if result_path.exists():
                    result = json.loads(result_path.read_text(encoding="utf-8"))
                if returncode == 0 and result.get("pass") is True:
                    status = "pass"
                elif returncode == 2 and result.get("pass") is False:
                    status = "fail"
            except subprocess.TimeoutExpired:
                status = "timeout"
            except (OSError, ValueError) as error:
                stderr.write(str(error) + "\n")
        rows.append({**case, "status": status, "returncode": returncode,
                     "sink_deliveries_per_s": result.get("sink_deliveries_per_s"),
                     "cpu_percent_one_core": result.get("cpu_percent_one_core_including_drain"),
                     "peak_rss_bytes": result.get("process_peak_rss_bytes")})
        print(f"{name}: {status}", flush=True)
        # Finish all repetitions at a level; stop increasing real-time load after
        # that level fails. Saturation curves continue unless a process fails.
        if status in ("error", "timeout"):
            failed_groups.add(group)
        elif case["repeat"] == args.repeat and case["mode"] == "realtime":
            level_rows = [r for r in rows if (r["scenario"], r["workers"]) == group and r["pairs"] == case["pairs"]]
            if any(r["status"] != "pass" for r in level_rows):
                failed_groups.add(group)
        write_json(output / "results.json", rows)

    boundaries = []
    for scenario in scenarios:
        for workers in args.workers:
            group_rows = [r for r in rows if r["scenario"] == scenario and r["workers"] == workers]
            highest, first_failure, blocked = None, None, False
            for pairs in args.levels:
                level_rows = [r for r in group_rows if r["pairs"] == pairs]
                passed = len(level_rows) == args.repeat and all(r["status"] == "pass" for r in level_rows)
                if passed and not blocked:
                    highest = pairs
                else:
                    blocked = True
                    if level_rows and first_failure is None:
                        first_failure = pairs
            boundaries.append({"scenario": scenario, "workers": workers,
                "mode": group_rows[0]["mode"] if group_rows else None,
                "highest_contiguous_passing_pairs": highest,
                "first_nonpassing_pairs": first_failure,
                "interpretation": "sampled bounds only; saturation pass means integrity, not a realtime capacity limit"})
    write_json(output / "boundaries.json", boundaries)
    if rows:
        with (output / "summary.csv").open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    return 1 if any(row["status"] in ("error", "timeout") for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
