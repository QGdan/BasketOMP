"""Summarize validated benchmark CSV files with reproducible statistics."""

from __future__ import annotations

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path


TIMING_FIELDS = (
    "load_ms", "cooccur_compute_ms", "merge_ms", "normalization_ms",
    "adjacency_ms", "truncate_ms", "recommend_ms", "evaluate_ms",
    "algorithm_ms", "end_to_end_ms",
)


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: summarize_results.py INPUT.csv [OUTPUT.csv]", file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    target = Path(sys.argv[2]) if len(sys.argv) == 3 else source.with_name(
        "summary.csv"
    )
    with source.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError("benchmark CSV is empty")

    groups: dict[tuple[str, ...], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        mode = row.get("mode", row.get("version", ""))
        key = (
            row["dataset"], row.get("algorithm_profile", "legacy"), mode,
            row["threads"], row.get("top_k", "10"),
            row.get("max_neighbors", "0"),
            row.get("cooccur_schedule", row.get("schedule", "none")),
            row.get("cooccur_chunk", "0"),
            row.get("merge_strategy", "legacy"),
            row.get("merge_buckets", "0"),
            row.get("recommend_schedule", "none"),
            row.get("recommend_chunk", "0"),
        )
        groups[key].append(row)

    serial_medians: dict[tuple[str, str, str, str], dict[str, float]] = {}
    openmp1_medians: dict[tuple[str, str, str, str, str, str], dict[str, float]] = {}
    for key, group in groups.items():
        dataset, profile, mode, threads, top_k, max_neighbors, *_ = key
        medians = {
            field: statistics.median(float(row.get(field, 0.0)) for row in group)
            for field in TIMING_FIELDS
        }
        if mode == "serial":
            serial_medians[(dataset, profile, top_k, max_neighbors)] = medians
        elif mode == "openmp" and int(threads) == 1:
            merge_strategy, merge_buckets = key[8], key[9]
            openmp1_medians[(dataset, profile, top_k, max_neighbors,
                             merge_strategy, merge_buckets)] = medians

    output_rows: list[dict[str, object]] = []
    for key in sorted(groups):
        (dataset, profile, mode, threads_text, top_k, max_neighbors,
         cooccur_schedule, cooccur_chunk, merge_strategy, merge_buckets,
         recommend_schedule, recommend_chunk) = key
        group = groups[key]
        threads = int(threads_text)
        summary: dict[str, object] = {
            "dataset": dataset,
            "algorithm_profile": profile,
            "version": mode,
            "mode": mode,
            "threads": threads,
            "top_k": top_k,
            "max_neighbors": max_neighbors,
            "cooccur_schedule": cooccur_schedule,
            "cooccur_chunk": cooccur_chunk,
            "merge_strategy": merge_strategy,
            "merge_buckets": merge_buckets,
            "recommend_schedule": recommend_schedule,
            "recommend_chunk": recommend_chunk,
            "runs": len(group),
        }
        for field in TIMING_FIELDS:
            values = [float(row.get(field, 0.0)) for row in group]
            summary[f"median_{field}"] = statistics.median(values)
            summary[f"min_{field}"] = min(values)
            summary[f"max_{field}"] = max(values)
            summary[f"stdev_{field}"] = statistics.stdev(values) if len(values) > 1 else 0.0
        pipeline_values = [
            float(row.get("cooccur_compute_ms", 0.0)) +
            float(row.get("merge_ms", 0.0)) for row in group
        ]
        summary["median_cooccur_pipeline_ms"] = statistics.median(pipeline_values)
        algorithm_median = float(summary["median_algorithm_ms"])
        summary["merge_fraction"] = (
            float(summary["median_merge_ms"]) / algorithm_median
            if algorithm_median > 0 else 0.0
        )
        summary["cooccur_pipeline_fraction"] = (
            float(summary["median_cooccur_pipeline_ms"]) / algorithm_median
            if algorithm_median > 0 else 0.0
        )
        serial_key = (dataset, profile, top_k, max_neighbors)
        if serial_key not in serial_medians:
            raise ValueError(f"missing matching serial baseline for {serial_key}")
        serial_stats = serial_medians[serial_key]
        serial_time = serial_stats["algorithm_ms"]
        algorithm_time = float(summary["median_algorithm_ms"])
        speedup = serial_time / algorithm_time if algorithm_time > 0 else 0.0
        summary["speedup"] = speedup
        summary["efficiency"] = speedup / threads if mode == "openmp" else 1.0
        summary["speedup_vs_serial"] = speedup
        summary["efficiency_vs_serial"] = (
            speedup / threads if mode == "openmp" else 1.0
        )
        for stage in ("cooccur_compute_ms", "adjacency_ms", "recommend_ms"):
            current = float(summary[f"median_{stage}"])
            summary[f"{stage[:-3]}_speedup_vs_serial"] = (
                serial_stats[stage] / current if current > 0 else 0.0
            )
        serial_pipeline = serial_stats["cooccur_compute_ms"] + serial_stats["merge_ms"]
        current_pipeline = float(summary["median_cooccur_pipeline_ms"])
        summary["cooccur_pipeline_speedup_vs_serial"] = (
            serial_pipeline / current_pipeline if current_pipeline > 0 else 0.0
        )
        openmp1_key = (dataset, profile, top_k, max_neighbors,
                       merge_strategy, merge_buckets)
        openmp1 = openmp1_medians.get(openmp1_key)
        if mode == "openmp" and openmp1 is not None:
            openmp1_speedup = (
                openmp1["algorithm_ms"] / algorithm_time
                if algorithm_time > 0 else 0.0
            )
            summary["speedup_vs_openmp1"] = openmp1_speedup
            summary["efficiency_vs_openmp1"] = openmp1_speedup / threads
        else:
            summary["speedup_vs_openmp1"] = ""
            summary["efficiency_vs_openmp1"] = ""
        if mode == "openmp" and threads > 1 and speedup > 0:
            summary["karp_flatt"] = ((1.0 / speedup) - (1.0 / threads)) / (
                1.0 - (1.0 / threads)
            )
        else:
            summary["karp_flatt"] = ""
        summary["serial_adjacency_fraction"] = (
            serial_stats["adjacency_ms"] / serial_time if serial_time > 0 else 0.0
        )
        for field in (
            "hardware_threads", "omp_max_threads", "graph_edge_entries",
            "max_degree", "active_users",
            "candidate_shortage_users", "empty_candidate_users",
            "total_candidates", "max_candidates", "hit_rate", "precision",
            "recall", "f1", "ndcg", "mrr", "micro_precision",
            "micro_recall",
            "cooccur_checksum", "recommendation_checksum",
        ):
            summary[field] = group[0].get(field, "")
        output_rows.append(summary)

    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(output_rows[0]))
        writer.writeheader()
        writer.writerows(output_rows)
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
