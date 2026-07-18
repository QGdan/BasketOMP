# 性能实验记录

性能实验仅在正确性质量门通过后进行。所有正式结果使用 Release 构建，并记录硬件、编译器、编译参数、数据规模、线程数和重复次数。

## medium 正式实验（2026-07-16）

原始数据：`results/raw/runtime-medium-20260716-125503.csv`

汇总数据：`results/summary/runtime-medium-20260716-125503-summary.csv`

动态调度中位数：

| 版本 | 线程 | algorithm_ms | Speedup | Efficiency |
| --- | ---: | ---: | ---: | ---: |
| serial | 1 | 7,711 | 1.000 | 100.0% |
| OpenMP | 1 | 7,885 | 0.978 | 97.8% |
| OpenMP | 2 | 4,224 | 1.826 | 91.3% |
| OpenMP | 4 | 2,668 | 2.890 | 72.3% |
| OpenMP | 8 | 1,569 | 4.915 | 61.4% |

所有组的 Hit Rate@10 均为 0.8428125，Recall@10 均为 0.31472267698；共现与推荐校验和完全一致。

8 线程 static 单次控制实验的 `algorithm_ms=1,689`，dynamic 三次中位数为 `1,569`。当前数据上 dynamic 略优，但正式结论仍以多次重复数据为主。

### 已修复的性能异常

初次实验中，OpenMP 1 线程的归并约 12.96 秒。原因是全局哈希表从很小容量开始，按线程局部哈希槽顺序批量插入时形成长线性探测链。归并前按局部键数上界预留容量后，OpenMP 1 线程归并降至约 48 毫秒，且完整正确性回归保持通过。

## large 完整规模验证（2026-07-16）

原始数据：`results/raw/runtime-large-20260716-smoke.csv`

实验机：AMD Ryzen 7 8845H，8 核 16 线程，约 27.81 GiB 内存，Windows NT 10.0.26200.0，MinGW-W64 GCC 8.1.0。

| 版本 | 线程 | algorithm_ms | end_to_end_ms | Speedup | Efficiency |
| --- | ---: | ---: | ---: | ---: | ---: |
| serial | 1 | 1,217,953 | 1,234,896 | 1.000 | 100.0% |
| OpenMP | 8 | 199,149 | 217,511 | 6.116 | 76.4% |

完整 large 数据产生 40,749,010 条唯一商品对和 238,428,378 次商品对事件。串行与 OpenMP 的 Hit Rate@10 均为 0.843189110503，Recall@10 均为 0.306467486678；共现校验和与推荐校验和逐项一致。

串行瓶颈是推荐阶段，耗时 1,196,608 ms，占算法时间约 98.2%；8 线程推荐阶段降至 184,111 ms。基础全图方案能够在现有内存上完成，无需为了可运行性强制加入 Top-N 截断。

## 第二阶段优化实验（2026-07-17）

### 边归一化分母预计算

保持完整图与推荐 ID 校验和不变时，medium 串行 `algorithm_ms` 从 7711 降到 4949，推荐从 7054 降到 4297；OpenMP 8 线程 `algorithm_ms` 从 1569 降到 1431，推荐从 999 降到 818。

该优化把 `sqrt(popularity(a)*popularity(b))` 从每次用户评分移到构图阶段，每条邻接边只计算一次。为保持浮点运算顺序，邻接项存储完整 `double` 分母，而不是按商品拆成两个逆平方根。

### medium Top-N 扫描

| 配置 | 邻接项 | 8 线程推荐 | 8 线程算法 | Hit Rate | Recall |
| --- | ---: | ---: | ---: | ---: | ---: |
| Full | 5,225,290 | 818 ms | 1431 ms | 0.8428125 | 0.314722676980 |
| Top-20 | 512,879 | 20 ms | 710 ms | 0.8403125 | 0.305343678140 |
| Top-50 | 1,052,186 | 45 ms | 748 ms | 0.8412500 | 0.308845818812 |
| Top-100 | 1,661,206 | 87 ms | 826 ms | 0.8396875 | 0.310919533357 |

Top-20 在 medium 满足质量门，但 large Recall 下降约 0.013，因此最终改选 Top-50。

### Top-50 最终结果

medium 三次中位数：

| 版本 | 线程 | adjacency_ms | algorithm_ms | Speedup | Efficiency |
| --- | ---: | ---: | ---: | ---: | ---: |
| serial | 1 | 887 | 1732 | 1.000 | 100.0% |
| OpenMP | 1 | 841 | 1744 | 0.993 | 99.3% |
| OpenMP | 2 | 465 | 1076 | 1.610 | 80.5% |
| OpenMP | 4 | 259 | 704 | 2.460 | 61.5% |
| OpenMP | 8 | 143 | 522 | 3.318 | 41.5% |

以上为 2026-07-18 并行邻接构建与并行 Top-N 完成后的三次中位数。此前 1.310 倍的结果保留在旧实验目录中，用于说明瓶颈定位过程，不再作为当前实现的最终 medium 加速比。

large 单次完整验证：

| 版本 | 线程 | algorithm_ms | recommend_ms | Speedup |
| --- | ---: | ---: | ---: | ---: |
| serial | 1 | 26,072 | 6,936 | 1.000 |
| OpenMP | 8 | 16,495 | 1,222 | 1.581 |

Top-50 让推荐阶段大幅缩短，上一版的邻接构建因此成为串行瓶颈；当前版本已将该阶段并行化。报告仍应分开陈述：

- 相同 Top-50 算法的 OpenMP 加速比；
- Full 到 Top-50 的算法优化收益；
- 旧 Full 串行到新 Top-50 OpenMP 的组合收益。

正式结果：

- `results/experiments/20260718-011527530-medium-fast-normalization-top50/`
- `results/experiments/20260718-011641111-medium-fast-normalization-full/`
- `results/experiments/20260717-210005-medium-fast-normalization-top50/`
- `results/experiments/20260717-205106-large-fast-normalization-top50/`
- `results/experiments/20260717-medium-topn-comparison/`
