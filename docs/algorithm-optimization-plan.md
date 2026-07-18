# 购物篮推荐算法优化需求与实施规划

## 1. 文档定位

本文档是在基础 C11 + OpenMP 版本已经完成并通过 G0–G10 审效测试之后，为第二阶段算法优化制定的需求基线。优化对象是 `project/` 下的实现；`basket_recommender/` 是串行算法同学提供的 C++17 参考版本，用于核对算法思想和可扩展方向，不直接作为 OpenMP 性能基线。

本阶段追求的是：代码简单、结果可解释、优化可单独开关、实验可复现。任何优化都必须先证明正确，再证明有效，不能只凭理论判断写入最终版本。

## 2. 当前项目结论

### 2.1 当前可用基线

- `project/` 已实现独立串行版和 OpenMP 版；
- 数据加载、共现统计、CSR 图、用户推荐和评估均有测试；
- 2026-07-17 重新执行 `scripts/run_correctness.ps1`，全部测试通过；
- medium 正式结果：串行 `algorithm_ms=7711`，OpenMP 8 线程 `1569 ms`，加速比 `4.915`；
- large 完整结果：串行 `1217953 ms`，OpenMP 8 线程 `199149 ms`，加速比 `6.116`；
- 串行与 OpenMP 的共现和推荐校验和一致。

### 2.2 性能瓶颈证据

| 数据规模与版本 | 共现计算 | 归并 | 邻接图 | 推荐 | 推荐占算法时间 |
| --- | ---: | ---: | ---: | ---: | ---: |
| medium 串行 | 340 ms | 0 ms | 340 ms | 7054 ms | 91.5% |
| medium OpenMP 8 线程 | 68 ms | 147 ms | 371 ms | 999 ms | 63.7% |
| large 串行 | 14480 ms | 0 ms | 6865 ms | 1196608 ms | 98.2% |
| large OpenMP 8 线程 | 3521 ms | 4407 ms | 7110 ms | 184111 ms | 92.4% |

结论：第二阶段必须优先优化推荐阶段。此时直接实现复杂的并行哈希归并，收益上限明显低于优化推荐内层循环和邻居规模。

## 3. 串行同学版本与当前版本的差异

| 项目 | `basket_recommender/` 串行 C++ | `project/` C/OpenMP | 本阶段决策 |
| --- | --- | --- | --- |
| 商品对键 | 字符串 `"a,b"` | 64 位整数编码 | 保留 64 位编码，效率更高 |
| 用户历史 | 去重商品列表 | 商品及真实购买频次 | 保留频次，符合评分定义 |
| 历史商品是否可推荐 | 排除已购商品 | 允许复购 | 保留复购；Instacart 复购是重要行为 |
| 共现评分 | 原始共现次数 | 热度归一化共现 | 保留归一化，降低热门偏置 |
| 邻居数量 | Top-N 截断 | 全量邻居 | 将 Top-N 作为可选优化引入 |
| 候选不足 | 热门商品补全 | 空历史返回空，普通用户通常候选充足 | 作为低优先级可选扩展 |
| 排名并列 | 未固定商品 ID 次序 | 分数降序、ID 升序 | 保留确定性次级排序 |
| 文件名 | prior/train 文件减少一个下划线 | 使用官方双下划线文件名 | 保持 `project/` 当前格式 |

因此，本阶段只吸收串行版本中“Top-N 邻居”和“热门补全”的思想，不复制字符串哈希、历史去重或不确定排序实现。

## 4. 强制需求

### 4.1 技术约束

1. 主实现继续使用 C11 + OpenMP；除实验绘图和结果处理外，不引入新的第三方运行库。
2. 独立串行函数必须保留，不能用 OpenMP 1 线程代替串行基线。
3. 优化后的串行和 OpenMP 必须调用同一套单用户评分逻辑，避免算法口径分叉。
4. 所有新增配置必须有明确默认值；不传新参数时仍能运行现有命令。
5. 并行区内禁止共享可写候选工作区、直接输出日志或写结果文件。
6. 关键代码继续添加中文注释，重点解释数据所有权、确定性和并行安全。

### 4.2 正确性约束

优化分为两类：

- **语义保持型优化**：只减少计算量，不改变候选集合和数学模型。要求串行/OpenMP 完全一致，并优先要求推荐商品校验和与优化前一致。
- **模型近似型优化**：例如 Top-N 截断，允许推荐列表变化。要求同一配置下串行/OpenMP 完全一致，并单独报告与全图基线相比的效果变化。

所有配置下均必须满足：

