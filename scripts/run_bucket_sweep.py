"""Run, validate, rank, and plot bucketed-merge configurations."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


QUALITY_FIELDS = (
    "hit_rate", "precision", "recall", "f1", "ndcg", "mrr",
    "micro_precision", "micro_recall",
)
CHECK_FIELDS = (
    "cooccur_checksum", "recommendation_checksum", "graph_edge_entries",
    "max_degree", *QUALITY_FIELDS,
)


def parse_int_list(text: str) -> list[int]:
    values = sorted({int(item.strip()) for item in text.split(",") if item.strip()})
    if not values or values[0] < 1:
        raise argparse.ArgumentTypeError("expected positive comma-separated integers")
    return values


def resolve_reported_path(value: str) -> Path:
    path = Path(value.strip())
    if not path.is_absolute():
        path = Path.cwd() / path
    return path.resolve()


def run_command(command: list[str], label: str) -> subprocess.CompletedProcess[str]:
    print(f"\n=== {label} ===", flush=True)
    completed = subprocess.run(command, text=True, capture_output=True)
    print(completed.stdout, end="")
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr)
        raise RuntimeError(f"{label} failed with exit code {completed.returncode}")
    return completed


def read_openmp_summary(directory: Path, requested_bucket: int) -> list[dict[str, str]]:
    source = directory / "summary.csv"
    with source.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = [
            dict(row) for row in csv.DictReader(handle)
            if row.get("mode", row.get("version")) == "openmp"
        ]
    if not rows:
        raise ValueError(f"no OpenMP summary rows in {source}")
    for row in rows:
        row["requested_merge_buckets"] = str(requested_bucket)
        row["experiment_dir"] = str(directory)
    return rows


def normalized_value(field: str, value: str) -> object:
    if field in QUALITY_FIELDS:
        return round(float(value), 12)
    return value


def validate_sweep(rows: list[dict[str, str]], buckets: list[int],
                   threads: list[int]) -> list[str]:
    errors: list[str] = []
    expected_pairs = {(thread, bucket) for thread in threads for bucket in buckets}
    actual_pairs: set[tuple[int, int]] = set()
    semantic_groups: dict[int, dict[str, set[object]]] = defaultdict(
        lambda: defaultdict(set)
    )
    for row in rows:
        thread = int(row["threads"])
        requested = int(row["requested_merge_buckets"])
        actual = int(row["merge_buckets"])
        actual_pairs.add((thread, requested))
        if actual != requested:
            errors.append(
                f"threads={thread}, requested={requested}: program reported {actual} buckets"
            )
        expected_strategy = "bucket-serial" if requested == 1 else "bucket-parallel"
        if row.get("merge_strategy") != expected_strategy:
            errors.append(
                f"threads={thread}, buckets={requested}: expected {expected_strategy}, "
                f"got {row.get('merge_strategy')}"
            )
        for field in CHECK_FIELDS:
            semantic_groups[thread][field].add(normalized_value(field, row[field]))
    missing = sorted(expected_pairs - actual_pairs)
    extra = sorted(actual_pairs - expected_pairs)
    if missing:
        errors.append(f"missing configurations: {missing}")
    if extra:
        errors.append(f"unexpected configurations: {extra}")
    for thread, fields in sorted(semantic_groups.items()):
        for field, values in fields.items():
            if len(values) != 1:
                errors.append(
                    f"threads={thread}: {field} differs across bucket configurations: "
                    f"{sorted(values, key=str)}"
                )
    return errors


def enrich_rows(rows: list[dict[str, str]], baseline_bucket: int) -> None:
    baseline = {
        int(row["threads"]): row for row in rows
        if int(row["requested_merge_buckets"]) == baseline_bucket
    }
    by_thread: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        thread = int(row["threads"])
        by_thread[thread].append(row)
        merge = float(row["median_merge_ms"])
        algorithm = float(row["median_algorithm_ms"])
        compute = float(row["median_cooccur_compute_ms"])
        pipeline = float(row.get("median_cooccur_pipeline_ms", compute + merge))
        reference = baseline[thread]
        reference_merge = float(reference["median_merge_ms"])
        reference_algorithm = float(reference["median_algorithm_ms"])
        reference_pipeline = float(reference.get(
            "median_cooccur_pipeline_ms",
            float(reference["median_cooccur_compute_ms"]) + reference_merge,
        ))
        row["median_cooccur_pipeline_ms"] = f"{pipeline:.6f}"
        row["merge_share_percent"] = (
            f"{100.0 * merge / algorithm:.6f}" if algorithm > 0 else ""
        )
        row["merge_speedup_vs_baseline"] = (
            f"{reference_merge / merge:.6f}" if merge > 0 else ""
        )
        row["cooccur_pipeline_speedup_vs_baseline"] = (
            f"{reference_pipeline / pipeline:.6f}" if pipeline > 0 else ""
        )
        row["algorithm_speedup_vs_baseline"] = (
            f"{reference_algorithm / algorithm:.6f}" if algorithm > 0 else ""
        )
        row["algorithm_improvement_percent"] = (
            f"{100.0 * (reference_algorithm - algorithm) / reference_algorithm:.6f}"
            if reference_algorithm > 0 else ""
        )
        algorithm_stdev = float(row.get("stdev_algorithm_ms", 0.0))
        row["algorithm_cv_percent"] = (
            f"{100.0 * algorithm_stdev / algorithm:.6f}" if algorithm > 0 else ""
        )
        row["bucket_baseline"] = str(baseline_bucket)

    for thread_rows in by_thread.values():
        ranked = sorted(
            thread_rows,
            key=lambda row: (float(row["median_algorithm_ms"]),
                             int(row["requested_merge_buckets"])),
        )
        for rank, row in enumerate(ranked, start=1):
            row["algorithm_rank"] = str(rank)
            row["is_best_for_threads"] = "1" if rank == 1 else "0"


def write_comparison(path: Path, rows: list[dict[str, str]]) -> None:
    preferred = [
        "dataset", "algorithm_profile", "threads", "requested_merge_buckets",
        "merge_strategy", "merge_buckets", "runs",
        "median_cooccur_compute_ms", "median_merge_ms",
        "median_cooccur_pipeline_ms", "median_adjacency_ms",
        "median_recommend_ms", "median_algorithm_ms", "stdev_algorithm_ms",
        "algorithm_cv_percent", "median_end_to_end_ms",
        "merge_share_percent", "merge_speedup_vs_baseline",
        "cooccur_pipeline_speedup_vs_baseline",
        "algorithm_speedup_vs_baseline", "algorithm_improvement_percent",
        "algorithm_rank", "is_best_for_threads", "bucket_baseline",
        *QUALITY_FIELDS, "cooccur_checksum", "recommendation_checksum",
        "experiment_dir",
    ]
    available = set().union(*(row.keys() for row in rows))
    fields = [field for field in preferred if field in available]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(sorted(
            rows,
            key=lambda row: (int(row["threads"]),
                             int(row["requested_merge_buckets"])),
        ))


def write_report(path: Path, rows: list[dict[str, str]], dataset: str) -> None:
    lines = [
        "# Bucketed merge sweep report", "",
        f"Dataset: `{dataset}`", "",
        "The best configuration is selected by the lowest median `algorithm_ms`, "
        "not by `merge_ms` alone.", "",
        "| Threads | Best buckets | Algorithm ms | CV | Merge ms | Merge share | "
        "Algorithm improvement vs baseline |",
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    best_rows = sorted(
        (row for row in rows if row["is_best_for_threads"] == "1"),
        key=lambda row: int(row["threads"]),
    )
    for row in best_rows:
        lines.append(
            f"| {row['threads']} | {row['requested_merge_buckets']} | "
            f"{float(row['median_algorithm_ms']):.3f} | "
            f"{float(row['algorithm_cv_percent']):.2f}% | "
            f"{float(row['median_merge_ms']):.3f} | "
            f"{float(row['merge_share_percent']):.2f}% | "
            f"{float(row['algorithm_improvement_percent']):.2f}% |"
        )
    lines += ["", "## Correctness", "",
              "All bucket configurations passed cross-experiment checksum, graph, "
              "and recommendation-quality consistency checks.", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a merge-bucket sweep with cross-run validation and ranking"
    )
    parser.add_argument("--executable", type=Path,
                        default=Path("build/basket_recommender"))
    parser.add_argument("--data", type=Path, default=Path("data/large"))
    parser.add_argument("--dataset", default="large")
    parser.add_argument("--threads", default="24,32,48")
    parser.add_argument("--max-threads", type=int, default=48)
    parser.add_argument("--buckets", default="1,128,256,384")
    parser.add_argument("--baseline-buckets", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--serial-repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--max-neighbors", type=int, default=50)
    parser.add_argument("--cooccur-schedule",
                        choices=("static", "dynamic", "guided"), default="dynamic")
    parser.add_argument("--cooccur-chunk", type=int, default=64)
    parser.add_argument("--recommend-schedule",
                        choices=("static", "dynamic", "guided"), default="dynamic")
    parser.add_argument("--recommend-chunk", type=int, default=16)
    parser.add_argument("--proc-bind", default="spread")
    parser.add_argument("--places", default="cores")
    parser.add_argument("--seed", type=int, default=20260718)
    parser.add_argument("--ordered", action="store_true")
    parser.add_argument("--correctness", action="store_true",
                        help="run the project correctness gate before the sweep")
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--build-command", default="make release")
    parser.add_argument("--output-root", type=Path,
                        default=Path("results/bucket-sweeps"))
    args = parser.parse_args()

    buckets = parse_int_list(args.buckets)
    threads = parse_int_list(args.threads)
    if buckets[-1] > 4096:
        parser.error("bucket count cannot exceed 4096")
    if args.baseline_buckets not in buckets:
        parser.error("--baseline-buckets must be included in --buckets")
    if max(threads) > args.max_threads:
        parser.error("requested thread count exceeds --max-threads")
    if min(args.repeats, args.serial_repeats) < 1 or args.warmups < 0:
        parser.error("repeats must be positive and warmups cannot be negative")

    run_id = datetime.now().strftime("%Y%m%d-%H%M%S%f")[:-3]
    sweep_dir = (args.output_root /
                 f"{run_id}-{args.dataset}-top{args.max_neighbors}").resolve()
    experiment_root = sweep_dir / "experiments"
    experiment_root.mkdir(parents=True, exist_ok=False)
    runner = Path(__file__).resolve().with_name("run_platform_benchmark.py")
    experiment_dirs: list[tuple[int, Path]] = []

    try:
        if args.correctness:
            command = [
                sys.executable, str(runner), "--correctness",
                "--executable", str(args.executable),
                "--build-command", args.build_command,
            ]
            run_command(command, "correctness gate")

        for index, bucket in enumerate(buckets):
            command = [
                sys.executable, str(runner),
                "--executable", str(args.executable),
                "--data", str(args.data), "--dataset", args.dataset,
                "--threads", args.threads, "--max-threads", str(args.max_threads),
                "--repeats", str(args.repeats),
                "--serial-repeats", str(args.serial_repeats),
                "--warmups", str(args.warmups),
                "--top-k", str(args.top_k),
                "--max-neighbors", str(args.max_neighbors),
                "--cooccur-schedule", args.cooccur_schedule,
                "--cooccur-chunk", str(args.cooccur_chunk),
                "--merge-buckets", str(bucket),
                "--recommend-schedule", args.recommend_schedule,
                "--recommend-chunk", str(args.recommend_chunk),
                "--proc-bind", args.proc_bind, "--places", args.places,
                "--seed", str(args.seed),
                "--output-root", str(experiment_root), "--no-plot",
                "--build-command", (
                    args.build_command if index == 0 and not args.correctness else ""
                ),
            ]
            if args.ordered:
                command.append("--ordered")
            completed = run_command(command, f"merge buckets: {bucket}")
            marker = "EXPERIMENT_DIR="
            line = next((item for item in completed.stdout.splitlines()
                         if item.startswith(marker)), None)
            if line is None:
                raise RuntimeError("platform runner did not report EXPERIMENT_DIR")
            experiment_dirs.append(
                (bucket, resolve_reported_path(line[len(marker):]))
            )

        combined: list[dict[str, str]] = []
        for requested_bucket, directory in experiment_dirs:
            combined.extend(read_openmp_summary(directory, requested_bucket))

        errors = validate_sweep(combined, buckets, threads)
        validation = {
            "schema_version": 1,
            "created_at": datetime.now(timezone.utc).astimezone().isoformat(),
            "status": "pass" if not errors else "fail",
            "row_count": len(combined),
            "configurations": len(buckets) * len(threads),
            "checks": list(CHECK_FIELDS),
            "errors": errors,
        }
        validation_path = sweep_dir / "validation.json"
        validation_path.write_text(
            json.dumps(validation, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        if errors:
            for error in errors:
                print(error, file=sys.stderr)
            return 1

        enrich_rows(combined, args.baseline_buckets)
        comparison = sweep_dir / "bucket-comparison.csv"
        report = sweep_dir / "bucket-report.md"
        write_comparison(comparison, combined)
        write_report(report, combined, args.dataset)

        figures = sweep_dir / "figures"
        if not args.no_plot:
            plotter = Path(__file__).resolve().with_name("plot_bucket_sweep.py")
            plot_result = subprocess.run(
                [sys.executable, str(plotter), str(comparison), str(figures)],
                check=False,
            )
            if plot_result.returncode != 0:
                print("warning: bucket sweep plotting failed", file=sys.stderr)

        manifest = {
            "schema_version": 2,
            "run_id": run_id,
            "created_at": datetime.now(timezone.utc).astimezone().isoformat(),
            "dataset": args.dataset,
            "threads": threads,
            "buckets": buckets,
            "bucket_baseline": args.baseline_buckets,
            "parameters": {
                key: str(value) if isinstance(value, Path) else value
                for key, value in vars(args).items()
            },
            "experiments": [
                {"merge_buckets": bucket, "directory": str(directory)}
                for bucket, directory in experiment_dirs
            ],
            "files": {
                "comparison": str(comparison),
                "validation": str(validation_path),
                "report": str(report),
                "figures": str(figures),
            },
        }
        (sweep_dir / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(f"BUCKET_SWEEP_DIR={sweep_dir}")
        print(f"BUCKET_COMPARISON={comparison}")
        print(f"BUCKET_REPORT={report}")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"bucket sweep failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())