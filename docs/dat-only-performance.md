# 删除 Pointer Trie 后的内存与性能（2026-09-20）

原始 `Trie.hpp`、`Trie`、`TrieNode`、`PointerWalker` 和 Legacy MP 内核已删除。默认词典现在只使用 raw DAT，并按用户确认统一为启动加载后只读。精确查词、词性、搜索和全模式全部使用 DAT；HMM 算法及其开关保持上一版行为。

**发布评估：代码和对比结果提交到 `dat_merged`，不发布新版本。** 这一修改改善了模型内存，普通 Cut 速度基本保持，查词、搜索和全模式得到额外收益。但默认后端、运行时修改接口和构建失败行为发生变化；与上一版 Legacy 默认相比，当前默认 C 初始化仍约为 3.64×（开启 HMM 优化的 D 约为 3.62×），还需要下游适配及 Windows/ARM64 验证。

## 比较对象与方法

- previous-A：提交 `8b8a3f7` 的默认 Legacy DAG/Pointer Trie，HMM 优化关闭。
- previous-D：同一提交的 DAT + Pointer Trie 并存模式，开启稠密/滚动 HMM；这是本次删除操作的直接基线。
- C：当前仅 DAT，HMM 优化关闭，是现在的默认构造行为。
- D：当前仅 DAT，开启与 previous-D 相同的 HMM 优化。
- 新旧二进制分别链接各自源码，使用同一份 `test/cpu_benchmark.cpp`、词典和语料。GCC 11.4、C++11、`-O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread`；Intel i7-13700、WSL2；CPU 亲和性 `0,2,4,6`，线程共享模型，未逐线程绑核。
- 单线程五轮，每 API/常规语料 2,000 请求，短文本及 Find 40,000 请求；四线程五轮，每 API/常规语料 20,000 请求，短文本及 Find 400,000 请求。每轮打乱版本顺序，每个进程重新构造模型，未清空 OS 文件页缓存。
- 短文本为 8 条查询；文档为 2,048 Rune / 5,786 UTF-8 字节；长 range 为 9,728 Rune / 29,184 字节。Find 用按每 997 行采样的约 128 项词典查询集并加入确定性 miss，输入预解码；不是全词典随机访问吞吐。
- MP 测预解码 range 内核；Find 测预解码的完整词精确查询。其他入口包含解码、预切分和输出构造。MB/s 是十进制原始 UTF-8 字节；原始数据另有 Rune/s、请求/s、P50/P95/P99、每轮结果和波动。诊断分配计数不进入正式计时二进制。

## 初始化与 RSS

五轮单线程进程中位数。RSS 在完整 Jieba 初始化后、读取基准语料及分词前测量。

| 配置 | 初始化 ms | 初始化后 RSS MiB | 初始化峰值 RSS MiB | DAT 构建 ms | 布局 ms | 验证 ms |
|---|---:|---:|---:|---:|---:|---:|
| previous-A | 240.54 | 123.31 | 123.31 | 0.00 | 0.00 | 0.00 |
| previous-D | 893.16 | 148.44 | 148.44 | 654.03 | 529.89 | 15.24 |
| C | 875.87 | 87.19 | 99.75 | 678.07 | 548.96 | 20.44 |
| D | 871.20 | 87.18 | 99.75 | 676.34 | 549.24 | 19.44 |

删除旧 Trie 后，D 的 RSS 减少 **61.25 MiB（41.3%）**。初始化从 893.2 ms 到 871.2 ms，变化较小；位图布局仍是主要成本。当前 D 相比 previous-A 的 RSS 也降低约 29.3%，但初始化仍约 3.62×。

当前模型仍保留地址稳定的 `DictUnit` 词/词性载荷、主词典缓存、HMM、IDF 和停用词。DAT 的净数组为：

| 数组 | 字节 |
|---|---:|
| 802,401 个 64 位 Unit | 6,419,208 |
| 802,401 个 uint32 词条来源索引 | 3,209,604 |
| 5,087 个 FP64 权重 | 40,696 |
| 合计 | **9,669,508（9.22 MiB）** |

新加约 3.06 MiB 来源索引，使相同权重的不同词能返回各自的词条和词性；它替代的是原来整棵 Pointer Trie。MP 热循环仍只访问 Unit/权重，不读取该元数据数组。

## 空闲堆页诊断

另用同一诊断程序分别链接新旧库，保持模型存活，对比 `malloc_trim(0)` 前后。下表是五次独立进程中位数，单位 MiB。**生产代码和正式吞吐基准均没有调用 trim。**

| 配置 | trim 前 RSS | trim 后 RSS | 分配器存活分配量 | 分配器空闲堆 |
|---|---:|---:|---:|---:|
| previous-A | 123.19 | 122.13 | 118.37 | 1.22 |
| previous-D | 148.31 | 129.28 | 125.51 | 19.29 |
| current-C | 87.06 | 65.19 | 61.43 | 22.21 |
| current-D | 87.18 | 66.17 | 62.41 | 21.23 |