- 分数为有限值；
- 每个用户 Top-K 无重复商品；
- 并列时商品 ID 升序；
- Hit Rate 和 Recall 位于 `[0,1]`；
- 1/2/4/8 线程结果稳定；
- 正确性检查不进入正式性能计时区。

### 4.3 实验约束

1. toy 用于手算，small 用于完整回归，medium 用于重复性能和参数选择，large 只验证最终候选配置。
2. 正式性能实验使用 Release 构建，先预热 1 次，再测量至少 3 次并取中位数。
3. 每轮只改变一个主要因素；原始 CSV 不手工修改。
4. 每条结果必须记录算法配置、线程配置、结构统计、阶段时间、效果指标和校验和。
5. 图表必须由原始结果自动生成，不能手工录入数值。

## 5. 优化优先级

### O1：预计算共现边归一化分母（已实施）

当前推荐内层对“用户历史商品 × 共现邻居”反复执行：

```text
sqrt(popularity(source) * popularity(candidate))
```

第一版曾尝试按商品预计算 `1/sqrt(popularity)`，但乘法次序变化导致 small 推荐 ID 校验和改变。为满足语义保持门，最终改为在 CSR 构图时为每条邻接项预计算原公式的完整分母：

```text
denominator(q,p) = sqrt(popularity(q) * popularity(p))
```

推荐内层继续保持原来的运算次序：

```text
frequency * edge_weight / denominator(source,candidate)
```

该方案把开方次数从“每个用户反复遍历每条相关边”降低为“构图时每条边一次”，并保持优化前后的推荐 ID 校验和完全一致。代价是 `CooccurNeighbor` 从 8 字节增加到 16 字节；Top-N 截断会显著降低最终图内存，但构图瞬时峰值仍按完整图产生。

审效结果：toy 排名不变，small 推荐校验和恢复为优化前的 `2830345476833032702`；medium 全图串行推荐中位数由 7054 ms 降到 4297 ms。

### O2：可选 Top-N 邻居截断（最高优先级）

参考串行同学版本，为每个商品只保留共现权重最高的 N 个邻居：

```text
--max-neighbors 0     全图，兼容当前基线
--max-neighbors 20
--max-neighbors 50
--max-neighbors 100
```

实现保持简单：先构建当前 CSR 全图，再对每个商品的邻接段按“权重降序、商品 ID 升序”选择前 N 个，原地压缩邻接数组；压缩后按商品 ID 重新排序，保持评分累加顺序稳定。`0` 表示不截断。

该优化主要减少推荐阶段遍历边数和最终图内存，不承诺降低构图瞬时峰值。若以后确有峰值内存压力，再考虑流式 Top-N 或堆选择，不在本轮加入复杂实现。

审效要求：

- 每个商品度数不超过 N；
- N 大于等于最大原始度数时，结果与全图完全一致；
- 相同权重按商品 ID 稳定选择；
- 截断图允许非对称，但不得出现越界、重复邻居或自环；
- 同一 N 下串行/OpenMP 推荐完全一致；
- medium 对 `N=20/50/100/0` 做速度、边数、Hit Rate、Recall 对比。

建议的候选配置准入线：相对全图 `recommend_ms` 至少降低 20%，同时 Hit Rate 和 Recall 的绝对下降均不超过 0.01。若没有配置满足，不把 Top-N 设为默认值，但仍保留为可选实验功能。

### O3：推荐调度参数化（中优先级）

当前推荐固定为 `schedule(dynamic,16)`。增加简单配置，不改变算法：

- `--recommend-schedule static|dynamic|guided`；
- `--recommend-chunk N`；
- 默认继续使用 `dynamic,16`。

只在 medium 上比较少量组合：`dynamic(4/16/64)`、`guided(4/16)`、`static`。不要进行大规模参数搜索。最终只保留一个默认配置，并把其他配置作为实验参数。

### O4：热门商品补全（低优先级、效果扩展）

仅当用户候选数少于 K 时，从预计算的全局热门商品列表中补全，并提供开关：

```text
--popular-fallback 0|1
```

该功能主要解决空历史或极短历史用户，不应影响候选充足的用户。热门列表只构建一次，不允许每个用户重新排序 popularity。

只有在统计证明存在候选不足用户，且开启后效果指标改善时才进入默认优化配置；否则保留为演示性扩展。

### O5：并行图处理与分桶归并（证据触发）

以下功能暂不直接实现：

- 并行度数统计与邻接填充；
- 并行 Top-N 排序；
- 分桶并行哈希归并；
- 按用户工作量重排任务；
- 时间衰减、类别融合、负采样。

只有当 O1/O2 完成后重新分析，发现邻接图或归并成为主要瓶颈，才从该列表选择一个最简单的方案继续。触发线建议为：某阶段在目标线程数下占 `algorithm_ms` 超过 25%。

