# dat_merged 分支编译与测试指南

本文对应 `lbsswhu/cppjieba` 的 **`dat_merged`** 分支。对话中写作 `dat_merge` 的分支，实际名称是 `dat_merged`；2026-09-21 核对时远端没有 `dat_merge`。本文依据提交 `7336ab0` 的构建目标和脚本编写。

以下命令以 **Linux / WSL、Bash** 为准，除获取源码步骤外均在仓库根目录执行。日常编译和正确性检查完成第 1～3 节即可，后续性能、跨版本差分和诊断按需执行。

## 1. 准备环境与源码

需要 C++11 编译器、CMake、Git；运行 Python 基准及差分脚本还需要 Python 3。本文验证环境为 GCC 11.4、Clang 14、CMake 3.22.1、Python 3.10。

本文完整命令按 CMake 3.20 或以上编写，其中使用了 `ctest --test-dir`。项目声明的 CMake 最低版本是 3.10，但完整测试用到了 CMake 3.14 起提供的 `FetchContent_MakeAvailable`；旧 CTest 可以进入构建目录后执行 `ctest --output-on-failure`。

Ubuntu / Debian 环境可安装：

```bash
sudo apt update
sudo apt install build-essential cmake git python3
```

首次获取此分支：

```bash
git clone --branch dat_merged --single-branch https://github.com/lbsswhu/cppjieba.git
cd cppjieba
git branch --show-current
```

最后一条应输出 `dat_merged`。已有仓库可在保存好本地修改后执行：

```bash
git fetch origin
git switch dat_merged
git pull --ff-only origin dat_merged
```

CMake 首次配置测试会从 GitHub 获取 GoogleTest **`release-1.12.1`**。源码树需要保留 `dict/` 和 `test/testdata/`；这些测试数据已经在 Git 仓库中，无需生成 DAT 模型文件。

## 2. Release 编译

```bash
cmake -S . -B build/dat-merged-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=11

cmake --build build/dat-merged-release --parallel 4
```

`--parallel 4` 是编译并行度，可根据机器资源调整。性能测试务必使用 Release；不要把 Debug 或 sanitizer 构建的耗时混入比较。

主要产物如下，路径对应 Linux 单配置生成器：

| 产物 | 用途 |
|---|---|
| `build/dat-merged-release/libcppjieba.a` | DAT 构建器静态库，调用方需要链接 |
| `build/dat-merged-release/test/test.run` | GoogleTest 单元测试 |
| `build/dat-merged-release/load_test` | 整篇文本分词、关键词提取的加载/循环检查 |
| `build/dat-merged-release/cpu_differential` | 当前实现的 HMM 变体、只读契约、边界及并发读取检查 |
| `build/dat-merged-release/dat_only_oracle` | 跨版本比较使用的完整输出生成程序 |
| `build/dat-merged-release/benchmark_bin` | 传统 `make benchmark` 对应的基准 |
| `build/dat-merged-release/cpu_benchmark` | C/D 配置的正式吞吐、延迟、RSS 基准 |
| `build/dat-merged-release/cpu_diagnostics` | 分配次数、申请字节及 MP 计数诊断 |

仅构建库、不下载 GoogleTest 或构建测试程序：

```bash
cmake -S . -B build/dat-merged-lib \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPPJIEBA_TOP_LEVEL_PROJECT=OFF
cmake --build build/dat-merged-lib --parallel 4
```

本项目用 `CPPJIEBA_TOP_LEVEL_PROJECT` 控制测试目标；不要依赖未接入的 `BUILD_TESTING=OFF` 来关闭它们。

## 3. 运行正确性测试

运行 CTest 中的全部目标：

```bash
ctest --test-dir build/dat-merged-release --output-on-failure
```

当前注册了三个目标：`./test/test.run`、`./load_test`、`cpu_differential`。预期结尾为：

```text
100% tests passed, 0 tests failed out of 3
```

CTest 的 **3 个目标**和 GoogleTest 内部的测试数量不同。本文对应代码的 `test.run` 内含 **60 项单测**，可以独立执行：

```bash
./build/dat-merged-release/test/test.run
```

只运行 DAT、融合 DP、HMM、词典查询相关测试：

```bash
./build/dat-merged-release/test/test.run \
  --gtest_filter='DatCpuTest.*:FusedCPU.*:HmmCpuTest.*:DictTrieTest.*'
```

单独执行一致性和并发读取检查：

```bash
./build/dat-merged-release/cpu_differential "$PWD" 300
```

