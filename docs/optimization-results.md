# 第二阶段算法优化结果

## 1. 最终结论

第二阶段完成了两项主要优化：

1. 构图时预计算每条共现边的归一化分母，消除推荐阶段重复 `sqrt()`；
2. 增加可选 Top-N 邻居截断，并通过 medium/large 的性能—效果实验选择 Top-50 作为推荐的优化配置。

程序命令行默认仍使用完整图（`--max-neighbors 0`），以保持旧命令和旧指标兼容。需要最终优化配置时显式传入：

```powershell
--max-neighbors 50
```

## 2. 正确性与审效结果

- `scripts/run_optimization_gate.ps1 -Gate O4` 完整通过；
- toy、small 的加载、哈希、共现、推荐、评估和集成测试通过；
- full 图优化前后 toy/small 推荐 ID 校验和不变；
- Top-N 下串行与 OpenMP 的商品 ID、分数和校验和一致；
- static、dynamic、guided 三种调度结果一致；
- 结果验证器可以拒绝缺字段、指标越界、图边数错误和重复运行编号；
- small 的完整实验流水线成功生成 manifest、raw、validation、summary 和 figures。

## 3. 归一化分母预计算收益

medium 完整图三次中位数：

| 版本 | 优化前 algorithm | 优化后 algorithm | 优化前推荐 | 优化后推荐 | 说明 |
| --- | ---: | ---: | ---: | ---: | --- |
| 串行 | 7711 ms | 4949 ms | 7054 ms | 4297 ms | 算法时间降低 35.8% |
| OpenMP 8 线程 | 1569 ms | 1431 ms | 999 ms | 818 ms | 算法时间降低 8.8% |

串行收益更明显，是因为原实现中重复开方完全位于推荐热点循环。OpenMP 版本原本已把推荐分摊到多个线程，因此绝对收益仍明显，但相对比例较低。

## 4. medium Top-N 权衡

8 线程三次中位数：

| 配置 | 邻接项 | recommend_ms | algorithm_ms | Hit Rate | Recall |
| --- | ---: | ---: | ---: | ---: | ---: |
| Full | 5,225,290 | 818 | 1431 | 0.842813 | 0.314723 |
| Top-20 | 512,879 | 20 | 710 | 0.840313 | 0.305344 |
| Top-50 | 1,052,186 | 45 | 748 | 0.841250 | 0.308846 |
| Top-100 | 1,661,206 | 87 | 826 | 0.839688 | 0.310920 |

Top-20 在 medium 上满足 0.01 绝对质量损失门限，但 large Recall 损失超过门限，因此最终采用更稳妥的 Top-50。

## 5. 最终 Top-50 medium 扩展性

2026-07-18 增加两遍式并行邻接构建和按商品并行 Top-N 后，三次中位数为：

| 版本 | 线程 | adjacency_ms | recommend_ms | algorithm_ms | 相同配置 Speedup | Efficiency |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 串行 | 1 | 887 | 462 | 1732 | 1.000 | 100.0% |
| OpenMP | 1 | 841 | 451 | 1744 | 0.993 | 99.3% |
| OpenMP | 2 | 465 | 242 | 1076 | 1.610 | 80.5% |
| OpenMP | 4 | 259 | 148 | 704 | 2.460 | 61.5% |
| OpenMP | 8 | 143 | 88 | 522 | 3.318 | 41.5% |

邻接阶段 8 线程加速约 6.20 倍，推荐阶段约 5.25 倍，Top-50 同算法总加速比由上一版的 1.310 提升到 3.318。串行基线仍调用独立的串行图构建函数，没有被 OpenMP 路径替代。

本轮本地机器存在明显的跨批次绝对时间波动，因此正式报告应使用同一实验目录内的串行中位数计算加速比，不应跨日期拼接单次时间。

## 6. large 最终验证

| 配置 | 版本 | algorithm_ms | recommend_ms | Hit Rate | Recall |
| --- | --- | ---: | ---: | ---: | ---: |
| 旧 Full | 串行 | 1,217,953 | 1,196,608 | 0.843189 | 0.306467 |
| 旧 Full | OpenMP 8 | 199,149 | 184,111 | 0.843189 | 0.306467 |
| 新 Top-50 | 串行 | 26,072 | 6,936 | 0.836978 | 0.296935 |
| 新 Top-50 | OpenMP 8 | 16,495 | 1,222 | 0.836978 | 0.296935 |

