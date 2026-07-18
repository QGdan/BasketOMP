# 测试报告

## G0 环境有效性审查

- 状态：通过
- 测试日期：2026-07-16
- GCC 版本：MinGW-W64 GCC 8.1.0（x86_64-win32-seh）
- OpenMP 线程验证：请求 1/2/4，实际创建 1/2/4，全部通过
- Debug 构建：通过，`-O0 -g -Wall -Wextra -Wpedantic -fopenmp`
- Release 构建：通过，`-O2 -DNDEBUG -Wall -Wextra -Wpedantic -fopenmp`
- 构建入口：本机无 GNU Make，使用 `scripts/build.ps1`；保留 Makefile 供其他环境使用
- Sanitizer：本机 MinGW 缺少 `libasan` 和 `libubsan`，不能链接；后续使用断言、边界检查、重复运行和完整结果比较补充验证
- 数据表头：四套数据的 4 个 CSV 均与方案一致
- toy 行数：orders 7、prior 6、train 4、products 4
- small 行数：orders 2,995、prior 26,575、train 1,327、products 6,170
- medium 行数：orders 81,832、prior 761,750、train 33,782、products 29,330
- large 行数：orders 3,421,083、prior 32,434,489、train 1,384,617、products 49,685
- 结论：G0 通过，可以进入阶段 1

## G1 数据加载正确性

- 状态：通过
- toy：7 orders、6 prior、4 train、4 products，购物篮/用户历史/train 真值逐项符合手算
- 空购物篮：订单 13 正确保留为长度 0 的 CSR 区间
- small：2,995 orders、26,575 prior、1,327 train、6,170 products
- 稳定性：toy 连续加载、验证、释放 20 次通过
- 错误路径：不存在的数据目录返回非零并输出错误信息

## G2 哈希表单元测试

- 状态：通过
- 覆盖：无序对编码/解码、空表、重复累加、线性探测冲突、两次以上扩容、批量 reserve、合并、计数溢出
- 随机预言机：固定种子 5,000 次插入与二维简单计数逐项一致
- 稳定性：完整哈希测试重复 100 次通过

## G3 串行共现正确性

- 状态：通过
- toy：`(1,2)=2`、`(1,3)=1`、`(2,3)=1`
- toy 热度：`1=2, 2=3, 3=1, 4=0`
- toy 商品对事件：4
- small：129,071 条唯一边、180,902 次商品对事件

## G4 串并行共现等价性

- 状态：通过
- 数据：toy、small
- 线程：1、2、4
- 调度：static、dynamic
- 重复：toy 每组 20 次，small 每组 2 次
- 比较：热度数组、唯一键数量、每个键计数、事件总数和校验和全部一致

## G5 共现图结构测试

- 状态：通过
- 检查：CSR offset 单调、度数和等于两倍唯一边数、双向权重与哈希表一致、邻居严格按商品 ID 排序

## G6 串行推荐正确性

- 状态：通过
- toy 用户 2 排名：`[2,1,3]`
- 空历史用户返回空列表
- 所有分数有限，Top-K 无重复，次级排序确定

## G7 并行推荐等价性

- 状态：通过
- 数据：toy、small
- 线程：1、2、4
- 重复：toy 每组 20 次，small 每组 2 次
- 比较：每个用户长度、商品 ID、分数（容差 `1e-12`）和校验和全部一致

## G8 指标和端到端集成

- 状态：通过
- toy：Hit Rate@10=`2/3`，宏平均 Recall@10=`0.5`
- `scripts/run_correctness.ps1` 完整通过
- Debug 串行/OpenMP 端到端推荐校验和一致
- 输出文件和推荐样例参数验证通过

## G9 性能有效性

- 状态：通过
- 构建：Release，`-O2 -DNDEBUG -std=c11 -fopenmp`
- medium：串行及 OpenMP 1/2/4/8 线程，每组 3 次，取中位数
- 正式原始数据：`results/raw/runtime-medium-20260716-125503.csv`
- 8 线程：1,569 ms；串行：7,711 ms；Speedup=4.915；Efficiency=61.4%
- static 控制实验已完成；所有组的指标及两类校验和一致
- 已发现并修复 OpenMP 1 线程病态归并，修复后重新执行完整回归和正式实验

## G10 large 与最终交付

