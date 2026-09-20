# dat_merged 与 master 的性能及内存比较（2026-09-20）

本次直接编译远端 master 的源码进行比较。**结果：新版热态分词更快，初始化后 RSS 约降低 29.3%；初始化约慢 3.5×。** 本次未修改库算法、未发布版本；冷启动成本和只读/链接接口差异仍是发布前必须考虑的条件。

## 版本与测量边界

- master：`103e1a2c9f06d80705d261ab1cd66bc48c090abd`（本次用 `git ls-remote`、`git fetch` 核对）。原 Pointer Trie + DAG + 原 HMM；原始记录标为 `original-A`。
- dat_merged：`59310b0229d52212d87680dbb4233dea1c6d3ff1`。C 为现在的默认配置：DAT + 融合 DP，HMM 优化关闭；D 另外开启稠密发射表和滚动 HMM。
- master 直接使用其原始头文件；新版链接其 DatBuilder.cpp。两边编译同一份基准源：GCC 11.4、C++11、`-O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread`。没有拿上一版 DAT+旧 Trie 的性能替代 master。
- 五份词典和语料已逐字节确认相同。短文本为 8 条查询；文档为 2,048 Rune / 5,786 UTF-8 字节；长 range 为 9,728 Rune / 29,184 字节。Find 是约 128 项预解码热查询集，包含确定性 miss。
- Intel i7-13700 / WSL2，CPU 亲和性 `0,2,4,6`。单线程及四线程共享一个模型，未逐线程固定核。每组五轮独立进程、确定性打乱版本顺序，报告中位数并保留每轮及 min/max/标准差。
- 单线程每 API/语料 2,000 请求，短文本和 Find 40,000；四线程为 20,000，短文本和 Find 400,000。模型重新构造，OS 文件页缓存没有清空。
- MP 与 Find 使用预解码输入；其他入口包含解码、预切分、结果构造。MB/s 使用十进制原始 UTF-8 字节，原始记录另含 Rune/s、请求/s 和 P50/P95/P99。
- 性能构建不启用逐字符和分配计数。独立诊断构建统计 new/new[] 请求的字节和次数，排除分配器元数据、栈和其他直接 malloc；其耗时不参与性能比较。

## 冷启动和常驻内存

五轮中位数。RSS 在完整 Jieba 构造结束、读取基准语料及执行分词之前采集；包含词典、HMM、IDF、停用词、库代码和分配器保留页。

| 配置 | 初始化 ms | 初始化后 RSS MiB | 初始化峰值 RSS MiB | RSS / master | 初始化 / master |
|---|---:|---:|---:|---:|---:|
| master | 257.24 | 123.31 | 123.31 | 1.000× | 1.00× |
| dat_merged 默认 C | 910.86 | 87.18 | 99.62 | 0.707× | 3.54× |
| dat_merged + HMM D | 902.29 | 87.18 | 99.62 | 0.707× | 3.51× |

默认 C 的 RSS 减少 36.12 MiB（29.3%），构造额外花费约 654 ms。DAT 构建约 707 ms，其中位图布局约 573 ms。不能只用热态吞吐评价频繁构造模型的场景。

## 单线程完整路径吞吐

五轮中位数。倍数表示新版吞吐为 master 的多少倍。

