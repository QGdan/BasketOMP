# 分桶并行归并实现与实验说明

## 1. 实现目标

学校平台的 large Top-50 实验表明，线程数增加后共现计算、邻接构建和推荐都能继续缩短，但线程局部共现表的串行归并反而从 1 线程约 1176 ms 增长到 24 线程约 5553 ms、48 线程约 7263 ms。48 线程时归并约占 `algorithm_ms` 的 66.8%，已经成为新的主瓶颈。

本轮优化的目标是：保持商品对计数、商品热度、推荐结果和全部校验和不变，把“所有线程依次写一个全局哈希表”改成“多个桶可同时归并”，并保持 C11 + OpenMP 实现简单、可解释。

## 2. 核心数据布局

商品对仍使用 64 位键：

```c
key = ((uint64_t)min(product_a, product_b) << 32)
    | max(product_a, product_b);
mixed = mix64(key);
bucket = (mixed >> 32) % bucket_count;  // 高位选桶，低位用于桶内探测
```

OpenMP 共现阶段中，每个工作线程拥有 `bucket_count` 个私有 `PairHashMap`。同一商品对无论由哪个线程处理，都会进入相同逻辑桶；不同线程仍不共享任何局部哈希表。

最终全局表仍使用一个连续的 `PairEntry entries[]`，同时增加：

```c
size_t bucket_count;
size_t *bucket_offsets;  // 长度 bucket_count + 1
```

桶 `b` 的合法槽位为：

```text
entries[bucket_offsets[b] .. bucket_offsets[b+1])
```

每个桶根据所有线程对应局部桶的条目数上界单独计算容量，维持不超过约 0.70 的负载因子。各桶槽位范围互不重叠，因此可以直接形成最终查询表，不需要在并行归并后再做一次串行拼接。

## 3. 无锁并行归并流程

```text
订单级 OpenMP 共现
  -> 每线程、每桶私有哈希表
  -> 计算每桶局部条目数上界
  -> 一次性分配连续分桶目标表
  -> OpenMP dynamic,1 并行遍历 bucket
       -> 当前任务独占目标桶
       -> 依次合并所有线程的同编号局部桶
  -> 汇总各桶唯一键数
  -> 按 product_id 并行归约 popularity
```

并行归并区不使用 `omp_lock_t`、`critical` 或原子计数。安全性来自写所有权：一个桶在同一时刻只由一个 OpenMP 迭代处理，而不同桶对应不重叠的地址范围。

`pair_map_merge_into_bucket()` 还会重新计算源键所属桶；发现路由不一致、容量不足或计数溢出时立即返回失败。

## 4. 参数和结构化输出

新增命令行参数：

```text
--merge-buckets N
```

- `N=0`：自动选择；约取线程数的 4 倍并向上取 2 的幂，限制在 64–1024；
- 8 线程自动选择 64 桶；
- 48 线程自动选择 256 桶；
- `N=1`：仍走同一分桶数据路径，但只有一个归并任务，可作为受控对照；
- 可显式测试 128、256、384 等桶数；允许非 2 的幂桶数；
- 上限为 4096，防止错误参数造成大量空哈希表。

程序和实验 CSV 新增：

```text
merge_strategy=serial|bucket-serial|bucket-parallel
merge_buckets=<实际桶数>
```

`merge_ms` 包含目标分桶表分配、桶内归并、唯一键数汇总和商品热度归约。`cooccur_compute_ms` 包含局部键路由及局部分桶哈希表更新，因此评估桶数时不能只看 `merge_ms`，应以 `algorithm_ms` 为主。

## 5. 正确性审效

已完成的审效包括：

1. `test_pair_hash` 验证 7 桶这种非 2 的幂布局的插入、重复累加、查询、遍历和禁止整体 rehash；
2. `test_cooccurrence` 在 toy/small 上覆盖 1/2/4 线程、static/dynamic/guided 调度和自动/7/64 桶；
3. 每种组合完整比较商品热度数组、每个商品对计数、事件数和稳定校验和；
4. 完整图及 Top-N 图继续与串行图逐字节一致；
5. 推荐与评估集成测试保持通过；
6. `run_correctness.ps1` 完整回归通过；
7. 结构化 small 冒烟实验的 `validation.json` 状态为 `pass`；
8. 4097 桶等越界参数被拒绝。

small 正确性基准仍为：129,071 个唯一商品对、180,902 次商品对事件，共现校验和 `18436125889662285428`。

