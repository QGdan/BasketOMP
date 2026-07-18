# OpenMP Instacart Basket Recommender

本项目使用 **C11 + OpenMP** 实现 Instacart 购物篮推荐任务，用于并行程序设计课程大作业。程序从用户历史订单中统计商品共现关系，为每个用户生成 Top-K 推荐，并比较独立串行版本与 OpenMP 版本的运行时间、加速比和并行效率。

## 1. 当前状态

基础版本已经完成并通过 G0–G10 审效测试，包含：

- 4 类 CSV 数据加载与结构验证；
- 64 位商品对编码和开放寻址哈希表；
- 串行商品共现与商品热度统计；
- OpenMP 订单级并行共现统计；
- CSR 商品共现图；
- 串行与 OpenMP 用户级 Top-K 推荐；
- Hit Rate@K、宏平均 Recall@K；
- toy 手算测试、small 回归测试和串并行逐项比较；
- medium 重复性能实验和 large 完整规模验证；
- 原始结果、汇总 CSV 和性能图表生成脚本。

第二阶段 O0–O6 算法优化与自动化开发也已完成：

- 共现边归一化分母预计算，保持旧推荐校验和不变；
- 可选 Top-N 邻居截断，最终推荐实验配置为 Top-50；
- 共现与推荐分别支持 static、dynamic、guided 及 chunk 参数；
- 自动生成实验 manifest、raw CSV、验证报告、汇总 CSV 和图表；
- medium/large 的 Full、Top-20/50/100 性能—效果实验；
- 自动正确性门和结构化结果验证器。

## 2. 总体架构

程序按照下面的数据流运行：

```text
products.csv + orders.csv + prior.csv + train.csv
                         │
                         ▼
                 csv_loader.c
                         │
                         ▼
 Dataset
 ├── BasketTable：prior 购物篮 CSR
 ├── UserHistory：用户历史频次 CSR
 └── GroundTruth：train 验证真值 CSR
                         │
             ┌───────────┴───────────┐
             ▼                       ▼
  串行共现统计                OpenMP 订单级共现统计
             └───────────┬───────────┘
                         ▼
 CooccurResult
 ├── PairHashMap：无序商品对及共现次数
 └── popularity：商品历史出现次数
                         │
                         ▼
              CooccurGraph（CSR 邻接图）
                         │
             ┌───────────┴───────────┐
             ▼                       ▼
      串行用户推荐             OpenMP 用户级推荐
             └───────────┬───────────┘
                         ▼
             RecommendationResult
                         │
                         ▼
              Hit Rate@K / Recall@K
```

### 2.1 两个主要并行点

1. **订单级商品共现统计**

   不同购物篮互不依赖。每个线程维护独立的商品对哈希表和商品热度数组，避免并发写全局哈希表。并行循环结束后再进行归并。

2. **用户级 Top-K 推荐**

   不同用户的候选生成和评分互不依赖。每个线程使用私有候选工作区，每个用户只写自己对应的结果区间，因此不需要锁。

### 2.2 基础评分函数

```text
score(u,p) = 1.0 × freq(u,p)
           + 0.8 × co_score(u,p)
           + 0.2 × log(1 + popularity(p))
```

其中共现项使用商品热度归一化，防止全局热门商品完全压过用户历史偏好。Top-K 按“分数降序、商品 ID 升序”排序，确保串行和并行结果稳定一致。

## 3. 项目目录和文件作用

```text
project/
├── README.md
├── Makefile
├── data/
├── include/
├── src/
├── tests/
├── scripts/
├── results/
├── docs/
└── build/
```

### 3.1 根目录

| 文件 | 作用 |
| --- | --- |
| `README.md` | 项目架构、构建运行、测试和组员交接说明 |
| `Makefile` | GNU Make 构建入口；适合安装了 `make` 的 Linux/MinGW 环境 |

### 3.2 `include/`：公共接口和数据结构

