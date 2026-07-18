"""Cross-platform benchmark runner for multi-core lab servers.

The runner does not assume PowerShell. It executes an already built binary,
records structured results, validates them, summarizes repeated runs and can
generate figures. Use ``--threads auto --max-threads 48`` on the school
platform to obtain 1/2/4/8/12/16/24/32/48 thread points. The quarter- and
half-capacity points help reveal physical-core and NUMA boundaries.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import random
import shlex
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


INTEGER_KEYS = (
    "orders", "prior_rows", "train_rows", "products", "users",
    "hardware_threads", "omp_max_threads", "unique_pairs", "pair_events",
    "graph_edge_entries", "max_degree", "active_users",
    "candidate_shortage_users", "empty_candidate_users", "total_candidates",
    "max_candidates",
)
FLOAT_KEYS = (
    "load_ms", "cooccur_compute_ms", "merge_ms", "normalization_ms",
    "adjacency_ms", "truncate_ms", "recommend_ms", "evaluate_ms",
    "algorithm_ms", "end_to_end_ms", "hit_rate", "precision", "recall",
    "f1", "ndcg", "mrr", "micro_precision", "micro_recall",
)


def auto_threads(limit: int) -> list[int]:
    values = {1, limit}
    current = 2
    while current < limit:
        values.add(current)
        current *= 2
    if limit >= 16:
        values.add(max(1, limit // 4))
        values.add(max(1, limit // 2))
    return sorted(value for value in values if value <= limit)


def parse_threads(value: str, limit: int) -> list[int]:
    if value.lower() == "auto":
        return auto_threads(limit)
    result = sorted({int(item) for item in value.split(",") if item.strip()})
    if not result or result[0] < 1 or result[-1] > limit:
        raise ValueError(f"threads must be within 1..{limit}")
    return result


def parse_key_values(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def required(values: dict[str, str], key: str) -> str:
    if key not in values:
        raise RuntimeError(f"program output is missing required field: {key}")
    return values[key]


def run_program(args: argparse.Namespace, mode: str, threads: int,
                environment: dict[str, str]) -> tuple[dict[str, str], float]:
    command = [
        str(args.executable), "--data", str(args.data), "--mode", mode,
        "--threads", str(threads), "--top-k", str(args.top_k),
        "--max-neighbors", str(args.max_neighbors),
        "--cooccur-schedule", args.cooccur_schedule,
        "--cooccur-chunk", str(args.cooccur_chunk),
        "--recommend-schedule", args.recommend_schedule,
        "--recommend-chunk", str(args.recommend_chunk),
    ]
    started = time.perf_counter()
    completed = subprocess.run(
        command, check=False, capture_output=True, text=True,
        encoding="utf-8", errors="replace", env=environment,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    if completed.returncode != 0:
        raise RuntimeError(
            f"benchmark failed ({mode}, {threads} threads):\n{completed.stderr}"
        )
    return parse_key_values(completed.stdout), elapsed_ms


def make_record(args: argparse.Namespace, run_id: str, mode: str,
                threads: int, repeat: int, values: dict[str, str],
                runner_elapsed_ms: float) -> dict[str, object]:
    record: dict[str, object] = {
        "run_id": run_id,
        "timestamp": datetime.now(timezone.utc).astimezone().isoformat(),
        "dataset": args.dataset,
        "algorithm_profile": required(values, "algorithm_profile"),
        "version": mode,
        "mode": mode,
        "threads": 1 if mode == "serial" else threads,
        "repeat": repeat,
        "run": repeat,
        "warmup": 0,
        "top_k": args.top_k,
        "max_neighbors": args.max_neighbors,
        "popular_fallback": 0,
        "schedule": "none" if mode == "serial" else args.cooccur_schedule,
        "cooccur_schedule": "none" if mode == "serial" else args.cooccur_schedule,
        "cooccur_chunk": args.cooccur_chunk,
        "recommend_schedule": "none" if mode == "serial" else args.recommend_schedule,
        "recommend_chunk": args.recommend_chunk,
        "runner_elapsed_ms": runner_elapsed_ms,
    }
    for key in INTEGER_KEYS:
        record[key] = int(required(values, key))
    for key in FLOAT_KEYS:
        record[key] = float(required(values, key))
    record["cooccur_checksum"] = required(values, "cooccur_checksum")
    record["recommendation_checksum"] = required(values, "recommendation_checksum")
    record["status"] = "ok"
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path,
                        default=Path("build/basket_recommender.exe"))
    parser.add_argument("--data", type=Path, default=Path("data/medium"))
    parser.add_argument("--dataset", default="medium")
    parser.add_argument("--threads", default="auto",
                        help="comma list or auto")
    parser.add_argument("--max-threads", type=int,
                        default=os.cpu_count() or 1)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--serial-repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--max-neighbors", type=int, default=0)
    parser.add_argument("--cooccur-schedule",
                        choices=("static", "dynamic", "guided"), default="dynamic")
    parser.add_argument("--cooccur-chunk", type=int, default=64)
    parser.add_argument("--recommend-schedule",
                        choices=("static", "dynamic", "guided"), default="dynamic")
    parser.add_argument("--recommend-chunk", type=int, default=16)
    parser.add_argument("--proc-bind", default="spread")
    parser.add_argument("--places", default="cores")
    parser.add_argument("--ordered", action="store_true",
                        help="do not randomize thread order within each repeat")
    parser.add_argument("--seed", type=int, default=20260718)
    parser.add_argument("--output-root", type=Path,
                        default=Path("results/platform-experiments"))
    parser.add_argument("--build-command",
                        help="optional shell command, for example 'make release'")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    if (args.max_threads < 1 or args.repeats < 1 or args.serial_repeats < 1):
        parser.error("thread and repeat counts must be positive")
    if args.warmups < 0 or args.top_k < 1 or args.max_neighbors < 0:
        parser.error("invalid warmup/top-k/max-neighbors")
    threads = parse_threads(args.threads, args.max_threads)
    detected = os.cpu_count() or 1
    if max(threads) > detected:
        print(
            f"warning: requesting {max(threads)} threads but OS reports {detected}",
            file=sys.stderr,
        )
    if args.build_command:
        subprocess.run(args.build_command, shell=True, check=True)
    if not args.executable.exists():
        alternative = Path("build/basket_recommender")
        if args.executable.name.endswith(".exe") and alternative.exists():
            args.executable = alternative
        else:
            parser.error(f"executable not found: {args.executable}")

    profile = ("fast-normalization-full" if args.max_neighbors == 0
               else f"fast-normalization-top{args.max_neighbors}")
    run_id = datetime.now().strftime("%Y%m%d-%H%M%S%f")[:-3]
    run_id += f"-{args.dataset}-{profile}"
    target = args.output_root / run_id
    target.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["OMP_PROC_BIND"] = args.proc_bind
    environment["OMP_PLACES"] = args.places

    for _ in range(args.warmups):
        run_program(args, "serial", 1, environment)
        for thread_count in threads:
            run_program(args, "openmp", thread_count, environment)

    records: list[dict[str, object]] = []
    for repeat in range(1, args.serial_repeats + 1):
        values, elapsed = run_program(args, "serial", 1, environment)
        records.append(make_record(args, run_id, "serial", 1, repeat,
                                   values, elapsed))
    generator = random.Random(args.seed)
    for repeat in range(1, args.repeats + 1):
        order = list(threads)
        if not args.ordered:
            generator.shuffle(order)
        for thread_count in order:
            values, elapsed = run_program(
                args, "openmp", thread_count, environment
            )
            records.append(make_record(args, run_id, "openmp", thread_count,
                                       repeat, values, elapsed))

    raw = target / "raw.csv"
    with raw.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(records[0]))
        writer.writeheader(); writer.writerows(records)
    parameters = vars(args).copy()
    parameters["threads_resolved"] = threads
    manifest = {
        "schema_version": 2,
        "run_id": run_id,
        "created_at": datetime.now(timezone.utc).astimezone().isoformat(),
        "platform": platform.platform(),
        "hostname": platform.node(),
        "processor": platform.processor(),
        "detected_logical_processors": detected,
        "python": platform.python_version(),
        "executable": str(args.executable.resolve()),
        "data": str(args.data.resolve()),
        "parameters": parameters,
        "openmp_environment": {
            "OMP_PROC_BIND": args.proc_bind,
            "OMP_PLACES": args.places,
        },
        "files": {"raw": "raw.csv", "validation": "validation.json",
                  "summary": "summary.csv", "figures": "figures/"},
    }
    # Convert pathlib values before JSON serialization.
    manifest["parameters"] = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in manifest["parameters"].items()
    }
    (target / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    script_dir = Path(__file__).resolve().parent
    subprocess.run([sys.executable, str(script_dir / "validate_results.py"),
                    str(raw)], check=True)
    summary = target / "summary.csv"
    subprocess.run([sys.executable, str(script_dir / "summarize_results.py"),
                    str(raw), str(summary)], check=True)
    if not args.no_plot:
        subprocess.run([sys.executable, str(script_dir / "plot_results.py"),
                        str(summary), str(target / "figures")], check=True)
    print(f"EXPERIMENT_DIR={target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