第一个参数是仓库根目录，第二个是随机文本数量。`300` 加上固定输入和仓库语料，当前共检查 411 条文本，同时覆盖四线程共享读取、超长词、冻结写入口和构建失败。该程序比较的是**同一新版库中的 HMM 配置**；跨版本完整输出比较见第 7 节。

遇到失败可查看构建目录下的 `Testing/Temporary/LastTest.log`。非法 UTF-8、空文本等负例会产生预期的 `Decode failed` 或 `words illegal` 日志；测试退出码、`FAILED` 断言和 sanitizer 报告仍须检查。

## 4. 性能与分配诊断

### 4.1 快速运行传统基准

```bash
cmake --build build/dat-merged-release --target benchmark
```

该目标运行 `benchmark_bin`，输出词典/HMM 加载、MP/Mix 分词、查词等指标。它使用当前默认配置，HMM 保持原算法；它不属于 CTest 的三个目标。

### 4.2 选择 C 或 D 配置

当前 `cpu_benchmark` 支持：

| 模式 | 含义 |
|---|---|
| `C` | DAT + 融合 DP，HMM 保持原算法；当前默认配置 |
| `D` | C 的基础上开启 HMM 稠密发射表和滚动分数数组 |

两种配置都可以进行 HMM 分词，区别在于是否优化 HMM 的内部存储与递推。当前二进制不支持旧的 A/B 后端。

```bash
./build/dat-merged-release/cpu_benchmark "$PWD" \
  --mode C --threads 1 --iterations 2000 --corpus document --api cut

./build/dat-merged-release/cpu_benchmark "$PWD" \
  --mode D --threads 4 --iterations 20000 --corpus document --api cut
```

`--api` 支持 `mp`、`cut`、`search`、`small`、`keywords`、`hmm`、`full`、`find`、`all`。`--corpus` 支持 `short`、`document`、`long`、`lookup`、`full_document`、`all`；`all` 不包括完整文档 `full_document`，查词性能建议配合 `lookup`。

二进制的仓库目录是第一个位置参数；Python 包装脚本则使用 `--repo`，不要混用。

### 4.3 多轮记录和诊断

```bash
mkdir -p build/results
python3 test/run_cpu_benchmark.py \
  --repo "$PWD" \
  --binary build/dat-merged-release/cpu_benchmark \
  --diagnostics-binary build/dat-merged-release/cpu_diagnostics \
  --modes C,D --repeats 5 --iterations 2000 --threads 1 \
  --output build/results/dat-merged.jsonl
```

输出包含逐轮 `.jsonl` 和同名 `.summary.json`，记录初始化、RSS、吞吐、P50/P95/P99 及波动。分配计数在独立诊断进程中采集，不参与正式吞吐比较。

四线程比较可把参数改为 `--threads 4 --iterations 20000`。短文本及查词实际执行请求数是 `iterations` 的 20 倍；线程共享同一个模型，不是每线程构建一个模型。

Linux 上可用 `--cpus` 限定 CPU 集合。先检查当前进程允许的编号，再给新旧版本使用同一组：

```bash
python3 -c 'import os; print(sorted(os.sched_getaffinity(0)))'
```

不要直接假设每台机器都允许 `0,2,4,6`。测试期间避免并行编译；初始化时间、热态吞吐、模型 RSS 和请求临时内存应分别比较。

## 5. 与 master 比较性能

下面固定使用本轮报告中的 master 提交 **`103e1a2`**。它是本分支的历史祖先，前面的完整克隆包含该提交；浅克隆需要先补齐历史。

```bash
mkdir -p build/reference-master
git archive 103e1a2 | tar -x -C build/reference-master

c++ -std=c++11 -O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread \
  -DCPPJIEBA_CPU_BASELINE -Ibuild/reference-master/include \
  test/cpu_benchmark.cpp -o build/master-benchmark

c++ -std=c++11 -O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread \
  -Iinclude test/cpu_benchmark.cpp src/DatBuilder.cpp \
  -o build/current-benchmark

python3 test/run_cpu_benchmark.py \
  --repo "$PWD" \
  --binary build/current-benchmark \
  --baseline-binary build/master-benchmark \
  --modes C,D --repeats 5 --iterations 2000 --threads 1 \
  --output build/results/master-compare.jsonl
```

同一份基准源分别编译、链接各自版本的库；master 当时是纯头文件实现，所以它的命令不包含 `src/DatBuilder.cpp`。这里不需要切换当前工作分支。

## 6. ASan / UBSan 检查（Linux，选做）

下面直接编译一致性程序，不依赖 GoogleTest，也不覆盖正式 Release 产物：