| 文件 | 作用 |
| --- | --- |
| `model.h` | 定义 `Dataset`、`BasketTable`、`UserHistory`、`GroundTruth`、`CooccurGraph` 等核心结构 |
| `csv_loader.h` | 声明数据加载、数据验证和内存释放接口 |
| `pair_hash.h` | 声明商品对编码、哈希表增查、预留容量、合并、遍历和校验和接口 |
| `cooccurrence.h` | 声明串行/OpenMP 共现统计、结果比较、图构建和图验证接口 |
| `recommender.h` | 声明串行/OpenMP 推荐、推荐结果比较和校验和接口 |
| `evaluator.h` | 定义 `Metrics`，声明 Hit Rate 和 Recall 评估接口 |
| `timer.h` | 预留的计时模块接口；当前主要计时直接使用 `omp_get_wtime()` |

### 3.3 `src/`：核心实现

| 文件 | 作用 |
| --- | --- |
| `main.c` | 命令行入口；串起加载、共现、图构建、推荐、评估、计时和摘要输出 |
| `csv_loader.c` | 读取 4 个 CSV；建立订单索引；构建购物篮、用户历史和 train 真值 CSR |
| `pair_hash.c` | 开放寻址 64 位商品对哈希表；包含线性探测、扩容、预留、合并和校验和 |
| `cooccurrence_serial.c` | 独立串行共现基线、结果比较、校验和及 CSR 共现图构建 |
| `cooccurrence_openmp.c` | 线程局部哈希表、订单动态调度、局部结果串行归并及分阶段计时 |
| `recommender_internal.h` | 推荐模块内部工作区和单用户推荐函数，不对其他模块公开 |
| `recommender_serial.c` | 候选评分、确定性 Top-K、独立串行推荐和推荐结果比较 |
| `recommender_openmp.c` | 按用户动态调度；每线程复用稠密分数数组和代际标记工作区 |
| `evaluator.c` | 计算 Hit Rate@K 和用户宏平均 Recall@K |
| `timer.c` | 预留计时实现文件；当前没有独立计时逻辑 |

### 3.4 `tests/`：测试程序

| 文件 | 作用 |
| --- | --- |
| `omp_smoke.c` | 验证编译器 OpenMP 支持及请求线程数是否生效 |
| `test_loader.c` | 验证 toy 逐项内容、small 统计量、CSR 不变量、重复加载释放和错误路径 |
| `test_pair_hash.c` | 验证编码、冲突、扩容、reserve、合并、遍历、溢出和随机预言机 |
| `test_cooccurrence.c` | 验证 toy 手算共现、Top-N 图，以及 1/2/4 线程三种调度与串行逐项一致 |
| `test_recommender.c` | 验证 toy 排名、空历史、无重复商品和串并行推荐一致性 |
| `test_evaluator.c` | 验证 toy 的 Hit Rate@10=`2/3`、Recall@10=`0.5` |
| `test_integration.c` | 验证完整图、超大 N、Top-N 以及 guided OpenMP 的端到端一致性 |

### 3.5 `scripts/`：构建、回归和实验脚本

| 文件 | 作用 |
| --- | --- |
| `build.ps1` | Windows 主构建入口，支持 Debug、Release、Smoke 和各单元测试目标 |
| `run_correctness.ps1` | 构建并执行全部正确性测试，最后比较串行/OpenMP 端到端校验和 |
| `run_benchmark.ps1` | 支持预热、Top-N、两阶段调度和重复运行，生成独立实验目录 |
| `run_platform_benchmark.py` | Linux/Windows 跨平台实验运行器，支持自动生成至 48 线程的测试点、线程绑定、随机运行顺序及自动验证/汇总/绘图 |
| `collect_environment.ps1` | 采集编译器、CPU、系统和实验参数并生成 `manifest.json` |
| `validate_results.py` | 校验 CSV schema、数值范围、阶段时间、图规模和校验和 |
| `summarize_results.py` | 输出中位数、最小/最大值、标准差、Speedup 和 Efficiency |
| `plot_results.py` | 在当前实验目录生成运行时间、加速比、效率和阶段占比图 |
| `compare_profiles.py` | 汇总 Full/Top-N 的性能与效果差异并生成权衡图 |
| `run_topn_sweep.ps1` | 自动运行一组 N、验证结果、汇总并生成 Top-N 对比 |
| `run_optimization_gate.ps1` | 执行 O0–O6 正确性、结果验证和可选 medium 实验门 |

