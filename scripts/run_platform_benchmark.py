"""跨平台自动基准测试入口 —— 学校多核实验平台首选脚本。

支持功能:
  --auto-config       自动检测平台，选择合理默认配置
  --batch             批量运行多组 (数据集, max_neighbors) 组合
  --correctness       实验前运行正确性门
  --all               一键完整实验（正确性 + small + medium + large）
  --report            生成 Markdown 汇总报告

用法示例:
  # 单次实验（自动检测线程数）
  python scripts/run_platform_benchmark.py --auto-config --data data/medium

  # 批量 Top-N sweep
  python scripts/run_platform_benchmark.py --batch --data data/medium \\
      --max-neighbors 0,20,50,100

  # 一键完整实验（含正确性验证）
  python scripts/run_platform_benchmark.py --all --max-threads 48

  # 仅运行正确性门
  python scripts/run_platform_benchmark.py --correctness
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import random
import shlex
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


# ── 结构化输出字段 ────────────────────────
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

# ── 自动平台检测 ──────────────────────────
def detect_platform() -> dict:
    """检测硬件和软件环境，返回适合的默认配置。"""
    cpu_count = os.cpu_count() or 1
    info = {
        "logical_processors": cpu_count,
        "platform": platform.platform(),
        "hostname": platform.node(),
        "processor": platform.processor(),
        "python": platform.python_version(),
    }

    # 检测物理核心数
    try:
        import psutil
        info["physical_cores"] = psutil.cpu_count(logical=False)
        info["total_memory_gib"] = round(psutil.virtual_memory().total / (1024**3), 1)
    except ImportError:
        info["physical_cores"] = None
        info["total_memory_gib"] = None

    # 检测编译器
    for cc in ["gcc", "clang", "cc"]:
        if shutil.which(cc):
            result = subprocess.run([cc, "--version"], capture_output=True, text=True)
            info["compiler"] = result.stdout.splitlines()[0] if result.stdout else cc
            info["cc_path"] = shutil.which(cc)
            break

    # 根据核心数推荐线程序列
    if cpu_count >= 48:
        info["recommended_threads"] = [1, 2, 4, 8, 12, 16, 24, 32, 48]
        info["recommended_proc_bind"] = "spread"
        info["recommended_places"] = "cores"
    elif cpu_count >= 24:
        info["recommended_threads"] = [1, 2, 4, 8, 12, 16, 24]
        info["recommended_proc_bind"] = "spread"
        info["recommended_places"] = "cores"
    elif cpu_count >= 16:
        info["recommended_threads"] = [1, 2, 4, 8, 16]
        info["recommended_proc_bind"] = "spread"
        info["recommended_places"] = "cores"
    elif cpu_count >= 8:
        info["recommended_threads"] = [1, 2, 4, 8]
        info["recommended_proc_bind"] = "spread"
        info["recommended_places"] = "cores"
    else:
        info["recommended_threads"] = [1, 2, 4]
        info["recommended_proc_bind"] = "close"
        info["recommended_places"] = "threads"

    return info


# ── 线程数解析 ────────────────────────────
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
        raise ValueError(f"线程数必须在 1..{limit} 之间")
    return result


# ── 程序输出解析 ──────────────────────────
def parse_key_values(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def required(values: dict[str, str], key: str) -> str:
    if key not in values:
        raise RuntimeError(f"程序输出缺少必要字段: {key}")
    return values[key]


# ── 运行单次基准 ──────────────────────────
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
            f"基准测试失败 ({mode}, {threads} 线程):\n{completed.stderr}"
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
        "proc_bind": args.proc_bind,
        "places": args.places,
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


# ── 实验索引管理 ──────────────────────────
def update_experiment_index(output_root: Path, experiment_dir: Path,
                            config: dict) -> None:
    """维护 experiments/index.json 汇总所有实验。"""
    index_path = output_root / "index.json"
    if index_path.exists():
        index = json.loads(index_path.read_text(encoding="utf-8"))
    else:
        index = {"schema_version": 1, "experiments": []}

    index["experiments"].append({
        "run_id": config["run_id"],
        "created_at": config["created_at"],
        "dataset": config["dataset"],
        "profile": config["profile"],
        "threads": config["threads"],
        "max_neighbors": config["max_neighbors"],
        "directory": str(experiment_dir.relative_to(output_root)),
        "platform": platform.platform(),
        "hostname": platform.node(),
    })
    index_path.write_text(json.dumps(index, ensure_ascii=False, indent=2),
                          encoding="utf-8")


# ── 单次实验 ──────────────────────────────
def run_single_experiment(args: argparse.Namespace) -> Path:
    """执行一次完整的基准实验，返回实验目录路径。"""
    detected = os.cpu_count() or 1
    threads = parse_threads(args.threads, args.max_threads)

    if max(threads) > detected:
        print(
            f"⚠ 请求 {max(threads)} 线程但 OS 报告 {detected} 个逻辑处理器",
            file=sys.stderr,
        )

    # ── 编译 ──────────────────────────────
    if args.build_command:
        print(f"  编译: {args.build_command}")
        subprocess.run(args.build_command, shell=True, check=True)

    if not args.executable.exists():
        alternative = Path("build/basket_recommender")
        if args.executable.name.endswith(".exe") and alternative.exists():
            args.executable = alternative
        else:
            # 尝试无后缀
            alt_noext = Path(str(args.executable).replace(".exe", ""))
            if alt_noext.exists():
                args.executable = alt_noext
            else:
                raise FileNotFoundError(f"可执行文件未找到: {args.executable}")

    # ── 实验目录 ──────────────────────────
    profile = ("fast-normalization-full" if args.max_neighbors == 0
               else f"fast-normalization-top{args.max_neighbors}")
    run_id = datetime.now().strftime("%Y%m%d-%H%M%S%f")[:-3]
    run_id += f"-{args.dataset}-{profile}"
    target = args.output_root / run_id
    target.mkdir(parents=True, exist_ok=False)

    # ── OpenMP 环境 ───────────────────────
    environment = os.environ.copy()
    environment["OMP_PROC_BIND"] = args.proc_bind
    environment["OMP_PLACES"] = args.places

    # ── 预热 ──────────────────────────────
    print(f"  预热 {args.warmups} 轮...")
    for _ in range(args.warmups):
        run_program(args, "serial", 1, environment)
        for thread_count in threads:
            run_program(args, "openmp", thread_count, environment)

    # ── 计时运行 ──────────────────────────
    records: list[dict[str, object]] = []
    print(f"  串行 {args.serial_repeats} 次...")
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
            print(f"    OpenMP {thread_count:>3d} 线程  [{repeat}/{args.repeats}]", end="\r")
            values, elapsed = run_program(
                args, "openmp", thread_count, environment
            )
            records.append(make_record(args, run_id, "openmp", thread_count,
                                       repeat, values, elapsed))
    print()  # 清除进度行

    # ── 保存数据 ──────────────────────────
    raw = target / "raw.csv"
    if records:
        with raw.open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(records[0]))
            writer.writeheader()
            writer.writerows(records)

    # ── manifest ──────────────────────────
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
        "parameters": {key: str(value) if isinstance(value, Path) else value
                       for key, value in parameters.items()},
        "openmp_environment": {
            "OMP_PROC_BIND": args.proc_bind,
            "OMP_PLACES": args.places,
        },
        "files": {"raw": "raw.csv", "validation": "validation.json",
                  "summary": "summary.csv", "figures": "figures/"},
    }
    (target / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    # ── 后处理 ────────────────────────────
    script_dir = Path(__file__).resolve().parent
    python = sys.executable

    # 验证
    print("  验证...")
    subprocess.run([python, str(script_dir / "validate_results.py"),
                    str(raw)], check=True)

    # 汇总
    summary = target / "summary.csv"
    print("  汇总...")
    subprocess.run([python, str(script_dir / "summarize_results.py"),
                    str(raw), str(summary)], check=True)

    # 图表
    if not args.no_plot:
        print("  绘图...")
        subprocess.run([python, str(script_dir / "plot_results.py"),
                        str(summary), str(target / "figures")], check=True)

    # 更新索引
    update_experiment_index(args.output_root, target, {
        "run_id": run_id,
        "created_at": manifest["created_at"],
        "dataset": args.dataset,
        "profile": profile,
        "threads": threads,
        "max_neighbors": args.max_neighbors,
    })

    return target


# ── 正确性门 ──────────────────────────────
def run_correctness_gate(args: argparse.Namespace) -> bool:
    """运行正确性验证门。返回 True 表示通过。"""
    print("── 正确性门 ────────────────────────────")
    build_ok = True
    if args.build_command:
        result = subprocess.run(args.build_command, shell=True)
        build_ok = result.returncode == 0

    # 运行各个测试
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    build_dir = project_root / "build"

    # 平台适配：Linux 上可执行文件无 .exe 后缀
    def _find_exe(base_name):
        candidates = [base_name, base_name.replace('.exe', '').replace('.out', ''),
                      base_name + '.exe', base_name + '.out']
        for name in candidates:
            p = build_dir / name
            if p.exists() and p.is_file():
                return p
        return None

    tests = [
        ("test_loader", [str(args.data)]),
        ("test_pair_hash", []),
        ("test_cooccurrence", [str(args.data)]),
        ("test_recommender", [str(args.data)]),
        ("test_evaluator", []),
        ("test_integration", [str(args.data)]),
    ]

    all_passed = build_ok
    for exe_name, exe_args in tests:
        exe_path = _find_exe(exe_name)
        if exe_path is None:
            print(f"  ⚠ 跳过 {exe_name}（未找到可执行文件）")
            continue
        result = subprocess.run(
            [str(exe_path)] + exe_args, capture_output=True, text=True
        )
        display_name = exe_path.name
        if result.returncode == 0 and "PASS" in result.stdout:
            print(f"  ✓ {display_name}")
        else:
            print(f"  ✗ {display_name} 失败")
            if result.stderr:
                print(f"    {result.stderr[:200]}")
            all_passed = False

    if all_passed:
        print("  正确性门通过 ✓")
    else:
        print("  正确性门失败 ✗")

    # 自测验证器
    print("  验证器自测...")
    result = subprocess.run(
        [sys.executable, str(script_dir / "validate_results.py"), "--self-test"],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        print("  ✓ 验证器自测通过")
    else:
        print("  ✗ 验证器自测失败")
        all_passed = False

    return all_passed


# ── 生成汇总报告 ──────────────────────────
def generate_report(output_root: Path) -> Path:
    """从实验索引生成 Markdown 汇总报告。"""
    index_path = output_root / "index.json"
    if not index_path.exists():
        raise FileNotFoundError(f"实验索引未找到: {index_path}")

    index = json.loads(index_path.read_text(encoding="utf-8"))
    report_path = output_root / "REPORT.md"
    lines = [
        "# 基准测试汇总报告",
        "",
        f"**生成时间**: {datetime.now().astimezone().isoformat()}",
        f"**平台**: {platform.platform()}",
        f"**主机名**: {platform.node()}",
        f"**处理器**: {platform.processor()}",
        f"**逻辑处理器数**: {os.cpu_count()}",
        f"**实验总数**: {len(index['experiments'])}",
        "",
        "---",
        "",
        "## 实验列表",
        "",
    ]

    for exp in index["experiments"]:
        lines.append(f"### {exp['run_id']}")
        lines.append(f"- **数据集**: {exp['dataset']}")
        lines.append(f"- **配置**: {exp['profile']}")
        lines.append(f"- **线程**: {exp['threads']}")
        lines.append(f"- **目录**: `{exp['directory']}`")
        # 尝试读取 summary
        summary_path = output_root / exp["directory"] / "summary.csv"
        if summary_path.exists():
            lines.append(f"- **汇总**: [{exp['directory']}/summary.csv]({exp['directory']}/summary.csv)")
            # 读取最佳加速比
            try:
                with summary_path.open(encoding="utf-8-sig") as f:
                    reader = csv.DictReader(f)
                    best = 1.0
                    best_threads = 1
                    for row in reader:
                        speedup = float(row.get("speedup", 0))
                        threads = int(row.get("threads", 0))
                        if speedup > best:
                            best = speedup
                            best_threads = threads
                    lines.append(f"- **最佳加速比**: {best:.2f}× ({best_threads} 线程)")
                    lines.append(f"- **最佳效率**: {best/best_threads*100:.1f}%")
            except Exception:
                pass
        lines.append("")

    lines.append("---")
    lines.append(f"*报告由 run_platform_benchmark.py 自动生成*")

    report_path.write_text("\n".join(lines), encoding="utf-8")
    return report_path


# ── 主函数 ────────────────────────────────
def main() -> int:
    parser = argparse.ArgumentParser(
        description="OpenMP 购物篮推荐跨平台自动基准测试",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --auto-config --data data/medium
  %(prog)s --batch --data data/medium --max-neighbors 0,20,50,100
  %(prog)s --all --max-threads 48
  %(prog)s --correctness
  %(prog)s --report --output-root results/experiments
        """,
    )
    # ── 运行模式 ──────────────────────────
    parser.add_argument("--auto-config", action="store_true",
                        help="自动检测平台并使用推荐配置")
    parser.add_argument("--batch", action="store_true",
                        help="批量模式：运行多组配置")
    parser.add_argument("--correctness", action="store_true",
                        help="仅运行正确性门")
    parser.add_argument("--all", action="store_true",
                        help="一键完整实验（正确性 + small + medium + large）")
    parser.add_argument("--report", action="store_true",
                        help="从已有索引生成汇总报告")

    # ── 实验参数 ──────────────────────────
    parser.add_argument("--executable", type=Path,
                        default=Path("build/basket_recommender.exe"))
    parser.add_argument("--data", type=Path, default=Path("data/medium"))
    parser.add_argument("--dataset", default="medium")
    parser.add_argument("--threads", default="auto",
                        help="逗号分隔列表或 auto（自动生成）")
    parser.add_argument("--max-threads", type=int,
                        default=os.cpu_count() or 1)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--serial-repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--max-neighbors", type=int, default=50,
                        help="0 表示完整图；>0 表示 Top-N 截断")

    # ── 调度参数 ──────────────────────────
    parser.add_argument("--cooccur-schedule",
                        choices=("static", "dynamic", "guided"), default="dynamic")
    parser.add_argument("--cooccur-chunk", type=int, default=64)
    parser.add_argument("--recommend-schedule",
                        choices=("static", "dynamic", "guided"), default="dynamic")
    parser.add_argument("--recommend-chunk", type=int, default=16)

    # ── OpenMP 绑定 ───────────────────────
    parser.add_argument("--proc-bind", default="spread",
                        choices=["spread", "close", "master"])
    parser.add_argument("--places", default="cores",
                        choices=["cores", "threads", "sockets"])

    # ── 运行控制 ──────────────────────────
    parser.add_argument("--ordered", action="store_true",
                        help="不随机化线程顺序")
    parser.add_argument("--seed", type=int, default=20260718)
    parser.add_argument("--output-root", type=Path,
                        default=Path("results/platform-experiments"))
    parser.add_argument("--build-command",
                        default="make release",
                        help="编译命令，例如 'make release' 或 'bash scripts/build.sh release'")
    parser.add_argument("--no-plot", action="store_true")

    args = parser.parse_args()

    # ── 参数校验 ──────────────────────────
    if args.max_threads < 1 or args.repeats < 1 or args.serial_repeats < 1:
        parser.error("线程数和重复次数必须为正")
    if args.warmups < 0 or args.top_k < 1 or args.max_neighbors < 0:
        parser.error("无效的预热/top-k/max-neighbors 值")

    # ── 自动配置模式 ──────────────────────
    if args.auto_config:
        info = detect_platform()
        print("── 平台检测 ────────────────────────────")
        print(f"  逻辑处理器: {info['logical_processors']}")
        print(f"  物理核心:   {info.get('physical_cores', 'N/A')}")
        print(f"  内存:       {info.get('total_memory_gib', 'N/A')} GiB")
        print(f"  编译器:     {info.get('compiler', 'N/A')}")
        print(f"  推荐线程数: {info['recommended_threads']}")
        print(f"  推荐绑定:   {info['recommended_proc_bind']}")
        args.max_threads = info["logical_processors"]
        args.threads = ",".join(str(t) for t in info["recommended_threads"])
        args.proc_bind = info["recommended_proc_bind"]
        args.places = info["recommended_places"]

    # ── 仅生成报告 ────────────────────────
    if args.report:
        report_path = generate_report(args.output_root)
        print(f"报告已生成: {report_path}")
        return 0

    # ── 仅正确性门 ────────────────────────
    if args.correctness:
        if run_correctness_gate(args):
            return 0
        return 1

    # ── 一键完整实验 ──────────────────────
    if args.all:
        # 先编译所有测试
        print("── 编译 ────────────────────────────────")
        if shutil.which("make"):
            subprocess.run("make all", shell=True, check=False)
        elif Path("scripts/build.sh").exists():
            subprocess.run(["bash", "scripts/build.sh", "all"], check=False)

        # 正确性门
        args.data = Path("data/toy")
        args.dataset = "toy"
        if not run_correctness_gate(args):
            print("✗ 正确性门失败，终止实验")
            return 1

        configs = [
            ("small", 0, "Full"),
            ("small", 50, "Top-50"),
            ("medium", 0, "Full"),
            ("medium", 50, "Top-50"),
        ]
        # 只有在数据存在时才运行 large
        if (Path("data/large/orders.csv").exists() and
            Path("data/large/order_products__prior.csv").exists()):
            configs.append(("large", 50, "Top-50"))

        print(f"\n── 批量实验 ({len(configs)} 组) ────────────")
        for dataset, max_nb, label in configs:
            print(f"\n{'='*50}")
            print(f"  {dataset} / {label} (max_neighbors={max_nb})")
            print(f"{'='*50}")
            args.data = Path(f"data/{dataset}")
            args.dataset = dataset
            args.max_neighbors = max_nb
            try:
                result_dir = run_single_experiment(args)
                print(f"  ✓ 完成: {result_dir}")
            except Exception as exc:
                print(f"  ✗ 失败: {exc}", file=sys.stderr)
                continue

        # 生成最终报告
        report_path = generate_report(args.output_root)
        print(f"\n── 汇总报告 ────────────────────────────")
        print(f"  {report_path}")
        return 0

    # ── 批量模式 ──────────────────────────
    if args.batch:
        neighbor_list_str = os.environ.get("MAX_NEIGHBORS", "")
        if not neighbor_list_str and args.max_neighbors:
            # 解析 --max-neighbors 中的逗号列表
            try:
                neighbor_list = [int(x) for x in str(args.max_neighbors).split(",")]
            except ValueError:
                neighbor_list = [args.max_neighbors]
        else:
            neighbor_list = [int(x) for x in neighbor_list_str.split(",")] if neighbor_list_str else [args.max_neighbors]

        print(f"── 批量实验: max_neighbors={neighbor_list} ──")
        results = []
        for max_nb in neighbor_list:
            args.max_neighbors = max_nb
            label = "Full" if max_nb == 0 else f"Top-{max_nb}"
            print(f"\n  {label}...")
            try:
                result_dir = run_single_experiment(args)
                results.append((max_nb, result_dir))
                print(f"  ✓ {result_dir}")
            except Exception as exc:
                print(f"  ✗ {label} 失败: {exc}", file=sys.stderr)

        # 批量比较图
        if len(results) >= 2:
            script_dir = Path(__file__).resolve().parent
            summaries = [str(r[1] / "summary.csv") for r in results if (r[1] / "summary.csv").exists()]
            if len(summaries) >= 2:
                comparison_dir = args.output_root / f"{datetime.now().strftime('%Y%m%d')}-comparison"
                comparison_dir.mkdir(parents=True, exist_ok=True)
                subprocess.run([sys.executable, str(script_dir / "compare_profiles.py"),
                                str(comparison_dir)] + summaries, check=True)
                print(f"\n  对比图: {comparison_dir}")
        return 0

    # ── 单次实验 ──────────────────────────
    result_dir = run_single_experiment(args)
    print(f"\n── 实验完成 ────────────────────────────")
    print(f"EXPERIMENT_DIR={result_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
