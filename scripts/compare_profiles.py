"""Compare full-graph and Top-N experiment summaries and draw trade-off charts."""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt

plt.rcParams["axes.unicode_minus"] = False


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: compare_profiles.py OUTPUT_DIR SUMMARY.csv SUMMARY.csv ...",
              file=sys.stderr)
        return 2
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, object]] = []
    for name in sys.argv[2:]:
        with Path(name).open("r", encoding="utf-8-sig", newline="") as handle:
            rows = list(csv.DictReader(handle))
        serial = next(row for row in rows if row.get("mode") == "serial")
        parallel_rows = [row for row in rows if row.get("mode") == "openmp"]
        parallel = max(parallel_rows, key=lambda row: int(row["threads"]))
        records.append({
            "dataset": parallel["dataset"],
            "max_neighbors": int(parallel["max_neighbors"]),
            "threads": int(parallel["threads"]),
            "graph_edge_entries": int(parallel["graph_edge_entries"]),
            "serial_algorithm_ms": float(serial["median_algorithm_ms"]),
            "parallel_algorithm_ms": float(parallel["median_algorithm_ms"]),
            "parallel_recommend_ms": float(parallel["median_recommend_ms"]),
            "hit_rate": float(parallel["hit_rate"]),
            "recall": float(parallel["recall"]),
            "within_profile_speedup": float(parallel["speedup"]),
        })
    records.sort(key=lambda row: (row["max_neighbors"] != 0,
                                  int(row["max_neighbors"])))
    baseline = next((row for row in records if row["max_neighbors"] == 0), None)
    if baseline is None:
        raise ValueError("comparison requires a max_neighbors=0 full-graph summary")
    baseline_serial = float(baseline["serial_algorithm_ms"])
    baseline_hit = float(baseline["hit_rate"])
    baseline_recall = float(baseline["recall"])
    for row in records:
        row["combined_speedup_vs_full_serial"] = (
            baseline_serial / float(row["parallel_algorithm_ms"])
        )
        row["hit_rate_delta"] = float(row["hit_rate"]) - baseline_hit
        row["recall_delta"] = float(row["recall"]) - baseline_recall

    target = output / "profile-comparison.csv"
    with target.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(records[0]))
        writer.writeheader(); writer.writerows(records)

    labels = ["Full" if row["max_neighbors"] == 0 else f"Top-{row['max_neighbors']}"
              for row in records]
    algorithm = [float(row["parallel_algorithm_ms"]) for row in records]
    recommend = [float(row["parallel_recommend_ms"]) for row in records]
    hit_delta = [100.0 * float(row["hit_rate_delta"]) for row in records]
    recall_delta = [100.0 * float(row["recall_delta"]) for row in records]
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.5))
    x = list(range(len(labels)))
    axes[0].bar(x, algorithm, label="Algorithm")
    axes[0].bar(x, recommend, label="Recommendation")
    axes[0].set_xticks(x, labels)
    axes[0].set_ylabel("Median runtime (ms)")
    axes[0].set_title("Performance")
    axes[0].legend()
    axes[0].grid(axis="y", alpha=0.25)
    axes[1].plot(x, hit_delta, marker="o", label="Hit Rate delta")
    axes[1].plot(x, recall_delta, marker="o", label="Recall delta")
    axes[1].axhline(-1.0, linestyle="--", color="gray", label="-1 pp threshold")
    axes[1].set_xticks(x, labels)
    axes[1].set_ylabel("Absolute change (percentage points)")
    axes[1].set_title("Recommendation quality vs Full")
    axes[1].legend()
    axes[1].grid(alpha=0.25)
    figure.tight_layout()
    figure.savefig(output / "profile-tradeoff.png", dpi=180)
    plt.close(figure)
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