### 3.6 `data/`：输入数据

| 目录 | 规模 | 用途 |
| --- | --- | --- |
| `toy/` | 7 个订单、6 条 prior | 人工手算和单元测试 |
| `small/` | 2,995 个订单、26,575 条 prior | 正确性、回归和快速联调 |
| `medium/` | 81,832 个订单、761,750 条 prior | 正式重复性能实验 |
| `large/` | 3,421,083 个订单、32,434,489 条 prior | 完整规模扩展性验证 |

每个数据目录包含：

| 文件 | 作用 |
| --- | --- |
| `orders.csv` | `order_id`、`user_id`、prior/train、订单顺序等信息 |
| `order_products__prior.csv` | 用户历史订单商品明细，用于共现和推荐模型 |
| `order_products__train.csv` | 下一购物篮真值，用于推荐效果评估 |
| `products.csv` | 商品 ID、名称、aisle 和 department 信息 |

`data/toy/EXPECTED.md` 给出了 toy 数据的手算购物篮、商品热度、共现次数、用户历史和 train 真值。

原始 CSV 不应在程序运行过程中被修改。

### 3.7 `results/`：实验结果

| 目录 | 作用 |
| --- | --- |
| `raw/` | 每次实验的原始记录，不应手工修改 |
| `summary/` | 中位数、加速比和并行效率汇总 |
| `figures/` | 报告可使用的 PNG 图表 |
| `baselines/` | 优化前正式结果登记和 manifest |
| `experiments/<run_id>/` | 第二阶段每次实验的 manifest、raw、validation、summary 和 figures |

正式 medium 数据是：

- `results/raw/runtime-medium-20260716-125503.csv`
- `results/summary/runtime-medium-20260716-125503-summary.csv`

带 `INVALID-pre-reserve-` 前缀的文件是修复病态归并前的无效性能实验，仅保留用于问题追踪，不应作为报告最终数据。

### 3.8 `docs/`：审效和性能记录

| 文件 | 作用 |
| --- | --- |
| `test-report.md` | G0–G10 各阶段测试内容、结果和通过条件 |
| `performance-notes.md` | medium/large 性能数据、瓶颈分析及归并优化记录 |
| `known-issues.md` | Sanitizer、峰值内存采集和 large 回归成本等已知限制 |
| `algorithm-optimization-plan.md` | 第二阶段算法优化需求、串行参考差异、优化优先级和实验准入标准 |
| `optimization-workflow-todos.md` | O0–O6 自动化开发工作流、审效门和可执行 TODO 看板 |
| `optimization-results.md` | 第二阶段正式性能、效果权衡、最终配置和报告结论 |
| `recommendation-quality-and-48thread-experiments.md` | 推荐质量综合评价、加速比下降原因和学校平台 48 线程正式实验方案 |

### 3.9 `build/`：本地构建产物

该目录包含主程序和测试程序的 `.exe`。这些文件可由构建脚本重新生成，不是算法源码。

主要产物：

- `basket_recommender.exe`：Release 主程序；
- `basket_recommender_debug.exe`：Debug 主程序；
- `test_*.exe`：各模块测试程序；
- `omp_smoke.exe`：OpenMP 环境测试程序。

## 4. 核心数据结构

### 4.1 CSR 连续存储

购物篮、用户历史、train 真值和共现图均使用类似 CSR 的连续数组：

```text
第 i 组数据范围 = [offsets[i], offsets[i+1])
```

这种设计比“每个订单/用户一个动态数组”减少了大量小对象、指针和内存碎片，也便于 OpenMP 线程只读共享。

### 4.2 商品对哈希表

无序商品对 `(a,b)` 编码为 64 位整数：

```c
key = ((uint64_t)min(a,b) << 32) | max(a,b);
```

哈希表采用开放寻址和线性探测，负载因子达到约 0.70 时扩容。OpenMP 共现阶段每个线程拥有独立哈希表，最后归并到全局表。

共现图邻接项保存邻居 ID、原始共现权重和预计算的归一化分母。推荐阶段不再重复开方，同时仍按优化前的浮点运算次序计算分数。

