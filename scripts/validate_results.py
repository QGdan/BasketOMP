"""Validate structured benchmark CSV files before summarizing or plotting."""

from __future__ import annotations

import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path


REQUIRED_FIELDS = (
    "run_id", "timestamp", "dataset", "algorithm_profile", "mode",
    "threads", "repeat", "warmup", "top_k", "max_neighbors",
    "popular_fallback", "cooccur_schedule", "cooccur_chunk",
    "merge_strategy", "merge_buckets",
    "recommend_schedule", "recommend_chunk", "orders", "prior_rows",
    "train_rows", "products", "users", "hardware_threads",
    "omp_max_threads", "unique_pairs", "pair_events",
    "graph_edge_entries", "max_degree", "active_users",
    "candidate_shortage_users", "empty_candidate_users",
    "total_candidates", "max_candidates", "load_ms",
    "cooccur_compute_ms", "merge_ms", "normalization_ms", "adjacency_ms",
    "truncate_ms", "recommend_ms", "evaluate_ms", "algorithm_ms",
    "end_to_end_ms", "hit_rate", "precision", "recall", "f1",
    "ndcg", "mrr", "micro_precision", "micro_recall", "cooccur_checksum",
    "recommendation_checksum", "status",
)

INTEGER_FIELDS = (
    "threads", "repeat", "warmup", "top_k", "max_neighbors",
    "popular_fallback", "cooccur_chunk", "merge_buckets",
    "recommend_chunk", "orders",
    "prior_rows", "train_rows", "products", "users", "hardware_threads",
    "omp_max_threads", "unique_pairs",
    "pair_events", "graph_edge_entries", "max_degree", "active_users",
    "candidate_shortage_users", "empty_candidate_users",
    "total_candidates", "max_candidates",
)

TIMING_FIELDS = (
    "load_ms", "cooccur_compute_ms", "merge_ms", "normalization_ms",
    "adjacency_ms", "truncate_ms", "recommend_ms", "evaluate_ms",
    "algorithm_ms", "end_to_end_ms",
)