| 语料 | 入口 | master MB/s | 默认 C MB/s | C / master | HMM D MB/s | D / master |
|---|---|---:|---:|---:|---:|---:|
| 短文本 | MP 内核 | 58.75 | 204.34 | 3.48× | 203.35 | 3.46× |
| 短文本 | Cut | 43.64 | 90.47 | 2.07× | 96.92 | 2.22× |
| 短文本 | Search | 36.60 | 69.46 | 1.90× | 74.75 | 2.04× |
| 短文本 | CutSmall(3) | 50.98 | 105.18 | 2.06× | 104.45 | 2.05× |
| 短文本 | 关键词 | 26.72 | 39.87 | 1.49× | 41.71 | 1.56× |
| 短文本 | CutHMM | 80.12 | 78.92 | 0.98× | 102.01 | 1.27× |
| 短文本 | CutAll | 44.31 | 54.50 | 1.23× | 54.69 | 1.23× |
| 文档 | MP 内核 | 46.78 | 444.43 | 9.50× | 434.98 | 9.30× |
| 文档 | Cut | 35.27 | 96.91 | 2.75× | 128.51 | 3.64× |
| 文档 | Search | 32.06 | 81.36 | 2.54× | 101.65 | 3.17× |
| 文档 | CutSmall(3) | 46.74 | 170.74 | 3.65× | 171.10 | 3.66× |
| 文档 | 关键词 | 15.28 | 24.66 | 1.61× | 26.69 | 1.75× |
| 文档 | CutHMM | 96.91 | 96.89 | 1.00× | 152.46 | 1.57× |
| 文档 | CutAll | 47.06 | 66.44 | 1.41× | 67.78 | 1.44× |
| 长 range | MP 内核 | 64.79 | 508.82 | 7.85× | 509.91 | 7.87× |
| 长 range | Cut | 56.62 | 216.54 | 3.82× | 217.99 | 3.85× |
| 长 range | Search | 48.10 | 156.81 | 3.26× | 159.36 | 3.31× |
| 长 range | CutSmall(3) | 56.76 | 211.28 | 3.72× | 212.56 | 3.74× |
| 长 range | 关键词 | 44.73 | 112.77 | 2.52× | 113.48 | 2.54× |
| 长 range | CutHMM | 129.34 | 128.77 | 1.00× | 161.35 | 1.25× |
| 长 range | CutAll | 51.08 | 55.76 | 1.09× | 56.12 | 1.10× |

C 的 HMM 算法与 master 相同，其 CutHMM 结果约为 0.98～1.00×，没有明显加速；D 才包含 HMM 优化。MP 微内核可达到约 9.5×，但完整 Cut 还要执行解码、HMM 和输出构造，因此不能把该倍数当成完整服务的收益。

| Find 热查询集 | master | 默认 C | HMM D |
|---|---:|---:|---:|
| M 次/s | 10.39 | 18.29 | 18.24 |

Find 是迭代器精确查词，包含每次请求计时开销，不包含 UTF-8 解码；不是全词典随机访问的速度保证。

## 四线程共享模型

同样取五轮中位数，采用扩大请求量的独立测试。虚拟化环境仍有调度波动，D 的 min–max 一并列出，不能直接承诺线性扩展。

| 语料 | 入口 | master MB/s | C / master | D / master | D min–max MB/s |
|---|---|---:|---:|---:|---:|
| 短文本 | Cut | 150.83 | 2.02× | 2.29× | 258.20–357.33 |
| 短文本 | Search | 126.06 | 1.92× | 2.14× | 179.51–294.64 |
| 短文本 | CutAll | 144.73 | 1.05× | 1.02× | 131.82–153.81 |
| 文档 | Cut | 128.62 | 2.55× | 3.38× | 399.80–441.07 |
| 文档 | Search | 115.83 | 2.23× | 3.00× | 311.77–357.82 |
| 文档 | CutAll | 156.91 | 1.38× | 1.45× | 203.92–231.95 |
| 长 range | Cut | 193.29 | 3.86× | 3.84× | 691.18–772.37 |
| 长 range | Search | 157.21 | 3.50× | 3.48× | 513.26–551.31 |
| 长 range | CutAll | 178.32 | 1.03× | 1.05× | 177.16–192.18 |

[四线程全部指标、分位数和波动](benchmarks/master-2026-09-20-parallel.summary.json)。


## 请求临时内存和分配

这部分是独立诊断结果，不和模型 RSS 混算。每次请求重新创建输出及 scratch；同一次请求的多个 range 会复用 scratch。

