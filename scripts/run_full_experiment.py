#!/usr/bin/env python3
"""
一键完整实验脚本 —— 从编译到最终报告，全部自动化。

用法:
  python scripts/run_full_experiment.py                    # 使用默认配置
  python scripts/run_full_experiment.py --max-threads 48   # 48 线程平台
  python scripts/run_full_experiment.py --skip-large       # 跳过 large 数据集
  python scripts/run_full_experiment.py --quick            # 快速模式（少重复）

输出结构:
  results/experiments/
  ├── index.json                          # 实验索引
  ├── REPORT.md                           # 汇总报告
  ├── 20260718-HHMMSS-xxx-small-Full/     # 各实验目录
  │   ├── raw.csv       (原始记录)
  │   ├── summary.csv   (汇总统计)
  │   ├── validation.json (验证报告)
  │   ├── manifest.json  (环境信息)
  │   └── figures/       (性能图表)
  └── comparison/                         # 对比图表
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


def detect_platform_info() -> dict:
    """打印平台信息。"""
    info = {
        "platform": platform.platform(),
        "hostname": platform.node(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count() or 1,
    }
    try:
        import psutil
        info["physical_cores"] = psutil.cpu_count(logical=False)
        info["memory_gib"] = round(psutil.virtual_memory().total / (1024**3), 1)
    except ImportError:
        info["physical_cores"] = None
        info["memory_gib"] = None
    return info


def run_command(cmd: str | list, description: str = "", check: bool = True) -> subprocess.CompletedProcess:
    """运行命令并显示进度。"""
    if isinstance(cmd, list):
        display = " ".join(str(c) for c in cmd)
    else:
        display = cmd
    print(f"  ▶ {description or display}")
    start = time.perf_counter()
    result = subprocess.run(
        cmd, shell=isinstance(cmd, str), check=False, text=True,
        capture_output=True,
    )
    elapsed = time.perf_counter() - start
    if check and result.returncode != 0:
        print(f"  ✗ 失败 ({elapsed:.1f}s)")
        if result.stderr:
            print(f"    {result.stderr[:500]}")
        raise subprocess.CalledProcessError(result.returncode, display,
                                            result.stdout, result.stderr)
    print(f"  ✓ 完成 ({elapsed:.1f}s)")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="OpenMP 购物篮推荐一键完整实验",
    )
    parser.add_argument("--max-threads", type=int,
                        default=os.cpu_count() or 8,
                        help="最大线程数（默认: 自动检测）")
    parser.add_argument("--quick", action="store_true",
                        help="快速模式: repeats=3, 跳过 Full 图 large 实验")
    parser.add_argument("--skip-large", action="store_true",
                        help="跳过 large 数据集实验")
    parser.add_argument("--skip-build", action="store_true",
                        help="跳过编译步骤（假设已编译）")
    parser.add_argument("--output-root", default="results/experiments",
                        help="实验输出根目录")
    parser.add_argument("--proc-bind", default="spread",
                        choices=["spread", "close", "master"])
    parser.add_argument("--places", default="cores",
                        choices=["cores", "threads", "sockets"])
    parser.add_argument("--build-only", action="store_true",
                        help="仅编译，不运行实验")
    args = parser.parse_args()

    # ── 平台信息 ──────────────────────────
    info = detect_platform_info()
    print("=" * 60)
    print("  OpenMP 购物篮推荐 —— 一键完整实验")
    print("=" * 60)
    print(f"  平台:     {info['platform']}")
    print(f"  主机名:   {info['hostname']}")
    print(f"  处理器:   {info['processor']}")
    print(f"  逻辑CPU:  {info['logical_cpus']}")
    if info.get("physical_cores"):
        print(f"  物理核心: {info['physical_cores']}")
    if info.get("memory_gib"):
        print(f"  内存:     {info['memory_gib']} GiB")
    print(f"  最大线程: {args.max_threads}")
    print(f"  模式:     {'快速' if args.quick else '完整'}")
    print()

    repeats = 3 if args.quick else 5
    serial_repeats = 2 if args.quick else 3

    # ── 编译 ──────────────────────────────
    if not args.skip_build:
        print("── 第 1 步: 编译 ────────────────────────")

        # 尝试检测构建工具
        build_cmd = None
        if shutil.which("make"):
            build_cmd = "make all"
        elif Path("scripts/build.sh").exists():
            build_cmd = "bash scripts/build.sh all"
        elif Path("scripts/build.ps1").exists():
            build_cmd = "powershell -File scripts/build.ps1"

        if build_cmd:
            run_command(build_cmd, "编译所有目标")
        else:
            print("  ⚠ 未找到构建工具，请手动编译")
            if args.build_only:
                return 0

    if args.build_only:
        print("\n构建完成。")
        return 0

    # ── 运行器路径 ────────────────────────
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    runner = script_dir / "run_platform_benchmark.py"

    if not runner.exists():
        print(f"错误: 未找到 {runner}", file=sys.stderr)
        return 1

    python = sys.executable

    # ── 第 2 步: 正确性门 ────────────────
    print("\n── 第 2 步: 正确性验证 ────────────────")
    cmd = [
        python, str(runner),
        "--correctness",
        "--data", str(project_root / "data/toy"),
        "--dataset", "toy",
        "--build-command", "true",  # 跳过重复编译
    ]
    try:
        run_command(cmd, "正确性门")
    except subprocess.CalledProcessError:
        print("\n✗ 正确性验证失败！请先修复问题再运行实验。")
        return 1

    # ── 第 3 步: 性能实验 ────────────────
    print("\n── 第 3 步: 性能基准实验 ──────────────")

    experiments = [
        # (数据集, max_neighbors, 标签, 是否可选)
        ("small", 0, "small-full", False),
        ("small", 50, "small-top50", False),
        ("medium", 0, "medium-full", False),
        ("medium", 50, "medium-top50", False),
        ("medium", 20, "medium-top20", True),
        ("medium", 100, "medium-top100", True),
        ("large", 50, "large-top50", args.skip_large),
    ]

    if args.quick:
        # quick 模式: 只跑 small+medium Top-50，跳过 Full 图和 large
        experiments = [
            ("small",   50, "small-top50",   False),
            ("medium",  50, "medium-top50",  False),
        ]

    output_root = project_root / args.output_root
    output_root.mkdir(parents=True, exist_ok=True)

    failed = []
    for i, (dataset, max_nb, label, skip) in enumerate(experiments):
        if skip:
            continue
        data_dir = project_root / f"data/{dataset}"
        if not (data_dir / "orders.csv").exists():
            print(f"  ⚠ 跳过 {label}: 数据目录 {data_dir} 不存在")
            continue

        print(f"\n  [{i+1}/{len(experiments)}] {label}")
        profile = "full" if max_nb == 0 else f"top{max_nb}"
        cmd = [
            python, str(runner),
            "--data", str(data_dir),
            "--dataset", dataset,
            "--threads", "auto",
            "--max-threads", str(args.max_threads),
            "--max-neighbors", str(max_nb),
            "--repeats", str(repeats),
            "--serial-repeats", str(serial_repeats),
            "--proc-bind", args.proc_bind,
            "--places", args.places,
            "--output-root", str(output_root),
            "--build-command", "true",  # 跳过重复编译
        ]
        try:
            run_command(cmd, f"{dataset} / {profile}")
        except subprocess.CalledProcessError:
            print(f"  ✗ {label} 失败，继续下一组...")
            failed.append(label)

    # ── 第 4 步: 生成对比图 ───────────────
    print("\n── 第 4 步: 生成对比图表 ──────────────")
    try:
        cmd = [
            python, str(runner),
            "--report",
            "--output-root", str(output_root),
        ]
        run_command(cmd, "汇总报告")
    except subprocess.CalledProcessError:
        print("  ⚠ 报告生成失败")

    # ── 最终摘要 ──────────────────────────
    print("\n" + "=" * 60)
    print("  实验完成！")
    print("=" * 60)
    print(f"  输出目录: {output_root}")
    print(f"  汇总报告: {output_root / 'REPORT.md'}")
    print(f"  实验索引: {output_root / 'index.json'}")

    report_path = output_root / "REPORT.md"
    if report_path.exists():
        print(f"\n  报告内容预览:")
        print(f"  {'─' * 50}")
        content = report_path.read_text(encoding="utf-8")
        # 只打印前 30 行
        lines = content.splitlines()[:30]
        for line in lines:
            print(f"  {line}")
        if len(content.splitlines()) > 30:
            print(f"  ... (共 {len(content.splitlines())} 行)")

    if failed:
        print(f"\n  失败的实验: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