当 `--max-neighbors N` 大于 0 时，每个商品按“权重降序、商品 ID 升序”选择前 N 个邻居，原地压缩后再按商品 ID 排序。截断图允许非对称，因为每个商品独立选择自己的 Top-N。

### 4.3 推荐线程私有工作区

推荐阶段没有为每个用户反复创建哈希表，而是每个线程复用：

- `scores[product_id]`：候选商品分数；
- `marks[product_id]`：代际标记；
- `candidates[]`：当前用户实际出现的候选商品。

代际标记避免每处理一个用户就清空全部约 5 万个商品位置。

## 5. 构建环境

已验证环境：

- Windows NT 10.0.26200.0；
- AMD Ryzen 7 8845H，8 核 16 线程；
- 约 27.81 GiB 内存；
- MinGW-W64 GCC 8.1.0；
- Python 及 Matplotlib，用于汇总和绘图。

### 5.1 Windows PowerShell 构建

```powershell
cd project

# Debug 主程序
.\scripts\build.ps1 Debug

# Release 主程序，正式性能实验必须使用此版本
.\scripts\build.ps1 Release

# OpenMP 环境验证程序
.\scripts\build.ps1 Smoke
.\build\omp_smoke.exe 4
```

### 5.2 构建单个测试

```powershell
.\scripts\build.ps1 TestLoader
.\scripts\build.ps1 TestHash
.\scripts\build.ps1 TestCooccur
.\scripts\build.ps1 TestRecommender
.\scripts\build.ps1 TestEvaluator
```

### 5.3 GNU Make

安装了 GNU Make 的环境也可以运行：

```powershell
make debug
make release
make smoke

# MinGW 若命令名为 mingw32-make：
mingw32-make debug
mingw32-make release
mingw32-make smoke
```

## 6. 运行方式

### 6.1 串行版本

```powershell
.\build\basket_recommender.exe `
  --data data\medium `
  --mode serial `
  --top-k 10
```

### 6.2 OpenMP 版本

```powershell
.\build\basket_recommender.exe `
  --data data\medium `
  --mode openmp `
  --threads 8 `
  --top-k 10 `
  --max-neighbors 50 `
  --cooccur-schedule dynamic `
  --cooccur-chunk 64 `
  --recommend-schedule dynamic `
  --recommend-chunk 16
```

### 6.3 命令行参数

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--data DIR` | 含 4 个 CSV 的数据目录 | `data/toy` |
| `--mode serial\|openmp` | 串行或 OpenMP 模式 | `serial` |
| `--threads N` | OpenMP 线程数 | OpenMP 最大线程数 |
| `--top-k K` | 每个用户输出的推荐数量 | `10` |
| `--max-neighbors N` | 每个商品最多保留 N 个邻居；0 为完整图 | `0` |
| `--cooccur-schedule static\|dynamic\|guided` | 共现阶段调度 | `dynamic` |
| `--cooccur-chunk N` | 共现阶段 chunk | `64` |
| `--recommend-schedule static\|dynamic\|guided` | 推荐阶段调度 | `dynamic` |
| `--recommend-chunk N` | 推荐阶段 chunk | `16` |
| `--schedule ...` | 兼容旧命令，仅设置共现调度 | `dynamic` |
| `--samples N` | 额外输出前 N 个非空用户推荐样例 | 不输出 |
| `--output FILE` | 将摘要写入指定文件 | 标准输出 |

示例：

```powershell
.\build\basket_recommender_debug.exe `
  --data data\toy `
  --mode serial `
  --samples 3 `
  --output build\toy-summary.txt