```bash
c++ -std=c++11 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -fno-fast-math -ffp-contract=off -pthread \
  -Iinclude test/cpu_differential.cpp src/DatBuilder.cpp \
  -o build/cpu-differential-asan

ASAN_OPTIONS=detect_leaks=1 \
  ./build/cpu-differential-asan "$PWD" 30
```

`30` 表示随机输入数量；仍会执行固定边界、超长词、失败模型和共享模型读取用例。预期退出码为 0，且没有 AddressSanitizer、UndefinedBehaviorSanitizer 或 LeakSanitizer 错误。此程序用于正确性诊断，不用于性能结论。

## 7. 跨版本完整输出差分（选做）

`compare_dat_only.py` 的旧版参考是 **`8b8a3f7`**，即尚未删除 Pointer Trie、同时支持 A/C/D 的版本。这一节用于验证删除 Trie 前后的输出兼容性，和第 5 节的 master 性能比较是不同用途；不能把 master 直接作为 `CPPJIEBA_CPU_PREVIOUS` 编译。

```bash
mkdir -p build/reference-before-removal
git archive 8b8a3f7 | tar -x -C build/reference-before-removal

c++ -std=c++11 -O3 -DNDEBUG -fno-fast-math -ffp-contract=off -pthread \
  -DCPPJIEBA_CPU_PREVIOUS -Ibuild/reference-before-removal/include \
  test/dat_only_oracle.cpp build/reference-before-removal/src/DatBuilder.cpp \
  -o build/previous-oracle

python3 test/compare_dat_only.py \
  --repo "$PWD" \
  --previous build/previous-oracle \
  --current build/dat-merged-release/dat_only_oracle \
  --all-dictionary-words \
  --output-dir build/results/independent-comparison
```

脚本逐字节比较六种独立进程配置的完整输出，包含候选、token、字节/Rune 坐标、词性、关键词权重、启动覆盖、分隔符与超长词。全词典模式会执行约 34.9 万次显式查词探测；成功时 `comparison.json` 的 `passed` 为 `true`。

正常结果会删除较大的临时输出文本，保留输入、日志和摘要；调试时可添加 `--keep-transcripts`。本版删除了旧后端，不能只比较当前库的两个对象来代替上述跨版本检查。

## 8. Bazel 冒烟测试（选做）

安装了 Bazel 或 Bazelisk 后，可在仓库根目录执行；本机验证版本为 Bazel 9.2.0：

```bash
bazel test -c opt //:bazel_smoke_test --cache_test_results=no
```

如果命令名是 `bazelisk`，替换上面的 `bazel` 即可。使用 `-c opt`，避免未优化的 DAT 构建耗时干扰默认构建时间预算。Bazel 当前只注册这一个冒烟目标，不能代替 CMake 的完整单测和差分检查。

## 9. 在自己的程序中使用

本分支需要链接 `cppjieba` 库；不能沿用旧版“只包含头文件即可”的方式。把 cppjieba 作为子目录时：

```cmake
add_subdirectory(cppjieba)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE cppjieba)
```

下面是独立验证程序，可保存为 `build/dat-merged-smoke.cpp`：

```cpp
#include "cppjieba/Jieba.hpp"
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "dict";
  cppjieba::CpuCutOptions options;
  options.optimize_hmm = true;
  cppjieba::Jieba jieba(dir + "/jieba.dict.utf8", dir + "/hmm_model.utf8",
                      dir + "/user.dict.utf8", dir + "/idf.utf8",
                      dir + "/stop_words.utf8", options);
  std::vector<std::string> words;
  jieba.Cut("南京市长江大桥", words);
  for (size_t i = 0; i < words.size(); ++i) {
    if (i) std::cout << '/';
    std::cout << words[i];
  }
  std::cout << '\n';
  return words == std::vector<std::string>{"南京市", "长江大桥"} ? 0 : 1;
}
```

链接刚编译的库：

```bash
c++ -std=c++11 -O3 -fno-fast-math -ffp-contract=off -pthread \
  -Iinclude build/dat-merged-smoke.cpp build/dat-merged-release/libcppjieba.a \
  -o build/dat-merged-smoke
./build/dat-merged-smoke "$PWD/dict"
```

预期输出为 `南京市/长江大桥`。如果直接编译源码，把上面的静态库路径替换为 `src/DatBuilder.cpp`。

可选安装到本地前缀：

```bash
cmake --install build/dat-merged-release --prefix "$PWD/build/dat-merged-install"
```