Top-50 large 最终图为 2,449,985 个邻接项；旧完整图为 81,498,020 个邻接项，最终图规模降低约 97.0%。

- Hit Rate 绝对下降约 0.00621；
- Recall 绝对下降约 0.00953；
- 相同 Top-50 配置的 8 线程加速比为 1.581；
- 相对旧 Full OpenMP 8 线程，算法时间约快 12.1 倍；
- 相对旧 Full 串行，最终组合收益约为 73.8 倍。

## 7. 调度实验

Top-20 medium 8 线程控制实验：

| 调度 | algorithm_ms |
| --- | ---: |
| dynamic | 710 |
| static | 710 |
| guided | 700 |

差异约 1.4%，不足以支持更改默认调度。最终继续采用：

```text
cooccurrence: dynamic, chunk=64
recommendation: dynamic, chunk=16
```

## 8. 热门补全决策

程序新增候选统计后发现：

- medium 完整图有效用户中，候选不足 K 的用户数为 0；
- large Top-20 和 Top-50 的候选不足用户数也为 0；
- small 仅有 1 个有效用户候选不足，另一个空用户是 ID 0 占位。

因此热门商品补全没有现实收益，本阶段不实现，避免增加无效复杂度。

## 9. 自动化命令

完整正确性与结构化结果验证门：

```powershell
.\scripts\run_optimization_gate.ps1 -Gate O4
```

Top-N 自动扫描：

```powershell
.\scripts\run_topn_sweep.ps1 `
  -Dataset medium `
  -NeighborCounts 0,20,50,100 `
  -Threads 8 `
  -Repeats 3 `
  -Warmups 1
```

最终 Top-50 medium 实验：

```powershell
$raw = .\scripts\run_benchmark.ps1 `
  -Dataset medium `
  -Repeats 3 `
  -Warmups 1 `
  -ThreadCounts 1,2,4,8 `
  -MaxNeighbors 50

python .\scripts\validate_results.py $raw
python .\scripts\summarize_results.py $raw
```

## 10. 正式结果路径

- 并行邻接 Top-50 medium：`results/experiments/20260718-011527530-medium-fast-normalization-top50/`
- 并行邻接 Full medium：`results/experiments/20260718-011641111-medium-fast-normalization-full/`
- 优化后 Full medium：`results/experiments/20260717-204200-medium-fast-normalization-full/`
- 最终 Top-50 medium：`results/experiments/20260717-210005-medium-fast-normalization-top50/`
- Top-N 比较：`results/experiments/20260717-medium-topn-comparison/`
- large Top-20 质量复核：`results/experiments/20260717-204922-large-fast-normalization-top20/`
- 最终 Top-50 large：`results/experiments/20260717-205106-large-fast-normalization-top50/`

## 11. 分桶并行归并（O8）

2026-07-18 根据学校平台 large 1–48 线程结果实现分桶并行归并。每个线程在共现热点循环中写私有分桶哈希表；归并阶段每个 OpenMP 任务独占目标表的一个连续桶区间，不使用锁、`critical` 或原子更新。新增 `--merge-buckets N`，0 表示自动选择，48 线程默认 256 桶。

完整 toy/small 回归通过，自动、7、64 桶在 1/2/4 线程和三种调度下均与串行逐项一致。当前开发机 medium Top-50、8 线程三次诊断中位数如下：

| 桶数 | cooccur_compute_ms | merge_ms | algorithm_ms |
| ---: | ---: | ---: | ---: |
| 1 | 67 | 189 | 397 |
| 64 | 79 | 27 | 242 |
| 256 | 82 | 25 | 243 |

64 桶归并约比 1 桶快 7.0 倍，总算法时间改善约 39.0%；256 桶归并略快，但总算法时间与64桶基本相同。审效还修复了“低位选桶、低位选槽”导致的探测聚集，最终采用高位选桶、低位选槽。该数据是本地诊断，不替代学校平台 large 正式结果。下一轮应固定 24/32/48 线程比较 128/256/384 桶，并以 `algorithm_ms` 中位数而非单独 `merge_ms` 选择配置。详见 `docs/bucketed-parallel-merge.md`。