def validate_rows(rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    if not rows:
        return ["benchmark CSV is empty"]
    for row in rows:
        row.setdefault("merge_strategy", "legacy")
        row.setdefault("merge_buckets", "0")
    missing = [field for field in REQUIRED_FIELDS if field not in rows[0]]
    if missing:
        return [f"missing required fields: {', '.join(missing)}"]

    parsed: list[dict[str, object]] = []
    for index, row in enumerate(rows, start=2):
        item: dict[str, object] = dict(row)
        for field in REQUIRED_FIELDS:
            if row.get(field, "") == "":
                errors.append(f"line {index}: empty field {field}")
        try:
            for field in INTEGER_FIELDS:
                item[field] = int(row[field])
            for field in TIMING_FIELDS + (
                "hit_rate", "precision", "recall", "f1", "ndcg", "mrr",
                "micro_precision", "micro_recall",
            ):
                item[field] = float(row[field])
        except ValueError as exc:
            errors.append(f"line {index}: invalid numeric value: {exc}")
            continue

        if row["status"] != "ok":
            errors.append(f"line {index}: status is not ok")
        if item["threads"] < 1 or item["repeat"] < 1 or item["top_k"] < 1:
            errors.append(f"line {index}: threads/repeat/top_k must be positive")
        if (item["max_neighbors"] < 0 or item["warmup"] < 0 or
                item["merge_buckets"] < 0):
            errors.append(
                f"line {index}: max_neighbors/warmup/merge_buckets cannot be negative"
            )
        mode = str(item["mode"])
        strategy = str(item["merge_strategy"])
        buckets = int(item["merge_buckets"])
        if buckets > 4096:
            errors.append(f"line {index}: merge_buckets exceeds 4096")
        if strategy != "legacy":
            if mode == "serial" and (strategy != "serial" or buckets != 0):
                errors.append(
                    f"line {index}: serial mode requires merge_strategy=serial "
                    "and merge_buckets=0"
                )
            elif mode == "openmp":
                valid = ((strategy == "bucket-serial" and buckets == 1) or
                         (strategy == "bucket-parallel" and buckets > 1))
                if not valid:
                    errors.append(
                        f"line {index}: inconsistent OpenMP merge metadata "
                        f"({strategy}, {buckets})"
                    )
        for field in TIMING_FIELDS:
            value = item[field]
            if not math.isfinite(value) or value < 0:
                errors.append(f"line {index}: invalid timing {field}={value}")
        for field in (
            "hit_rate", "precision", "recall", "f1", "ndcg", "mrr",
            "micro_precision", "micro_recall",
        ):
            value = item[field]
            if not math.isfinite(value) or not 0.0 <= value <= 1.0:
                errors.append(f"line {index}: {field} outside [0,1]")
        if item["hardware_threads"] < 1 or item["omp_max_threads"] < 1:
            errors.append(f"line {index}: invalid OpenMP thread capacity")

        stage_sum = (item["cooccur_compute_ms"] + item["merge_ms"] +
                     item["adjacency_ms"] + item["recommend_ms"])
        tolerance = max(5.0, 0.02 * max(item["algorithm_ms"], 1.0))
        if abs(item["algorithm_ms"] - stage_sum) > tolerance:
            errors.append(
                f"line {index}: algorithm_ms does not match stage sum "
                f"({item['algorithm_ms']} vs {stage_sum})"
            )
        if item["end_to_end_ms"] + tolerance < item["algorithm_ms"]:
            errors.append(f"line {index}: end_to_end_ms smaller than algorithm_ms")
        if item["active_users"] + item["empty_candidate_users"] != item["users"]:
            errors.append(f"line {index}: candidate user counts do not sum to users")
        if item["candidate_shortage_users"] > item["active_users"]:
            errors.append(f"line {index}: shortage users exceed active users")
        if item["max_neighbors"] > 0 and item["max_degree"] > item["max_neighbors"]:
            errors.append(f"line {index}: max degree exceeds max_neighbors")
        if item["max_neighbors"] == 0 and (
            item["graph_edge_entries"] != 2 * item["unique_pairs"]
        ):
            errors.append(f"line {index}: full graph edge count mismatch")
        parsed.append(item)

    repeat_groups: dict[tuple[object, ...], set[int]] = defaultdict(set)
    semantic_groups: dict[tuple[object, ...], set[tuple[str, str]]] = defaultdict(set)
    for item in parsed:
        config = (
            item["run_id"], item["dataset"], item["algorithm_profile"],
            item["mode"], item["threads"], item["top_k"],
            item["max_neighbors"], item["cooccur_schedule"],
            item["cooccur_chunk"], item["merge_strategy"],
            item["merge_buckets"], item["recommend_schedule"],
            item["recommend_chunk"],
        )
        repeat = int(item["repeat"])
        if repeat in repeat_groups[config]:
            errors.append(f"duplicate repeat {repeat} for config {config}")
        repeat_groups[config].add(repeat)
        semantics = (
            item["run_id"], item["dataset"], item["top_k"],
            item["max_neighbors"], item["popular_fallback"],
        )
        semantic_groups[semantics].add(
            (str(item["cooccur_checksum"]), str(item["recommendation_checksum"]))
        )

    for semantics, checksums in semantic_groups.items():
        if len(checksums) != 1:
            errors.append(f"checksum mismatch for semantic config {semantics}")
    return errors


def self_test() -> int:
    base = {field: "1" for field in REQUIRED_FIELDS}
    base.update({
        "run_id": "self-test", "timestamp": "2026-01-01T00:00:00",
        "dataset": "toy", "algorithm_profile": "test", "mode": "serial",
        "cooccur_schedule": "none", "merge_strategy": "serial",
        "merge_buckets": "0", "recommend_schedule": "none",
        "status": "ok", "max_neighbors": "0", "warmup": "0",
        "unique_pairs": "3", "graph_edge_entries": "6", "users": "4",
        "active_users": "2", "empty_candidate_users": "2",
        "candidate_shortage_users": "1", "cooccur_compute_ms": "2",
        "merge_ms": "0", "adjacency_ms": "3", "recommend_ms": "5",
        "algorithm_ms": "10", "end_to_end_ms": "12", "load_ms": "1",
        "normalization_ms": "1", "truncate_ms": "0", "evaluate_ms": "1",
        "hardware_threads": "8", "omp_max_threads": "8",
        "hit_rate": "0.5", "precision": "0.2", "recall": "0.4",
        "f1": "0.25", "ndcg": "0.4", "mrr": "0.5",
        "micro_precision": "0.2", "micro_recall": "0.4",
        "cooccur_checksum": "10",
        "recommendation_checksum": "20",
    })
    if validate_rows([base.copy()]):
        print("self-test valid row was rejected", file=sys.stderr)
        return 1
    cases = []
    missing = base.copy(); missing.pop("status"); cases.append([missing])
    bad_metric = base.copy(); bad_metric["recall"] = "1.5"; cases.append([bad_metric])
    bad_edges = base.copy(); bad_edges["graph_edge_entries"] = "5"; cases.append([bad_edges])
    duplicate = [base.copy(), base.copy()]; cases.append(duplicate)
    bad_merge = base.copy(); bad_merge.update({
        "mode": "openmp", "merge_strategy": "bucket-parallel",
        "merge_buckets": "1",
    }); cases.append([bad_merge])
    too_many_buckets = base.copy(); too_many_buckets.update({
        "mode": "openmp", "merge_strategy": "bucket-parallel",
        "merge_buckets": "4097",
    }); cases.append([too_many_buckets])
    for number, rows in enumerate(cases, start=1):
        if not validate_rows(rows):
            print(f"self-test invalid case {number} was accepted", file=sys.stderr)
            return 1
    print("PASS: result validator self-test")
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        return self_test()
    if len(sys.argv) not in (2, 3):
        print("usage: validate_results.py INPUT.csv [OUTPUT.json]", file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    target = Path(sys.argv[2]) if len(sys.argv) == 3 else source.with_name(
        "validation.json"
    )
    with source.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    errors = validate_rows(rows)
    report = {
        "schema_version": 1,
        "source": str(source),
        "row_count": len(rows),
        "status": "pass" if not errors else "fail",
        "errors": errors,
    }
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                      encoding="utf-8")
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