| 文档 / 入口 | master 分配次数 | 默认 C | HMM D | master 累计申请 KiB | 默认 C KiB | HMM D KiB |
|---|---:|---:|---:|---:|---:|---:|
| 文档/MP | 2916 | 3 | 3 | 268.30 | 36.13 | 36.13 |
| 文档/Cut | 4928 | 1699 | 685 | 446.00 | 242.17 | 221.26 |
| 文档/HMM | 723 | 723 | 17 | 267.11 | 267.11 | 165.59 |
| 长 range/MP | 17934 | 3 | 3 | 1283.98 | 171.13 | 171.13 |
| 长 range/Cut | 17926 | 7 | 7 | 2141.00 | 1004.13 | 1004.13 |
| 长 range/HMM | 6 | 6 | 5 | 1237.00 | 1237.00 | 752.50 |

调用峰值并不总随分配次数下降：文档 Cut 的最大同时存活请求分配为 master 160,500 B、C 160,708 B、D 160,723 B，结果及其他缓冲仍占主要部分；长 range Cut 则从 1,700,864 B 降至 856,200 B。上述数字不含模型和输入语料，也不是整个进程 RSS。

## 分配器空闲页的独立检查

另用保持模型存活的 Linux/glibc 诊断程序，比较 `malloc_trim(0)` 前后；五轮中位数。正式性能程序及库代码没有调用 trim。

| 配置 | trim 前 RSS MiB | trim 后 RSS MiB | 存活分配 MiB | 空闲堆 MiB |
|---|---:|---:|---:|---:|
| master | 123.06 | 122.01 | 118.37 | 1.22 |
| C | 87.06 | 65.19 | 61.43 | 22.21 |
| D | 87.06 | 66.17 | 62.41 | 21.23 |

各版本 trim 前后的存活分配量不变。新版真实存活分配显著减少，同时构建结束后保留的空闲堆页更多，所以普通 RSS 只下降约 29%，而 trim 后的差距更大。D 的额外 HMM 数组约 0.98 MiB，能使用 C 已保留的堆页，因此 C/D 的普通 RSS 接近不代表两者存活分配相同。

## 兼容性与验证

- 所有计时进程校验输入字节、Rune 数、输出 token 数及实际 DAT/HMM 后端；没有构建失败或回退数据。当前源码 CTest 3/3、60 项单测通过。
- 库源码与被测提交完全相同，本次仅新增测量记录与报告。初始化后只读场景下的更完整新旧差分及 sanitizer 证据见 [删除 Trie 的报告](dat-only-performance.md)。
- master 支持原来的运行时词典修改；新版按此前确认统一为启动加载后只读。新版需要链接 cppjieba 构建器库，且 DAT 构建失败会抛出 DatBuildError。该接口差异不计入只读性能收益。
- 本机是 WSL2/x86-64；没有用本次数据声称 Windows/ARM64 性能或发布验收完成。

## 重现与原始数据

```sh
git fetch origin master
mkdir -p /tmp/cppjieba-master-103e1a2
git archive 103e1a2 | tar -x -C /tmp/cppjieba-master-103e1a2
c++ -std=c++11 -O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread \
  -DCPPJIEBA_CPU_BASELINE -I/tmp/cppjieba-master-103e1a2/include \
  test/cpu_benchmark.cpp -o /tmp/master-benchmark
c++ -std=c++11 -O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread \
  -Iinclude test/cpu_benchmark.cpp src/DatBuilder.cpp -o /tmp/current-benchmark
python3 test/run_cpu_benchmark.py --binary /tmp/current-benchmark \
  --baseline-binary /tmp/master-benchmark --modes C,D --repeats 5 \
  --iterations 2000 --threads 1 --cpus 0,2,4,6 --output master-compare.jsonl
# 四线程组使用 --iterations 20000 --threads 4。
```

证据：[单线程原始记录](benchmarks/master-2026-09-20.jsonl)、[单线程汇总](benchmarks/master-2026-09-20.summary.json)、[四线程原始记录](benchmarks/master-2026-09-20-parallel.jsonl)、[master 请求分配记录](benchmarks/master-2026-09-20-allocations.json)、[空闲页诊断](benchmarks/master-2026-09-20-memory.json)、[版本与源码摘要](benchmarks/master-2026-09-20.sources.json)。