所有配置 trim 前后的存活分配量相同。D 的存活分配从 125.51 降至 62.41 MiB，trim 后 RSS 从 129.28 降至 66.17 MiB；因此下降包含实际存活对象减少，不是仅改变了空闲页归还策略。常规 RSS 仍包含构建后分配器保留的页。这里的存活分配量来自 glibc `mallinfo2`，不等同于整个进程 RSS。

## 单线程热态吞吐

五轮中位数。下面直接比较 previous-D 与 D，HMM 设置相同；最后一列的倍数还给出与上一版 Legacy 默认的比较。

| 语料 | 入口 | previous-D MB/s | D MB/s | D / previous-D | D / previous-A |
|---|---|---:|---:|---:|---:|
| 短文本 | MP | 204.88 | 210.30 | 1.026× | 3.49× |
| 短文本 | Cut | 100.15 | 100.47 | 1.003× | 2.24× |
| 短文本 | Search | 66.45 | 76.49 | 1.151× | 2.02× |
| 短文本 | CutSmall(3) | 110.73 | 110.01 | 0.993× | 2.14× |
| 短文本 | 关键词 | 42.56 | 43.45 | 1.021× | 1.55× |
| 短文本 | HMM | 106.19 | 105.01 | 0.989× | 1.29× |
| 短文本 | CutAll | 46.67 | 56.65 | 1.214× | 1.24× |
| 文档 | MP | 458.88 | 455.10 | 0.992× | 9.67× |
| 文档 | Cut | 131.85 | 132.96 | 1.008× | 3.68× |
| 文档 | Search | 101.28 | 107.88 | 1.065× | 3.29× |
| 文档 | CutSmall(3) | 174.16 | 176.23 | 1.012× | 3.74× |
| 文档 | 关键词 | 27.66 | 27.88 | 1.008× | 1.78× |
| 文档 | HMM | 160.59 | 158.06 | 0.984× | 1.61× |
| 文档 | CutAll | 47.81 | 69.89 | 1.462× | 1.45× |
| 长 range | MP | 522.36 | 531.82 | 1.018× | 8.01× |
| 长 range | Cut | 223.06 | 226.19 | 1.014× | 3.92× |
| 长 range | Search | 144.36 | 164.74 | 1.141× | 3.29× |
| 长 range | CutSmall(3) | 221.71 | 221.31 | 0.998× | 3.81× |
| 长 range | 关键词 | 118.55 | 118.69 | 1.001× | 2.57× |
| 长 range | HMM | 167.69 | 168.26 | 1.003× | 1.25× |
| 长 range | CutAll | 53.66 | 58.67 | 1.093× | 1.09× |

Find 热查询集从 **10.94 M 次/s 提高到 19.02 M 次/s（1.74×）**。普通 Cut 约为上一版 D 的 1.00～1.01×；MP/HMM 等未改算法的路径在约百分之几内浮动，不宣称这部分有额外稳定加速。新迁移的 Search、CutAll 和 Find 收益更明显。

## 四线程共享模型

五轮中位数，使用扩大后的请求量。WSL 调度仍有波动，同时列出 D 的 min–max；不保证线性扩展。

| 语料 | 入口 | previous-D MB/s | D MB/s | D / previous-D | D min–max MB/s |
|---|---|---:|---:|---:|---:|
| 短文本 | Cut | 256.97 | 341.86 | 1.330× | 273.03–369.52 |
| 短文本 | Search | 242.85 | 223.52 | 0.920× | 195.06–284.65 |
| 短文本 | CutAll | 131.81 | 147.31 | 1.118× | 114.72–162.18 |
| 文档 | Cut | 435.08 | 423.65 | 0.974× | 401.94–444.58 |
| 文档 | Search | 320.76 | 333.00 | 1.038× | 323.34–351.71 |
| 文档 | CutAll | 160.91 | 225.39 | 1.401× | 216.96–235.66 |
| 长 range | Cut | 745.99 | 738.97 | 0.991× | 727.73–752.87 |
| 长 range | Search | 473.15 | 546.18 | 1.154× | 541.76–560.23 |
| 长 range | CutAll | 177.40 | 192.96 | 1.088× | 189.38–193.25 |

四线程 Find 热查询集：24.18 → 45.69 M 次/s（1.89×）。[全部分位数、标准差和 C/D/A 比较](benchmarks/dat-only-2026-09-20-parallel.summary.json)。


### 四线程短文本搜索的后续检查

