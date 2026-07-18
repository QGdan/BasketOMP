# 算法优化自动化开发工作流与 TODO

## 1. 工作流目标

基础阶段的 G0–G10 已完成。本工作流从 O0 开始，用于管理第二阶段优化，保证每个改动都经过“需求确认 → 单元测试 → 串并行等价 → medium 审效 → large 验证 → 文档更新”的闭环。

当前仓库尚未配置自动化 CI，因此本阶段采用可重复执行的本地 PowerShell + Python 工作流。文档中标记为“拟新增”的脚本尚未实现，必须按 TODO 完成后才能当作真实命令使用。

## 2. 状态流转

```text
待实现
  ↓
编译与单元测试
  ↓
toy/small 正确性门
  ↓
medium 审效实验
  ↓
收益判定 ──不达标──> 保留实验记录并回退默认配置
  ↓达标
large 最终验证
  ↓
文档与交付更新
```

任何阶段失败时只回到当前优化点，不允许带着已知失败继续叠加下一项优化。

## 3. 自动化脚本规划

### 3.1 保留并增强的现有脚本

| 脚本 | 当前作用 | 本阶段改造 |
| --- | --- | --- |
| `scripts/build.ps1` | 构建主程序和单元测试 | 增加集成测试目标；可选打印编译器与 flags |
| `scripts/run_correctness.ps1` | 完整正确性回归 | 增加 Top-N、调度、参数错误和优化前后校验 |
| `scripts/run_benchmark.ps1` | 批量生成原始 CSV | 参数化 K、Top-N、预热、推荐调度；记录结构与环境字段 |
| `scripts/summarize_results.py` | 中位数、加速比、效率 | 增加 schema 校验、波动统计、同配置基线匹配和失败退出 |
| `scripts/plot_results.py` | 生成四类性能图 | 输出到独立 run 目录；增加 Top-N 性能—效果图 |

### 3.2 拟新增脚本

| 脚本 | 作用 |
| --- | --- |
| `scripts/validate_results.py` | 校验 CSV schema、重复次数、数值范围、校验和和计时闭合 |
| `scripts/run_optimization_gate.ps1` | 按 O0–O6 串联构建、测试、审效和结果验证 |
| `scripts/collect_environment.ps1` | 生成实验环境 JSON，避免报告手工抄写 |

建议入口形式：

```powershell
# 拟新增，当前不可用
.\scripts\run_optimization_gate.ps1 -Gate O2

# 改造后的目标形式
$raw = .\scripts\run_benchmark.ps1 `
  -Dataset medium `
  -Repeats 3 `
  -Warmups 1 `
  -ThreadCounts 1,2,4,8 `
  -TopK 10 `
  -MaxNeighbors 50 `
  -RecommendSchedule dynamic `
  -RecommendChunk 16

python .\scripts\validate_results.py $raw
python .\scripts\summarize_results.py $raw
```

## 4. 结果目录规划

```text
results/
  experiments/
    <run_id>/
      manifest.json
      raw.csv
      summary.csv
      validation.json
      figures/
  baselines/
    baseline-medium.csv
    baseline-large.csv
```

`run_id` 建议使用：

```text
YYYYMMDD-HHMMSS-<dataset>-<profile>
```

旧的 `results/raw`、`results/summary` 和 `results/figures` 保留，不迁移、不覆盖；新结构仅用于第二阶段优化实验。

## 5. 审效门定义

### O0：基线冻结门

必须完成：

- 全量正确性回归通过；
- 保存当前 medium/large 正式结果引用；
- 记录当前推荐校验和、指标和默认参数；
- 确认优化前源码能够独立构建。

通过条件：`run_correctness.ps1` 输出 `PASS: complete correctness regression`，且基线文件可被汇总脚本读取。

### O1：归一化预计算门

必须完成：

- 单元测试覆盖 popularity 为 0、1 和普通正数；
- toy 排名和 Top-K 不变；
- small 串行与 1/2/4/8 线程完全一致；
- medium 对 baseline 和 fast-normalization 各运行 3 次；
- 检查推荐 ID 校验和是否与旧基线一致。

通过条件：正确性全部通过；`recommend_ms` 有稳定改善；若推荐 ID 变化则默认不通过，直到解释并解决。

### O2：Top-N 图门

必须完成：

