# CppJieba

[![CMake](https://github.com/yanyiwu/cppjieba/actions/workflows/cmake.yml/badge.svg)](https://github.com/yanyiwu/cppjieba/actions/workflows/cmake.yml)
[![Author](https://img.shields.io/badge/author-@yanyiwu-blue.svg?style=flat)](http://yanyiwu.com/) 
[![Platform](https://img.shields.io/badge/platform-Linux,macOS,Windows-green.svg?style=flat)](https://github.com/yanyiwu/cppjieba)
[![Performance](https://img.shields.io/badge/performance-excellent-brightgreen.svg?style=flat)](http://yanyiwu.com/work/2015/06/14/jieba-series-performance-test.html) 
[![Tag](https://img.shields.io/github/v/tag/yanyiwu/cppjieba.svg)](https://github.com/yanyiwu/cppjieba/releases)

## 简介

CppJieba是"结巴(Jieba)"中文分词的C++版本

### 主要特点

- 🚀 高性能：经过线上环境验证的稳定性和性能表现
- 📦 易集成：C++11 接口位于 `include/cppjieba/`，链接 `cppjieba` 构建器库即可使用
- 🔍 多种分词模式：支持精确模式、全模式、搜索引擎模式等
- 📚 自定义词典：支持启动时加载用户自定义词典，支持多词典路径（使用'|'或';'分隔）
- 💻 跨平台：支持 Linux、macOS、Windows 操作系统
- 🌈 UTF-8编码：原生支持 UTF-8 编码的中文处理

## 快速开始

### 环境要求

- C++ 编译器：
  - 支持 C++11 的 g++
  - 或 clang++
- cmake 3.14 以上（测试依赖使用 FetchContent）

### 安装步骤

```sh
git clone https://github.com/yanyiwu/cppjieba.git
cd cppjieba
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make

make test
```

### CPU 优化模式

默认构造使用位图 raw DAT 和融合反向 DP，原 Pointer Trie 已删除。以下选项另外启用 HMM 稠密发射表和滚动分数优化：

```cpp
#include "cppjieba/Jieba.hpp"

cppjieba::CpuCutOptions options;
options.mode = cppjieba::CpuCutMode::DatRawFused;
options.optimize_hmm = true;
cppjieba::Jieba jieba("dict/jieba.dict.utf8", "dict/hmm_model.utf8",
                     "dict/user.dict.utf8", "dict/idf.utf8",
                     "dict/stop_words.utf8", options);
```

CMake 调用方使用 `target_link_libraries(app PRIVATE cppjieba)`；直接编译使用：

```sh
c++ -std=c++11 -O3 -fno-fast-math -ffp-contract=off -pthread -Iinclude \
    app.cpp src/DatBuilder.cpp -o app
```

初始化路径需要链接构建器。所有词典完成启动加载后冻结：`InsertUserWord`/`DeleteUserWord` 返回 false，公开 `LoadUserDict`/`InserUserDictNode` 抛出 `std::logic_error`。请通过构造参数的用户词典路径加载自定义词，多个文件使用 `|` 或 `;` 分隔。DAT 超过资源或格式预算时构造函数抛出 `DatBuildError`，其 `GetStats()` 提供失败类别和原因；不会回退到已删除的旧后端。构造成功后可用 `GetDatBuildStats()` 查看模型规模和耗时。HMM 优化选项要求模型在构造后不再修改，包括保留的公开概率字段。

`Find`、词性、搜索和全模式均使用 DAT。终点的独立词条索引保留 `DictUnit` 的词和词性；头文件 `Trie.hpp` 已删除，需要这些数据类型的调用方使用 `DictTypes.hpp`。`LegacyDag`、`PointerFused` 选项也已移除。超长词使用 `size_t` 词长回溯，不再依赖旧 DAG 内核。删除旧 Trie 前后的内存与性能比较见 [DAT 独立模型报告](docs/dat-only-performance.md)，此前的优化测量保留在 [初版性能报告](docs/cpu-performance.md)。

### Benchmark

项目提供了一个用于本地性能对比的 benchmark 目标：

```sh
make benchmark
```

它会构建并运行 `test/benchmark.cpp`，输出以下指标：

- `DictTrieLoad`: 词典加载耗时，以及可用时的进程 RSS 内存占用
- `HMMModelLoad`: HMM 模型加载耗时，以及可用时的进程 RSS 内存占用
- `MPCut`: `MPSegment` 对基准文本的分词吞吐
- `MixCut`: `MixSegment` 对基准文本的分词吞吐
- `DictFind`: 词典查找吞吐

这个 benchmark 主要用于本地修改前后的性能回归和对比，不作为默认测试的一部分。

## 使用示例

```
./demo
```

结果示例：

```
[demo] Cut With HMM
他/来到/了/网易/杭研/大厦
[demo] Cut Without HMM
他/来到/了/网易/杭/研/大厦
我来到北京清华大学
[demo] CutAll
我/来到/北京/清华/清华大学/华大/大学
小明硕士毕业于中国科学院计算所，后在日本京都大学深造
[demo] CutForSearch
小明/硕士/毕业/于/中国/科学/学院/科学院/中国科学院/计算/计算所/，/后/在/日本/京都/大学/日本京都大学/深造
[demo] Startup User Dictionary
男默女泪
[demo] CutForSearch Word With Offset
[{"word": "小明", "offset": 0}, {"word": "硕士", "offset": 6}, {"word": "毕业", "offset": 12}, {"word": "于", "offset": 18}, {"word": "中国", "offset": 21}, {"word": "科学", "offset": 27}, {"word": "学院", "offset": 30}, {"word": "科学院", "offset": 27}, {"word": "中国科学院", "offset": 21}, {"word": "计算", "offset": 36}, {"word": "计算所", "offset": 36}, {"word": "，", "offset": 45}, {"word": "后", "offset": 48}, {"word": "在", "offset": 51}, {"word": "日本", "offset": 54}, {"word": "京都", "offset": 60}, {"word": "大学", "offset": 66}, {"word": "日本京都大学", "offset": 54}, {"word": "深造", "offset": 72}]
[demo] Tagging
我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。
[我:r, 是:v, 拖拉机:n, 学院:n, 手扶拖拉机:n, 专业:n, 的:uj, 。:x, 不用:v, 多久:m, ，:x, 我:r, 就:d, 会:v, 升职:v, 加薪:nr, ，:x, 当上:t, CEO:eng, ，:x, 走上:v, 人生:n, 巅峰:n, 。:x]
[demo] Keyword Extraction
我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。
[{"word": "CEO", "offset": [93], "weight": 11.7392}, {"word": "升职", "offset": [72], "weight": 10.8562}, {"word": "加薪", "offset": [78], "weight": 10.6426}, {"word": "手扶拖拉机", "offset": [21], "weight": 10.0089}, {"word": "巅峰", "offset": [111], "weight": 9.49396}]
```

For more details, please see [demo](https://github.com/yanyiwu/cppjieba-demo).

### 分词结果示例

**MPSegment**

Output:
```
我来到北京清华大学
我/来到/北京/清华大学

他来到了网易杭研大厦
他/来到/了/网易/杭/研/大厦

小明硕士毕业于中国科学院计算所，后在日本京都大学深造
小/明/硕士/毕业/于/中国科学院/计算所/，/后/在/日本京都大学/深造

```

**HMMSegment**

```
我来到北京清华大学
我来/到/北京/清华大学

他来到了网易杭研大厦
他来/到/了/网易/杭/研大厦

小明硕士毕业于中国科学院计算所，后在日本京都大学深造
小明/硕士/毕业于/中国/科学院/计算所/，/后/在/日/本/京/都/大/学/深/造

```

**MixSegment**

```
我来到北京清华大学
我/来到/北京/清华大学

他来到了网易杭研大厦
他/来到/了/网易/杭研/大厦

小明硕士毕业于中国科学院计算所，后在日本京都大学深造
小明/硕士/毕业/于/中国科学院/计算所/，/后/在/日本京都大学/深造

```

**FullSegment**

```
我来到北京清华大学
我/来到/北京/清华/清华大学/华大/大学

他来到了网易杭研大厦
他/来到/了/网易/杭/研/大厦

小明硕士毕业于中国科学院计算所，后在日本京都大学深造
小/明/硕士/毕业/于/中国/中国科学院/科学/科学院/学院/计算/计算所/，/后/在/日本/日本京都大学/京都/京都大学/大学/深造

```

**QuerySegment**

```
我来到北京清华大学
我/来到/北京/清华/清华大学/华大/大学

他来到了网易杭研大厦
他/来到/了/网易/杭研/大厦

小明硕士毕业于中国科学院计算所，后在日本京都大学深造
小明/硕士/毕业/于/中国/中国科学院/科学/科学院/学院/计算所/，/后/在/中国/中国科学院/科学/科学院/学院/日本/日本京都大学/京都/京都大学/大学/深造

```

以上依次是MP,HMM,Mix三种方法的效果。  

可以看出效果最好的是Mix，也就是融合MP和HMM的切词算法。即可以准确切出词典已有的词，又可以切出像"杭研"这样的未登录词。

Full方法切出所有字典里的词语。

Query方法先使用Mix方法切词，对于切出来的较长的词再使用Full方法。

### 自定义用户词典

自定义词典示例请看`dict/user.dict.utf8`。

用户词典支持以下三种格式，每行一条，列之间用空格分隔:

```text
词语
词语 词性
词语 词频 词性
```

- `1` 列: 只提供词语，词频使用默认值，词性为空。
- `2` 列: 当前解释为 `词语 词性`，词频仍使用默认值。
- `3` 列: 解释为 `词语 词频 词性`。
- 多个用户词典文件可以用 `|` 或 `;` 连接后一起传入。
- 不支持行内注释或额外的列。

主词典 `dict/jieba.dict.utf8` 的格式固定为三列:

```text
词语 词频 词性
```

其中:

- `词频` 在加载时会被转换成对数权重，供概率分词使用。
- 用户词典如果未显式提供词频，会使用主词典统计出来的默认权重；默认策略是中位数权重。
- `词性` 在代码中只是原样保存的字符串，没有内置枚举校验；主词典当前实际使用了 `55` 种 tag，例如 `n`、`v`、`nr`、`ns`、`nz`、`x`、`eng` 等。

没有使用自定义用户词典时的结果:

```
令狐冲/是/云/计算/行业/的/专家
```

使用自定义用户词典时的结果:

```
令狐冲/是/云计算/行业/的/专家
```

### 关键词抽取

```
我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。
["CEO:11.7392", "升职:10.8562", "加薪:10.6426", "手扶拖拉机:10.0089", "巅峰:9.49396"]
```

For more details, please see [demo](https://github.com/yanyiwu/cppjieba-demo).

### 词性标注

```
我是蓝翔技工拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上总经理，出任CEO，迎娶白富美，走上人生巅峰。
["我:r", "是:v", "拖拉机:n", "学院:n", "手扶拖拉机:n", "专业:n", "的:uj", "。:x", "不用:v", "多久:m", "，:x", "我:r", "就:d", "会:v", "升职:v", "加薪:nr", "，:x", "当上:t", "CEO:eng", "，:x", "走上:v", "人生:n", "巅峰:n", "。:x"]
```

For more details, please see [demo](https://github.com/yanyiwu/cppjieba-demo).

支持自定义词性。
比如在(`dict/user.dict.utf8`)增加一行

```
蓝翔 nz
```

结果如下：

```
["我:r", "是:v", "蓝翔:nz", "技工:n", "拖拉机:n", "学院:n", "手扶拖拉机:n", "专业:n", "的:uj", "。:x", "不用:v", "多久:m", "，:x", "我:r", "就:d", "会:v", "升职:v", "加薪:nr", "，:x", "当:t", "上:f", "总经理:n", "，:x", "出任:v", "CEO:eng", "，:x", "迎娶:v", "白富美:x", "，:x", "走上:v", "人生:n", "巅峰:n", "。:x"]
```

## 其它词典资料分享

+ [dict.367W.utf8] iLife(562193561 at qq.com)

## 生态系统

CppJieba 已经被广泛应用于各种编程语言的分词实现中：

- [GoJieba](https://github.com/yanyiwu/gojieba) - Go 语言版本
- [NodeJieba](https://github.com/yanyiwu/nodejieba) - Node.js 版本
- [CJieba](https://github.com/yanyiwu/cjieba) - C 语言版本
- [jiebaR](https://github.com/qinwf/jiebaR) - R 语言版本
- [exjieba](https://github.com/falood/exjieba) - Erlang 版本
- [jieba_rb](https://github.com/altkatz/jieba_rb) - Ruby 版本
- [iosjieba](https://github.com/yanyiwu/iosjieba) - iOS 版本
- [phpjieba](https://github.com/jonnywang/phpjieba) - PHP 版本
- [perl5-jieba](https://metacpan.org/pod/distribution/Lingua-ZH-Jieba/lib/Lingua/ZH/Jieba.pod) - Perl 版本

### 应用项目

- [simhash](https://github.com/yanyiwu/simhash) - 中文文档相似度计算
- [pg_jieba](https://github.com/jaiminpan/pg_jieba) - PostgreSQL 分词插件
- [gitbook-plugin-search-pro](https://plugins.gitbook.com/plugin/search-pro) - Gitbook 中文搜索插件
- [ngx_http_cppjieba_module](https://github.com/yanyiwu/ngx_http_cppjieba_module) - Nginx 分词插件
- [OpenCC](https://github.com/byvoid/OpenCC) - OpenCC 中的 jieba 分词插件

## 贡献指南

我们欢迎各种形式的贡献，包括但不限于：

- 提交问题和建议
- 改进文档
- 提交代码修复
- 添加新功能


如果您觉得 CppJieba 对您有帮助，欢迎 star ⭐️ 支持项目！
