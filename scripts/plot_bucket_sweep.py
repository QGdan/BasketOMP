"""Generate report-ready figures for a merge-bucket sweep."""

from __future__ import annotations

import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

plt.rcParams["axes.unicode_minus"] = False


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError("bucket comparison CSV is empty")
    return rows


def plot_lines(groups: dict[int, list[dict[str, str]]], field: str,
               ylabel: str, title: str, target: Path,
               percentage: bool = False) -> None:
    plt.figure(figsize=(7.5, 4.8))
    for threads, rows in sorted(groups.items()):
        ordered = sorted(rows, key=lambda row: int(row["requested_merge_buckets"]))
        x = [int(row["requested_merge_buckets"]) for row in ordered]
        y = [float(row[field]) for row in ordered]
        if percentage:
            y = [100.0 * value for value in y]
        plt.plot(x, y, marker="o", linewidth=2, label=f"{threads} threads")
    plt.xlabel("Merge buckets")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(target, dpi=180)
    plt.close()


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: plot_bucket_sweep.py COMPARISON.csv [OUTPUT_DIR]",
              file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    output = Path(sys.argv[2]) if len(sys.argv) == 3 else source.with_name("figures")
    rows = load_rows(source)
    groups: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        groups[int(row["threads"])].append(row)
    output.mkdir(parents=True, exist_ok=True)

    plot_lines(groups, "median_merge_ms", "Median merge time (ms)",
               "Bucket count vs merge time", output / "bucket-merge-ms.png")
    plot_lines(groups, "median_cooccur_pipeline_ms",
               "Median compute + merge time (ms)",
               "Bucket count vs cooccurrence pipeline",
               output / "bucket-cooccur-pipeline-ms.png")
    plot_lines(groups, "median_algorithm_ms", "Median algorithm time (ms)",
               "Bucket count vs total algorithm time",
               output / "bucket-algorithm-ms.png")
    plot_lines(groups, "algorithm_speedup_vs_baseline", "Speedup vs baseline",
               "Algorithm speedup relative to baseline buckets",
               output / "bucket-algorithm-speedup.png")
    plot_lines(groups, "merge_share_percent", "Merge share (%)",
               "Merge bottleneck share", output / "bucket-merge-share.png")

    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())