```

## 7. 程序输出说明

主程序以 `key=value` 形式输出，便于脚本解析。主要字段包括：

| 字段 | 含义 |
| --- | --- |
| `unique_pairs` | 唯一无序商品对数量 |
| `pair_events` | 所有购物篮枚举出的商品对事件总数 |
| `load_ms` | CSV 加载和内存数据结构构建时间 |
| `cooccur_compute_ms` | 串行/并行共现局部计算时间 |
| `merge_ms` | OpenMP 线程局部表归并时间；串行为 0 |
| `normalization_ms` | 构图时预计算边归一化分母的时间 |
| `adjacency_ms` | CSR 共现图构建时间 |
| `truncate_ms` | Top-N 选择与原地压缩时间，是 adjacency 的子集 |
| `recommend_ms` | 用户 Top-K 推荐时间 |
| `graph_edge_entries` | 截断后的有向邻接项数量 |
| `candidate_shortage_users` | 有历史但候选数少于 K 的用户数 |
| `algorithm_ms` | 共现、归并、图和推荐阶段总时间，不含 CSV I/O |
| `end_to_end_ms` | 从数据加载到评估完成的总时间 |
| `hit_rate_at_10` | 至少命中一个 train 商品的用户比例 |
| `recall_at_10` | 用户 Recall@10 的宏平均 |
| `cooccur_checksum` | 共现结果稳定校验和 |
| `recommendation_checksum` | 推荐商品 ID 稳定校验和 |

性能比较时，串行与 OpenMP 的两类校验和必须一致。

## 8. 正确性测试

运行完整回归：

```powershell
.\scripts\run_correctness.ps1
```

该脚本会：

1. 构建全部测试程序、Debug 主程序和 Release 主程序；
2. 验证 toy 和 small 数据加载；
3. 验证商品对哈希表；
4. 比较串行/OpenMP 共现结果；
5. 比较串行/OpenMP 推荐结果；
6. 验证 Hit Rate 和 Recall；
7. 比较端到端推荐校验和。

看到以下输出表示完整回归通过：

```text
PASS: complete correctness regression
```

修改共现、推荐、哈希表、数据结构或 OpenMP 代码后，必须重新运行该脚本。

## 9. 性能实验

### 9.1 批量运行

```powershell
$raw = .\scripts\run_benchmark.ps1 `
  -Dataset medium `
  -Repeats 5 -SerialRepeats 3 -Warmups 1 `
  -ThreadCounts 1,2,4,8 `
  -MaxNeighbors 50 -RandomizeThreadOrder
```

脚本会先构建 Release 版本，再对独立串行版和不同线程数 OpenMP 版重复运行，并将原始结果写入 `results/raw/`。

### 9.2 汇总中位数

```powershell
python .\scripts\summarize_results.py $raw
```

计算公式：

```text
Speedup(p)    = T_serial / T_parallel(p)
Efficiency(p) = Speedup(p) / p
```

### 9.3 生成图表

```powershell
python .\scripts\plot_results.py `
  results\summary\runtime-medium-20260716-125503-summary.csv
```

输出到 `results/figures/`：

- `medium-runtime.png`；
- `medium-speedup.png`；
- `medium-efficiency.png`；
- `medium-stage-speedup.png`；
- `medium-stages.png`。

### 9.4 学校平台 48 线程实验

跨平台运行器不依赖 PowerShell，适合 Linux 大数据实验平台：

```bash
python3 scripts/run_platform_benchmark.py \
  --executable build/basket_recommender \
  --data data/medium --dataset medium \
  --threads auto --max-threads 48 \
  --repeats 5 --serial-repeats 3 --warmups 1 \
  --max-neighbors 50 --proc-bind spread --places cores
```

自动测试点为 `1,2,4,8,12,16,24,32,48`。运行器会生成 manifest、原始 CSV、校验报告、汇总 CSV 和五类图表。正式报告建议分别运行 Full（`--max-neighbors 0`）与 Top-50：前者用于分析并行扩展性，后者用于展示实际性能—质量权衡。完整说明见 `docs/recommendation-quality-and-48thread-experiments.md`。

## 10. 当前性能结果

以下 10.1–10.2 是优化前完整图基线，仍保留用于纯 OpenMP 加速分析。

### 10.1 medium 三次中位数

| 版本 | 线程数 | algorithm_ms | Speedup | Efficiency |
| --- | ---: | ---: | ---: | ---: |
| serial | 1 | 7,711 | 1.000 | 100.0% |
| OpenMP | 1 | 7,885 | 0.978 | 97.8% |
| OpenMP | 2 | 4,224 | 1.826 | 91.3% |
| OpenMP | 4 | 2,668 | 2.890 | 72.3% |
| OpenMP | 8 | 1,569 | 4.915 | 61.4% |

### 10.2 large 完整规模

