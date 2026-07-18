"""Generate report-ready figures from one structured summary CSV."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt

plt.rcParams["axes.unicode_minus"] = False


def safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-")


def save_line(x, y, title, ylabel, target: Path, ideal=None,
              measured_label="Measured") -> None:
    positions = list(range(len(x)))
    plt.figure(figsize=(7, 4.5))
    plt.plot(positions, y, marker="o", linewidth=2, label=measured_label)
    if ideal is not None:
        plt.plot(positions, ideal, linestyle="--", color="gray", label="Ideal")
        plt.legend()
    plt.xticks(positions, x)
    plt.xlabel("OpenMP threads")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(target, dpi=180)
    plt.close()


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: plot_results.py SUMMARY.csv [OUTPUT_DIR]", file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    output = Path(sys.argv[2]) if len(sys.argv) == 3 else source.with_name("figures")
    with source.open("r", encoding="utf-8-sig", newline="") as handle:
        all_rows = list(csv.DictReader(handle))
    rows = sorted(
        [row for row in all_rows if row.get("mode", row.get("version")) == "openmp"],
        key=lambda row: int(row["threads"]),
    )
    if not rows:
        raise ValueError("summary contains no OpenMP rows")
    output.mkdir(parents=True, exist_ok=True)
    dataset = rows[0]["dataset"]
    profile = rows[0].get("algorithm_profile", "legacy")
    prefix = f"{safe_name(dataset)}-{safe_name(profile)}"
    threads = [int(row["threads"]) for row in rows]
    runtimes = [float(row["median_algorithm_ms"]) for row in rows]
    speedups = [float(row["speedup"]) for row in rows]
    efficiencies = [100.0 * float(row["efficiency"]) for row in rows]
    title = f"{dataset}: {profile}"

    save_line(threads, runtimes, f"{title} runtime", "Median runtime (ms)",
              output / f"{prefix}-runtime.png")
    save_line(threads, speedups, f"{title} speedup", "Speedup",
              output / f"{prefix}-speedup.png", ideal=threads)
    save_line(threads, efficiencies, f"{title} efficiency", "Efficiency (%)",
              output / f"{prefix}-efficiency.png")

    # 分阶段加速比能直接解释“总体加速比为何变小”：若推荐阶段已经
    # 很短，邻接表等弱并行阶段就会主导总时间。
    stage_fields = (
        ("cooccur_compute_speedup_vs_serial", "Cooccurrence"),
        ("adjacency_speedup_vs_serial", "Adjacency"),
        ("recommend_speedup_vs_serial", "Recommendation"),
    )
    positions = list(range(len(threads)))
    plt.figure(figsize=(7, 4.5))
    for field, label in stage_fields:
        values = [float(row[field]) for row in rows]
        plt.plot(positions, values, marker="o", linewidth=2, label=label)
    plt.xticks(positions, threads)
    plt.xlabel("OpenMP threads")
    plt.ylabel("Speedup vs serial")
    plt.title(f"{title} speedup by stage")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(output / f"{prefix}-stage-speedup.png", dpi=180)
    plt.close()

    cooccur = [float(row["median_cooccur_compute_ms"]) for row in rows]
    merge = [float(row["median_merge_ms"]) for row in rows]
    adjacency = [float(row["median_adjacency_ms"]) for row in rows]
    recommend = [float(row["median_recommend_ms"]) for row in rows]
    plt.figure(figsize=(7, 4.5))
    positions = list(range(len(threads)))
    plt.bar(positions, cooccur, label="Cooccurrence compute")
    plt.bar(positions, merge, bottom=cooccur, label="Merge")
    bottom = [a + b for a, b in zip(cooccur, merge)]
    plt.bar(positions, adjacency, bottom=bottom, label="Adjacency")
    bottom = [a + b for a, b in zip(bottom, adjacency)]
    plt.bar(positions, recommend, bottom=bottom, label="Recommendation")
    plt.xticks(positions, threads)
    plt.xlabel("OpenMP threads")
    plt.ylabel("Median runtime (ms)")
    plt.title(f"{title} runtime by stage")
    plt.legend()
    plt.tight_layout()
    plt.savefig(output / f"{prefix}-stages.png", dpi=180)
    plt.close()
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