- 状态：通过（峰值工作集未自动采集，见已知问题）
- 环境：Ryzen 7 8845H，8 核 16 线程，27.81 GiB 内存
- large：40,749,010 条唯一边、238,428,378 次商品对事件
- 串行算法：1,217,953 ms；8 线程：199,149 ms
- Speedup=6.116；Efficiency=76.4%
- 串行/OpenMP 指标、共现校验和和推荐校验和完全一致
- 完整全图可运行，不需要强制 Top-N 截断
- README、测试报告、性能说明、原始 CSV、汇总 CSV和图表已整理

## O0–O1 基线冻结与语义保持优化

- 状态：通过
- 日期：2026-07-17
- 优化前 medium/large 结果已登记到 `results/baselines/manifest.json`
- 首次按商品预计算逆平方根导致 small 推荐 ID 校验和变化，未通过门禁并被撤回
- 最终方案：在构图时预计算每条边的完整归一化分母，推荐仍执行 `frequency * weight / denominator`
- toy 推荐校验和：`6286369242441534757`，与优化前一致
- small 推荐校验和：`2830345476833032702`，与优化前一致
- medium 完整图串行推荐中位数：7054 ms 降至 4297 ms

## O2 Top-N 邻居截断

- 状态：通过
- 覆盖：N=0、N=1、N 大于最大度数、同权重 ID 次级选择、offset 单调、度数上限、无重复和无自环
- 截断图允许不同商品分别选择 Top-N，因此不要求对称
- 截断后邻接段重新按商品 ID 排序，保证评分累加次序确定
- toy/small 的串行、OpenMP 和 guided 调度逐项一致
- medium 完成 Full、Top-20、Top-50、Top-100 三次重复对比
- large Top-20 Recall 损失超过 0.01，未通过最终质量门
- large Top-50 的 Hit Rate/Recall 绝对损失均小于 0.01，选为最终实验配置

## O3 调度参数化

- 状态：通过
- 共现和推荐分别支持 static、dynamic、guided 和正整数 chunk
- chunk=0 等非法参数返回非零
- 三种调度 toy/small 推荐结果与串行一致
- medium Top-20 8 线程 dynamic/static/guided 总时间约 710/710/700 ms
- 差异不足 2%，保持 dynamic 默认配置

## O4 自动化实验流水线

- 状态：通过
- `run_benchmark.ps1` 支持预热、重复、线程、K、Top-N 和两阶段调度
- 每次实验生成独立 run 目录和 `manifest.json`、`raw.csv`
- `validate_results.py --self-test` 通过
- 验证器可拒绝缺字段、指标越界、完整图边数错误和重复 repeat
- small 冒烟实验成功生成 validation、summary 和 4 张图
- `run_topn_sweep.ps1` small 冒烟通过
- `run_optimization_gate.ps1 -Gate O4` 完整通过

## O5 热门补全触发审查

- 状态：不触发
- medium 完整图有效用户候选不足 K 的数量为 0
- large Top-20/Top-50 候选不足数量为 0
- 结论：不实现热门补全，保留统计字段供以后重新判断

## O6 最终优化配置

- 状态：通过
- medium Top-50：串行及 OpenMP 1/2/4/8，每组 3 次并预热 1 次
- medium 8 线程：`algorithm_ms=730`，Hit Rate=0.84125，Recall=0.308845818812
- large Top-50：串行 `26072 ms`，OpenMP 8 线程 `16495 ms`
- large Top-50：Hit Rate=0.83697764635，Recall=0.296935200991
- 所有正式新实验的 `validation.json` 状态均为 pass

以上 O6 数值是并行邻接构建前的历史阶段结果，当前实现以 O7 为准。

## O7 并行邻接构建与 Top-N

- 状态：通过
- 新增独立 `cooccur_graph_build_openmp`，串行基线继续使用 `cooccur_graph_build`
- 哈希槽按固定逻辑分区；线程局部度数归并后为每个分区预分配无冲突写入区间
- Top-N 按商品动态调度，线程间不共享排序工作区
- toy/small 在 1/2/4 线程下与串行图逐字节一致，完整正确性回归通过
- medium Top-50 三次中位数：串行 1732 ms，OpenMP 8 线程 522 ms，加速比 3.318
- medium Full 三次中位数：串行 4801 ms，OpenMP 8 线程 1074 ms，加速比 4.470
- 两组 `validation.json` 均为 pass，质量指标和两类校验和保持一致