组合基准里 D/previous-D 为 **0.920×**，即该组中位数下降约 8%。为检查这个变化，保留原始结果，并将此场景单独扩大到每进程 2,000,000 请求、十轮独立进程。
复测中位数为 248.98 → 279.91 MB/s（1.124×）；新版本范围 229.84–295.51 MB/s，旧版本 238.24–261.25 MB/s。
这说明该并发短请求场景对批次、运行顺序及调度条件敏感：没有复现稳定的下降，也不能据此保证所有并发工作负载都加速。两组数据均保留，发布前仍需目标业务负载验证。[复测原始记录](benchmarks/dat-only-2026-09-20-short-search.jsonl)、[复测汇总](benchmarks/dat-only-2026-09-20-short-search.summary.json)。


## 输出兼容性与接口变化

- `DictUnit`、`Dag`、`MAX_WORD_LENGTH` 移到 `DictTypes.hpp`；原 `Trie.hpp` 不再存在。生产库和测试中均不保留旧 Trie 实现，历史对照从 Git 提交独立编译。
- `CpuCutMode` 只剩 `DatRawFused`，默认就是 DAT。`LegacyDag`、`PointerFused` 和 Legacy DP 路径删除。
- 根据用户确认，所有词典启动后只读。`InsertUserWord` 两个重载及 `DeleteUserWord` 返回 false；公开 `LoadUserDict` 和 `InserUserDictNode` 抛出 `std::logic_error`。启动用户词典、多文件覆盖顺序和单字保护继续支持。
- 精确 Find 仍返回地址稳定的 `const DictUnit*`；Search 存在性查询改用 `Contains`；全模式通过 DAT 枚举候选。
- `L>65535` 改用 `size_t bestLenWide`，通过同一融合内核处理，保留 0、512、513、65535、65536、SIZE_MAX 的调用语义。
- DAT 构建超过预算或格式限制时抛出 `DatBuildError`，其 `GetStats()` 保留失败类别和原因。没有旧 Trie 回退，不返回半成品对象。HMM 稠密表预算不足仍使用哈希发射概率和滚动 Viterbi。

## 验证

- 上一版快照 CTest 3/3；本版 Release CTest 3/3，60 项 GoogleTest 全通过。
- 新旧六个独立进程配置逐字节比较完整输出：正常语料 409 条、348,984 次全词典 Find 探测、启动覆盖、中文分隔符重设及 513/65535/65536 超长词。比较词序、字节/Rune 坐标、全部候选、Find 的 Rune 词/词性/权重、用户单字集合和关键词 FP64 位模式。
- 小词典用独立词表穷举作为参考，覆盖重复词、共享权重但不同载荷、非终点/未命中、24 个同起点候选、正负零、nextafter 权重、MIN_DOUBLE、窄/宽词长与 scratch 复用。
- 另有 411 条文本的同库 HMM 变体、冻结/失败和四线程读取测试；它作为补充，不冒充新旧实现差分。
- DAT/词典/融合测试及全接口差分通过 ASan/UBSan；Clang 14 C++11 差分通过；Bazel 9.2.0 uncached smoke 1/1 通过。安装目录有 DictTypes.hpp，且不再安装 Trie.hpp。

## 重现

```sh
mkdir -p /tmp/cppjieba-dat-merged-before
git archive 8b8a3f7 | tar -x -C /tmp/cppjieba-dat-merged-before
cmake -S . -B build/dat-only -DCMAKE_BUILD_TYPE=Release
cmake --build build/dat-only -j4
ctest --test-dir build/dat-only --output-on-failure
# 同一基准源，链接上一版：
c++ -std=c++11 -O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread \
  -DCPPJIEBA_CPU_PREVIOUS -I/tmp/cppjieba-dat-merged-before/include \
  test/cpu_benchmark.cpp /tmp/cppjieba-dat-merged-before/src/DatBuilder.cpp \
  -o /tmp/previous-benchmark
python3 test/run_cpu_benchmark.py --binary build/dat-only/cpu_benchmark \
  --previous-binary /tmp/previous-benchmark --previous-modes A,D --modes C,D \
  --repeats 5 --iterations 2000 --threads 1 --cpus 0,2,4,6 --output dat-only.jsonl
# 将 --modes 改为 D、--threads 改为 4、--iterations 改为 20000 可复现并发组。
# dat_only_oracle.cpp 同样分别链接新旧库，再执行：
python3 test/compare_dat_only.py --previous /tmp/previous-oracle \
  --current build/dat-only/dat_only_oracle --all-dictionary-words
```

原始证据：[单线程 JSONL](benchmarks/dat-only-2026-09-20.jsonl)、[单线程汇总](benchmarks/dat-only-2026-09-20.summary.json)、[四线程 JSONL](benchmarks/dat-only-2026-09-20-parallel.jsonl)、[内存诊断](benchmarks/dat-only-2026-09-20-memory.json)、[独立进程正确性报告](benchmarks/dat-only-2026-09-20-correctness.json)、[源文件摘要](benchmarks/dat-only-2026-09-20.sources.json)。