| 版本 | 线程数 | algorithm_ms | Speedup | Efficiency |
| --- | ---: | ---: | ---: | ---: |
| serial | 1 | 1,217,953 | 1.000 | 100.0% |
| OpenMP | 8 | 199,149 | 6.116 | 76.4% |

large 共产生 40,749,010 条唯一共现边和 238,428,378 次商品对事件。串行与 OpenMP 的指标和校验和完全一致。

### 10.3 第二阶段最终 Top-50

2026-07-18 并行邻接构建与并行 Top-N 后的 medium 三次中位数：

| 版本 | 线程 | adjacency_ms | algorithm_ms | 相同配置 Speedup | Hit Rate | Recall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 1 | 887 | 1732 | 1.000 | 0.841250 | 0.308846 |
| OpenMP | 1 | 841 | 1744 | 0.993 | 0.841250 | 0.308846 |
| OpenMP | 2 | 465 | 1076 | 1.610 | 0.841250 | 0.308846 |
| OpenMP | 4 | 259 | 704 | 2.460 | 0.841250 | 0.308846 |
| OpenMP | 8 | 143 | 522 | 3.318 | 0.841250 | 0.308846 |

large 以下结果来自并行邻接构建实施前，需在学校平台重新运行：

| 版本 | 线程 | algorithm_ms | 相同配置 Speedup | Hit Rate | Recall |
| --- | ---: | ---: | ---: | ---: | ---: |
| serial | 1 | 26,072 | 1.000 | 0.836978 | 0.296935 |
| OpenMP | 8 | 16,495 | 1.581 | 0.836978 | 0.296935 |

Top-50 使推荐阶段不再是绝对瓶颈，因此相同算法的并行加速比下降；但相对优化前完整图串行，medium 和 large 的“算法优化 + OpenMP”组合收益分别约为 10.56 倍和 73.8 倍。详细解释见 `docs/optimization-results.md`。

## 11. 内存所有权和开发约束

- `dataset_load()` 成功后必须调用 `dataset_free()`；
- `build_cooccur_*()` 成功后必须调用 `cooccur_result_free()`；
- `cooccur_graph_build()` 成功后必须调用 `cooccur_graph_free()`；
- `recommend_*()` 成功后必须调用 `recommendation_result_free()`；
- OpenMP 并行区内禁止直接写全局商品对哈希表；
- 推荐线程不得共享候选工作区；
- 并行区内不要输出日志或写文件；
- 正确性测试使用 Debug 构建，正式计时使用 Release 构建；
- 性能计时区域内不得进行串并行结果比较；
- 修改排序规则后必须确认串行和并行 Top-K 仍完全一致；
- 不要手工修改 `results/raw/` 中的正式实验数据。

## 12. 组员接手建议

建议按以下顺序阅读：

1. 本 README 的总体架构和运行方式；
2. `include/model.h`，了解所有核心数据结构；
3. `src/main.c`，了解端到端执行顺序；
4. `src/cooccurrence_serial.c`，理解串行基线；
5. `src/cooccurrence_openmp.c`，理解线程局部统计和归并；
6. `src/recommender_serial.c` 和 `src/recommender_openmp.c`；
7. `tests/` 中对应测试，确认模块输入输出；
8. `docs/optimization-results.md`、`docs/test-report.md` 和 `docs/performance-notes.md`。

组员收到项目后建议先执行：

```powershell
cd project
.\scripts\run_correctness.ps1
```

如果完整回归通过，再运行 medium OpenMP 示例。不要直接从 large 完整图开始调试；需要 large 时优先使用经过验证的 `--max-neighbors 50`。

## 13. 已知限制与可选扩展

当前基础版尚未实现：

- 时间衰减权重；
- aisle/department 类别亲和度；
- 负采样和 AUC；
- 分桶并行归并；
- MPI 或 pthread 版本。

当前机器的 MinGW 缺少 ASan/UBSan 运行库，因此主要依靠编译警告、边界检查、结构不变量、重复运行和串并行逐项比较。完整说明见 `docs/known-issues.md`。

若继续扩展，必须保留现有串行基线和测试脚本，并在修改后重新通过完整正确性回归。