## 6. 建议的配置结构

为了避免后续不断增加长参数列表，建议引入两个小型结构：

```c
typedef struct {
    uint32_t top_k;
    uint32_t max_neighbors;      /* 0 表示完整共现图 */
    int popular_fallback;
    double alpha;
    double beta;
    double gamma;
} AlgorithmConfig;

typedef struct {
    int threads;
    OmpSchedule cooccur_schedule;
    int cooccur_chunk;
    OmpSchedule recommend_schedule;
    int recommend_chunk;
} ParallelConfig;
```

第一版可以只暴露确实使用的字段。配置结构的目的不是增加抽象层，而是保证串行/OpenMP、主程序、测试和实验脚本使用同一组参数。

## 7. 实验结果结构化要求

### 7.1 原始 CSV 必备字段

```text
run_id,timestamp,dataset,algorithm_profile,mode,threads,repeat,warmup,
top_k,max_neighbors,popular_fallback,
cooccur_schedule,cooccur_chunk,recommend_schedule,recommend_chunk,
orders,prior_rows,train_rows,products,users,unique_pairs,pair_events,
graph_edge_entries,max_degree,active_users,
load_ms,cooccur_compute_ms,merge_ms,adjacency_ms,truncate_ms,
recommend_ms,evaluate_ms,algorithm_ms,end_to_end_ms,
hit_rate,recall,cooccur_checksum,recommendation_checksum,status
```

其中 `algorithm_profile` 至少区分：

- `baseline-full`：当前全图算法；
- `fast-normalization-full`：O1；
- `fast-normalization-topN`：O1 + O2。

### 7.2 实验清单文件

每次实验目录额外保存 `manifest.json`，记录：

- 操作系统、CPU、物理核/逻辑线程、内存；
- 编译器版本和完整编译参数；
- 可执行文件时间戳；
- 数据集及参数；
- 原始 CSV、汇总 CSV、图表路径；
- 是否通过校验和与指标一致性检查。

### 7.3 汇总规则

- 时间输出中位数，并增加最小值、最大值或标准差；
- Speedup 始终使用同算法配置的独立串行中位数；
- 模型近似配置不得拿全图串行时间计算“纯并行加速比”；应分别报告算法优化收益、并行收益和组合收益；
- 汇总脚本发现缺字段、重复编号、校验和不一致或时间和明显不闭合时必须失败，而不是静默生成图表。

## 8. 推荐实验矩阵

### 8.1 正确性矩阵

| 数据 | 配置 | 线程 | 调度 | 用途 |
| --- | --- | --- | --- | --- |
| toy | full、Top-2、Top-50 | 1/2/4 | static/dynamic/guided | 手算、边界与确定性 |
| small | full、Top-20/50/100 | 1/2/4/8 | 默认调度 | 完整串并行回归 |

### 8.2 性能与效果矩阵

| 数据 | 配置 | 线程 | 重复 | 用途 |
| --- | --- | --- | ---: | --- |
| medium | baseline-full | serial、1/2/4/8 | 3 | 旧基线复核 |
| medium | fast-normalization-full | serial、1/2/4/8 | 3 | O1 收益 |
| medium | fast-normalization-top20/50/100 | serial、8 | 3 | Top-N 筛选 |
| medium | 最佳 Top-N | 1/2/4/8 | 3 | 完整扩展性 |
| large | full 与最终候选配置 | serial、8 | 1 | 最终规模验证 |

large 串行耗时较长，只有 medium 质量门通过后才执行。

## 9. 最终验收标准

1. 当前 `run_correctness.ps1` 继续通过；
2. 新增优化单元测试和负向测试全部通过；
3. 语义保持型优化的推荐 ID 校验和保持不变，或有经过审查的浮点差异说明；
4. Top-N 配置下串行/OpenMP 完全一致；
5. medium 原始实验至少 3 次完整记录，结果可由脚本重建；
6. 最终选择的优化必须有明确收益，未达准入线的功能不设为默认；
7. large 至少完成最终 OpenMP 配置验证；
8. README、测试报告、性能说明和已知问题同步更新；
9. 报告能够分别解释算法优化收益、OpenMP 并行收益和二者组合收益。

## 10. 本轮明确不做

- 重新实现串行同学的 C++ 字符串哈希版本；
- 为提升 Kaggle 排名引入复杂机器学习模型；
- 同时加入时间衰减、类别融合和负采样；
- 未经数据证明就重写哈希表或实现复杂并行归并；
- 将 Top-N 的效果变化混同为纯 OpenMP 加速；
- 为得到更好看的图表删除异常但真实的实验结果。