默认布局包括 `include/cppjieba/`、`lib/libcppjieba.a`、`share/cppjieba/dict/` 和 `lib/cmake/cppjieba/cppjiebaTargets.cmake`；`lib` 可能随平台/`CMAKE_INSTALL_LIBDIR` 变化。安装后请显式传入 `share/cppjieba/dict` 路径，避免依赖头文件位置推导出的默认词典目录。

当前安装导出的是 `cppjiebaTargets.cmake`，没有提供 `cppjiebaConfig.cmake`。已安装库的 CMake 调用方可以直接 `include` 该导出文件，再链接 `cppjieba`；不要假定 `find_package(cppjieba CONFIG)` 已可用。

## 10. 常见问题与本分支约定

| 情况 | 处理方法 |
|---|---|
| 找不到 `dat_merge` 分支 | 使用实际分支名 `dat_merged` |
| GoogleTest 获取失败 | 检查 GitHub 访问；也可准备 `release-1.12.1` 源码，用 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/实际源码根目录` 指定已有目录 |
| 指定 GoogleTest 后配置报目录不存在 | 路径必须包含其 `CMakeLists.txt`；更正路径，或重新配置时加 `-U FETCHCONTENT_SOURCE_DIR_GOOGLETEST` 清除旧缓存项 |
| 链接提示 `undefined reference to DatBuilder::Build` | 链接 `cppjieba` / `libcppjieba.a`，或把 `src/DatBuilder.cpp` 加入编译 |
| 找不到 `Trie.hpp` | 原实现已删除；所需 `DictUnit`/`Dag` 数据类型在 `DictTypes.hpp` |
| `InsertUserWord` / `DeleteUserWord` 返回 false | 所有词典启动后只读；通过构造函数加载用户词典，多路径用 `|` 或 `;` 分隔 |
| `LoadUserDict` / `InserUserDictNode` 抛出 `logic_error` | 初始化后拒绝修改；不要用它们绕过只读契约 |
| 抛出 `DatBuildError` | 查看 `what()` 和 `GetStats().status/reason`；可能是格式、非有限权重或槽位/内存/时间预算超限，没有旧 Trie 回退 |
| Debug / 诊断构建触发时间预算 | 先用 Release 核查；确需更大预算的调用方可设置 `CpuCutOptions::dat_build_options.max_build_ms` |
| 开启 `-ffast-math` 或 `-Ofast` 后编译失败 | 使用严格 FP64；CMake 目标已传播对应选项，手工编译不要覆盖它们 |
| 测试数据路径不正确 | CMake 测试使用配置时生成的路径；`cpu_differential` 和 `cpu_benchmark` 手动运行时把仓库根目录作为首个参数 |
| 测试通过但准备发布版本 | 还需评估冷启动、RSS、吞吐、只读接口及下游链接适配；测试通过不等同于发布条件齐备 |

默认 `CpuCutOptions` 使用 DAT + 融合 DP，HMM 优化默认关闭。开启 HMM 优化后，若稠密表预算不足，仍保留滚动 Viterbi 并使用哈希发射概率；这与 DAT 构建失败抛异常是两种不同情况。

本指南的实测范围是 Linux/WSL。Windows 多配置生成器需给构建指定 `--config Release`、给 CTest 指定 `-C Release`，可执行文件路径也可能多一层 `Release/`；本次没有执行 Windows/ARM64 验证。

## 11. 本次命令核验

2026-09-21 在 Linux/WSL 上实际执行了以下检查，未修改库源码：

- 新构建目录的 Release 配置与编译；GoogleTest 复用了 `release-1.12.1` 本地源码缓存。
- CTest **3/3**，其中完整 GoogleTest **60/60**；第 3 节的核心过滤集合 **30/30**。
- 仅构建静态库、安装、提取本文 C++ 示例并链接运行，输出 `南京市/长江大桥`。
- 传统 benchmark 目标、C/D 可执行程序、Python 多轮包装及 master 基准的命令连通性。
- ASan/UBSan 一致性程序，以及 Bazel `-c opt` 冒烟测试 **1/1**。
- 跨版本脚本的四种场景、六个独立进程配置，完整输出比较通过。

性能包装和跨版本脚本核验采用了缩短轮次/样本（跨版本参数为 `--random-texts 1 --dictionary-probes 32`）；本文提供了正式多轮和全词典的运行命令。命令核验期间还运行了编译任务，因此本次日志不作为新的性能测量结论，性能数据请查看下列已有报告。

相关资料：[当前实现设计](../jieba_cpu_dat_dp_design.md)、[移除 Trie 的对比](dat-only-performance.md)、[与 master 的性能及内存对比](master-comparison.md)。