## 6. 本地 medium 诊断实验

环境为当前 Windows 开发机，Release、medium Top-50、8 线程，每个桶数运行 3 次并取中位数。该结果只用于验证实现趋势，不替代学校平台正式实验。

| 桶数 | cooccur_compute_ms | merge_ms | algorithm_ms | 共现校验和 |
| ---: | ---: | ---: | ---: | --- |
| 1 | 67 | 189 | 397 | 一致 |
| 64 | 79 | 27 | 242 | 一致 |
| 256 | 82 | 25 | 243 | 一致 |

审效中发现初版“低位选桶、低位选槽”会让同一桶内的键从相同槽位附近开始探测，因此改为“混合哈希高位选桶、低位选槽”。修复后，64 桶把归并中位数从 189 ms 降到 27 ms，约为 7.0 倍归并加速；总算法时间从 397 ms 降到 242 ms，约改善 39.0%。256 桶归并为 25 ms，但总算法时间为 243 ms，与64桶接近。

因此自动策略在 8 线程选择 64 桶是合理的：它与256桶性能相当，但固定元数据和空桶开销更小；48 线程平台是否应选择 128、256 或 384，必须由 large 实测决定。

## 7. 学校平台正式实验

首先运行完整正确性门，然后对相同数据和线程点只改变桶数：

推荐直接使用专用脚本，它会为每个桶数调用正式平台运行器，并生成 `bucket-comparison.csv`：

```bash
python3 scripts/run_bucket_sweep.py \
  --executable build/basket_recommender \
  --data data/large --dataset large \
  --threads 8,16,24,32,48 --max-threads 48 \
  --buckets 1,64,128,256,384 --baseline-buckets 1 \
  --repeats 5 --serial-repeats 3 --warmups 1 \
  --max-neighbors 50 --proc-bind spread --places cores \
  --correctness
```

专项脚本会额外输出：

- `validation.json`：跨桶数比较 checksum、图规模和推荐质量指标，任何不一致都会终止扫描；
- `bucket-comparison.csv`：包含 `merge_share_percent`、共现流水线耗时、相对单桶加速、总算法改善率和排名；
- `bucket-report.md`：按线程数列出 `algorithm_ms` 最低的最佳桶数；
- `figures/`：五张可直接用于报告的桶数敏感性图。

也可以手工逐组运行：

```bash
python3 scripts/run_platform_benchmark.py --correctness

for buckets in 1 128 256 384; do
  python3 scripts/run_platform_benchmark.py \
    --executable build/basket_recommender \
    --data data/large --dataset large \
    --threads 8,16,24,32,48 --max-threads 48 \
    --repeats 5 --serial-repeats 3 --warmups 1 \
    --max-neighbors 50 --merge-buckets "$buckets" \
    --proc-bind spread --places cores
done
```

正式比较应检查：

- 所有桶数和线程数的两类 checksum 完全一致；
- `validation.json` 为 `pass`；
- 分别绘制 `cooccur_compute_ms`、`merge_ms`、`algorithm_ms`；
- 计算相对 1 桶的归并加速和总算法改善；
- 同时记录峰值内存，或至少记录平台总内存与运行是否成功；
- 选择 `algorithm_ms` 中位数最低且波动可接受的桶数，而不是选择 `merge_ms` 最低的桶数。

## 8. 复杂度与内存权衡

设线程数为 `T`、桶数为 `B`、线程局部唯一键总数为 `M`、最终唯一键数为 `U`：

- 共现更新仍近似 `O(pair_events)`；
- 归并扫描全部局部条目，工作量 `O(M)`；
- 桶任务提供至多 `min(B,T)` 个并行执行单元；
- 最终表空间近似 `O(U / 0.7)`，外加 `B+1` 个桶偏移；
- 局部表总键数不变，但每线程至少创建 `B` 个 16 槽小表，所以过大的 B 会增加固定内存和缓存压力。

该方案有意避免更复杂的并发 CAS 哈希表。它以少量分桶元数据换取清晰的无锁所有权边界，更适合课程项目解释、调试和正确性证明。

## 9. 当前结论

分桶并行归并已从“预计收益策略”转为可运行实现，并通过 toy/small 全回归及 medium 诊断。当前本地最优是 8 线程 64 桶；学校 48 线程 large 的最终桶数和正式加速比尚待重新实验，报告中不能把此前 3–6 倍归并收益估算当作实测结果。