- toy 构造等权邻居，验证商品 ID 次级选择；
- 验证 N=0 为全图；
- 验证 N 大于最大度数与全图一致；
- 验证每个邻接段不超过 N、无重复、无自环、offset 单调；
- small 的 N=20/50/100 串并行一致；
- medium 输出边数、推荐耗时、Hit Rate 和 Recall。

通过条件：结构与并行正确性通过；至少一个 N 满足规划文档中的性能—效果准入线。若不满足，功能保留但默认仍为全图。

### O3：调度参数门

必须完成：

- 非法调度名、0 或负 chunk 能明确报错；
- static/dynamic/guided 在 toy/small 的结果一致；
- medium 只比较预先规定的少量组合；
- 最终默认配置按中位数选择。

通过条件：结果校验和一致，默认调度相对当前 `dynamic,16` 无显著退化。

### O4：结构化实验脚本门

必须完成：

- 缺少字段时验证脚本失败；
- 重复 run 编号、非法数值、指标越界时失败；
- 同一语义保持配置校验和不一致时失败；
- `algorithm_ms` 与阶段和差异超过容差时给出错误；
- 每次运行生成 manifest、raw、summary、validation 和 figures。

通过条件：正常样例通过，至少 4 类故意破坏的 CSV 被正确拒绝。

### O5：热门补全门（可选）

必须完成：

- 空历史用户按热门列表得到确定性推荐；
- 已购商品是否允许补全必须与当前复购策略一致并写入测试；
- 候选充足用户开启/关闭补全结果相同；
- small/medium 统计真正受影响的用户数。

通过条件：有效果收益或确实解决空推荐问题，否则不设为默认。

### O6：最终交付门

必须完成：

- 选定配置重新跑 medium 完整矩阵；
- large 至少完成 8 线程验证，必要时补一次串行；
- 原始结果通过自动校验；
- 更新 README、测试报告、性能记录和已知问题；
- 报告用表和图可从结果目录重建。

通过条件：代码、结果、图表和文字结论使用同一配置，所有引用能追溯到原始 CSV。

## 6. 详细 TODO 看板

### 阶段 O0：冻结现有基线

- [x] **OPT-000** 精读问题分析与数据处理文档。
- [x] **OPT-001** 精读 `basket_recommender/` 串行 C++ 源码。
- [x] **OPT-002** 精读 `project/` C/OpenMP 核心源码、测试和实验脚本。
- [x] **OPT-003** 核对 medium/large 阶段耗时和瓶颈。
- [x] **OPT-004** 重新运行完整正确性回归。
- [x] **OPT-005** 将当前正式 medium/large 结果登记到 `results/baselines/`。
- [x] **OPT-006** 生成基线 `manifest.json`。

### 阶段 O1：预计算边归一化分母

- [x] **OPT-100** 在邻接项中增加完整归一化分母及所有权定义。
- [x] **OPT-101** 构图时按原公式一次性计算每条边分母。
- [x] **OPT-102** 修改推荐内层循环，移除重复 `sqrt()` 且保持乘后除次序。
- [x] **OPT-103** 验证分母、零热度保护和图结构。
- [x] **OPT-104** 验证 toy 排名和优化前推荐 ID 校验和。
- [x] **OPT-105** 运行 small 串并行与三种调度回归。
- [x] **OPT-106** 运行 medium 优化前后各 3 次审效实验。
- [x] **OPT-107** 记录速度、内存增量和浮点差异结论。

### 阶段 O2：可选 Top-N 邻居

- [x] **OPT-200** 为图构建接口增加 `max_neighbors`，0 表示全图。
- [x] **OPT-201** 实现权重降序、ID 升序的稳定 Top-N 选择。
- [x] **OPT-202** 实现邻接数组原地压缩并更新 offsets。
- [x] **OPT-203** 压缩后按商品 ID 排序，保持评分累加确定性。
- [x] **OPT-204** 修改图验证器以支持有向截断图。
- [x] **OPT-205** 新增等权、N=0、N 超大、度数上限测试。
- [x] **OPT-206** 将 `--max-neighbors` 加入命令行和 key=value 输出。
- [x] **OPT-207** small 执行完整图和 Top-N 串并行回归。
- [x] **OPT-208** medium 执行 N=20/50/100/0 性能—效果实验。
- [x] **OPT-209** medium 选出 Top-20，large 复核后最终选择 Top-50；CLI 默认仍保留全图以兼容旧命令。

### 阶段 O3：推荐调度参数化

- [x] **OPT-300** 扩展调度枚举支持 guided。
- [x] **OPT-301** 将共现调度和推荐调度配置分离。
- [x] **OPT-302** 增加共现/推荐 schedule 与 chunk 参数。
- [x] **OPT-303** 添加非法参数和所有调度结果一致性测试。
- [x] **OPT-304** medium 比较 dynamic/static/guided 控制配置。
- [x] **OPT-305** 三者差异不足 2%，保持 `dynamic,64` 与 `dynamic,16` 默认值。

### 阶段 O4：结构化实验流水线

- [x] **OPT-400** 为主程序输出图规模、候选统计和新增阶段时间字段。
- [x] **OPT-401** benchmark 脚本支持 Warmups、TopK、Top-N 和两阶段调度。
- [x] **OPT-402** benchmark 脚本记录 run_id、时间戳、状态和完整参数。
- [x] **OPT-403** 实现 `collect_environment.ps1` 与 manifest；CIM 受限时安全降级。
- [x] **OPT-404** 实现 `validate_results.py`。
- [x] **OPT-405** 验证器自测覆盖缺字段、指标越界、图边数错误和重复编号。
- [x] **OPT-406** 汇总脚本增加最小值、最大值、标准差和配置匹配。
- [x] **OPT-407** 增加通用 `hit_rate`/`recall` 字段，支持任意 K。
- [x] **OPT-408** 图表输出到独立 run 目录，避免覆盖旧图。
- [x] **OPT-409** 增加 `run_topn_sweep.ps1` 和性能—效果权衡图。

### 阶段 O5：热门补全（达到触发条件才做）

- [x] **OPT-500** 统计空历史、候选不足 K 的用户数量。
- [x] **OPT-501** medium 完整图有效用户候选不足数为 0，确认不触发热门补全。
- [x] **OPT-502** 不满足触发条件，取消热门列表实现，避免无收益复杂度。
- [x] **OPT-503** 保留候选不足统计字段，供以后数据变化时重新判断。
- [x] **OPT-504** large Top-20/50 候选不足数同样为 0，热门补全保持关闭。

### 阶段 O6：最终实验与交付

- [x] **OPT-600** 运行最终 Top-50 medium 串行和 1/2/4/8 线程三次实验。
- [x] **OPT-601** 分别计算算法优化收益、并行加速比和组合收益。
- [x] **OPT-602** 对最终 Top-50 配置运行 large 8 线程。
- [x] **OPT-603** 同一 large 实验补齐独立串行基线。
- [x] **OPT-604** 自动生成最终汇总表和图表。
- [x] **OPT-605** 更新 `README.md`。
- [x] **OPT-606** 更新 `docs/test-report.md`。
- [x] **OPT-607** 更新 `docs/performance-notes.md`。
- [x] **OPT-608** 更新 `docs/known-issues.md`。
- [x] **OPT-609** 新增 `docs/optimization-results.md`，整理报告结论和命令。

## 7. 每个 TODO 的最小工作循环

1. 确认该 TODO 的输入、输出和不变量；
2. 先增加或调整测试，再修改实现；
3. Debug 构建并处理全部警告；
4. 运行最小相关单元测试；
5. 运行 toy 和 small 回归；
6. 语义保持型优化检查旧/新推荐校验和；
7. 当前优化点全部正确后才运行 medium Release 实验；
8. 由验证脚本审查原始结果；
9. 更新 TODO、测试记录和性能说明；
10. 审效门通过后再进入下一阶段。

## 8. 变更审查清单

提交一个优化点前逐项确认：

- [ ] 串行和 OpenMP 是否使用同一算法参数？
- [ ] 新数组/图/工作区的所有权和释放路径是否完整？
- [ ] 是否存在线程共享写、伪共享或并行区输出？
- [ ] 排名并列规则是否仍确定？
- [ ] 参数为 0、1、超大值和非法值时是否处理？
- [ ] Debug 测试是否通过且无新警告？
- [ ] small 的串并行逐项比较是否通过？
- [ ] 性能实验是否使用 Release 和相同输入？
- [ ] 原始 CSV 是否包含完整配置和校验和？
- [ ] 结论是否由至少 3 次 medium 结果支持？

## 9. 完成定义

一个优化点只有同时满足以下条件才标记完成：

1. 实现代码与中文注释完成；
2. 对应单元测试和负向测试完成；
3. toy/small 审效门通过；
4. medium 性能或效果实验完成；
5. 结果验证脚本通过；
6. 达到准入线，或明确记录“不采用为默认”的原因；
7. 文档和 TODO 状态同步更新。
