# Jieba CPU 分词优化实现设计：位图 raw DAT 与融合反向 DP

> **2026-09-20 更新：** 已按用户要求删除原 Pointer Trie，并统一为启动后只读词典。当前默认后端、构建失败处理和超长词行为以第 17 节为准。第 1～16 节记录初版设计及提交 `8b8a3f7` 的实现，涉及保留旧 Trie、Legacy 默认或回退的描述仅适用于该历史版本。

本文给出 Jieba 的 CPU 软件优化方案，以当前 cppjieba 的 C++ 实现为行为基线：用 Double-Array Trie（DAT，双数组字典树）查询候选词，候选一旦命中就参与反向动态规划（DP），以环形数组保存分数，以最佳词长数组恢复分词边界，然后继续现有的隐马尔可夫模型（HMM）分词。

文档状态：第一阶段和第 9 节 HMM 优化已在本仓库实现。下文保留设计推导和算法说明，实际接口及验证结果见第 16 节。现状依据为 2026-09-18 工作区中的 cppjieba 源码；布局数字来自 `dat_optimization/02_bitmap_raw` 的归档模型。

## 1. 目标、术语与实施范围

### 1.1 数据和算法术语

| 名称 | 本文含义 |
|---|---|
| Rune | `uint32_t` 字符值，沿用现有 `DecodeRunesInString` 的解码结果 |
| `RuneStr` / `WordRange` | 前者保存 Rune 及原字符串坐标，后者用一对包含式 Rune 迭代器表示输出词 |
| range | `PreFilter` 划出的独立字符区间，以 `[begin, end)` 表示 |
| Pointer Trie | 现有字典树，每个节点通过 `unordered_map<Rune, TrieNode*>` 查找孩子 |
| 候选词 | 从某个位置出发的词典命中；每个位置还必须有长度为 1 的兜底候选 |
| DAG | 有向无环图，位置是节点、候选词是边；当前用 `vector<Dag>` 保存这些候选 |
| DP | 比较“当前候选权重 + 最优后缀分数”，选择整个 range 的最优切分 |
| DAT state | DAT 数组的实际下标，不是构建期的逻辑节点编号 |
| label / raw | label 是转移标签；raw 表示直接使用原始 Rune 值作为标签 |
| `base/check` | DAT 的寻址偏移和父状态校验字段，转移满足 `t = base[s] + label` 且 `check[t] == s` |
| `weightCode` | 完整词的权重索引；非终点使用专门的哨兵值 |
| `dpRing` / `bestLen` | 前者循环保存仍会被后续计算读取的 DP 分数，后者保存每个起点选中的词长 |
| FP64 | IEEE 754 的 64 位浮点数，对应目标平台上的 `double` |
| scratch | 一次调用独占、可在多个 range 之间复用的临时工作区 |

后文用 `n` 表示一个 range 的 Rune 数，用 `N` 表示词典逻辑节点数，用 `M` 表示 DAT 槽位数。`L` 是本次调用实际允许的最长候选词长，不能与 `n` 或模型的完整最长词长混用。

本文的 DAT 仅采用位图 raw 方案：构建时用空槽位图搜索最低可行地址，查询时直接计算 `base + Rune`。位图用于模型构建，不进入分词运行路径。

### 1.2 第一版交付内容

第一版完成以下闭环：

1. 从已经加载的 `DictTrie` 构建只读 DAT，权重直接复制现有 C++ `double` 的位模式。
2. 在 CPU 上实现融合的候选枚举与反向 DP，不构造完整 `vector<Dag>`。
3. 使用 `dpRing[L+1]` 和 `bestLen[n]`，输出既有 `WordRange`。
4. 保持 `MixSegment` 的单字分流以及现有 HMM 算法。
5. 让 `Cut`、`CutForSearch`、`CutSmall` 和 `KeywordExtractor` 正确使用新的主词典路径。
6. 保留旧实现用于兼容和差分验证，提供构建失败及不支持输入的回退路径。

软件内部保留原始字符串与 Rune 迭代器，直接产生既有的 `WordRange` 输出。

HMM 的滚动分数数组和连续发射概率表作为第二阶段，见第 9 节。运行时动态修改词典和释放旧词典结构作为后续阶段，见第 10 节、第 14 节。

### 1.3 第一版的词典生命周期

`Jieba` 对象在构造时加载主词典和启动用户词典，并由多个分词组件共享 `DictTrie`。本设计首先优化初始化后只读、可重复用于分词的词典对象。

新增优化模式明确采用初始化后冻结词典的契约。独立 cppjieba 调用者若需要运行时修改词典，继续使用默认旧模式。冻结仅限制词典修改，不取消启动用户词典，也不限制只读的 `Find`、词性查询或 `CutAll`。

冻结是显式选择优化模式后的契约，现有默认构造调用仍保持原有接口行为。优化模式的修改接口拒绝规则在第 10 节给出。

## 2. 当前调用链与改动边界

### 2.1 当前执行过程

```text
Jieba::Cut ───────────────────────────────────┐
Jieba::CutForSearch → QuerySegment::Cut ───────┤
KeywordExtractor::Extract ────────────────────┤
                                             v
                                     MixSegment::Cut
                                             |
                                             v
                                     PreFilter ranges
                                             |
                                             v
                                     MPSegment::Cut
                                       Trie::Find
                                       CalcDP
                                       CutByDag
                                             |
                                             v
                                MixSegment 连续普通单字分流
                                             |
                                             v
                                HMMSegment 的 ASCII / Viterbi
```

Viterbi 是 HMM 在状态序列上寻找最高分路径的动态规划。现有状态编号为 `B=0, E=1, M=2, S=3`，分别表示词首、词尾、词中、单字。

`QuerySegment` 在混合分词之后，还按原顺序添加已登录的二元词、三元词，再添加原词。`KeywordExtractor` 在混合分词之后执行停用词过滤、词频和逆文档频率加权、排序。新的主词典路径不能绕开这些后处理。

### 2.2 新执行过程

```text
PreFilter range
  → 从右向左选择起点 i
  → 从 i 向右查询 DAT，按词长递增产生候选
  → 候选即时打分，更新当前位置的最佳分数和词长
  → 保存 dpRing[i % (L+1)]、bestLen[i]
  → 从左向右按 bestLen 跳转，生成 WordRange
  → 原 MixSegment / HMM
  → 原搜索扩展或关键词处理
```

DAT 优化模型访问，融合 DP 优化请求临时状态。二者可以独立启用和测量，不把两个优化的收益混成一个无法归因的数字。

### 2.3 兼容路径

第一版保留 `DictTrie::trie_`、`static_node_infos_`、`active_node_infos_` 和用户单字集合。新增 DAT 是同一份启动词典的只读查询副本。

| 调用 | 第一版行为 |
|---|---|
| `MPSegment::Cut` | 优化模式下使用融合内核，其他情况使用旧 DAG 内核 |
| `MixSegment::Cut` | 使用新的主词典结果，单字分流和 HMM 保持原行为 |
| `QuerySegment::Cut` | 主词典部分间接加速；二元、三元扩展仍调用原 `DictTrie::Find` |
| `KeywordExtractor::Extract` | 经其共享的 `MixSegment` 使用优化，评分和输出不变 |
| `DictTrie::Find(begin,end)`、`Jieba::Find` | 保留原返回类型及 Pointer Trie 查词 |
| `FullSegment::Cut` / `CutAll` | 保留原 DAG 枚举和输出顺序 |
| `Tag` / `LookupTag` | 分词可使用优化，词性查找仍从原 `DictUnit` 取 `tag` |
| `CutHMM` | 第一版保持原实现 |

因此，第一版不能释放 `DictUnit` 或宣称已经移除了 CPU Pointer Trie 的模型内存。它首先降低每次主词典切分的分配、候选写入及回读开销。

## 3. 必须保持的语义

### 3.1 主词典查询

- 每个起点始终产生长度 1 的候选。即使字符没有匹配到 DAT 节点，或者匹配节点不是完整词，也使用 `DictTrie::GetMinWeight()` 作为候选权重。
- 长度大于 1 时，只有完整词终点才产生候选；仅前缀节点继续转移但不打分。
- 候选顺序与现有 `Trie::Find()` 相同：长度 1、2、3……。
- 重复词按原加载顺序由后加载者覆盖，不能在排序时丢失覆盖顺序。
- 用户单字保护集合沿用现有启动加载结果。它不等于“所有词典单字”，也不从 DAT 终点集合推导。

### 3.2 词长上限

现有 `MAX_WORD_LENGTH=512` 是默认参数，`Trie::Find()` 实际按调用者传入的 `max_word_len` 比较。`Jieba::CutSmall()` 允许调用者传入其他 `size_t` 值。

模型记录全部生效词的真实最大长度 `actualMaxWordLen`，不预先截断到 512。对于非空 range：

```cpp
size_t L = std::max(size_t(1),
    std::min(n, std::min(actualMaxWordLen, requestedMaxWordLen)));
```

其中 `requestedMaxWordLen` 是本次调用的参数。普通 `Cut` 默认传 512；`CutSmall` 原样转发参数。

即使参数为 0，现有实现也会产生单字候选，因此 `L` 下限必须是 1。若 `L > UINT16_MAX`，第一版在写出任何结果前使用旧内核，不把词长截断成 16 位。词典包含超长词本身不必禁用其他短上限调用。

### 3.3 浮点、比较和回溯

`MIN_DOUBLE` 是源码中的有限值 `-3.14e100`，不是负无穷。`minWeight` 是主词典最小词权重，两个值作用不同。

每个位置初始 `best=MIN_DOUBLE`、`chosenLen=1`；每个候选执行与旧代码相同的计算：

```cpp
double score = 0.0;
if (endExclusive < n) {
  score += futureScore;
}
score += candidateWeight;
if (score > best) {
  best = score;
  chosenLen = candidateLength;
}
```

保持严格 `>`，同分时保留先枚举的短词。不要把初始化改成“无条件接受首个候选”，这样才能保留极小有限权重下旧实现仍留在 `MIN_DOUBLE` 的行为。主词典未选中任何候选时，原 `pInfo=NULL` 对应输出单字，新的 `chosenLen=1` 与之相同。

权重、分数均使用 `double`，复制权重使用 `memcpy` 读取位模式。不要重新计算 `log`、按 epsilon 去重、改成 FP32 或改变加法结合顺序。优化目标文件不能启用允许浮点重结合的 fast-math；也要检查项目已有编译选项，避免局部设置被覆盖。

### 3.4 range 和输出

保留现有解码和 `PreFilter` 行为，包括分隔符形成独立 range 的规则。当前解码器不是完整的 Unicode 合法性校验器，本优化不顺带收紧输入校验。

软件输出继续使用包含式区间 `WordRange(left, right)`。内部候选的 `endExclusive=i+len` 是不包含式结束位置，转换时使用 `begin+i+len-1`。

迭代器重载向输出容器追加；字符串重载保持现有的清空和转换规则。`WordRange` 始终引用原 `RuneStrArray`，不能引用生命周期更短的临时数组。

## 4. 接口与对象所有权

### 4.1 模式选择

新增头文件 `CpuCutOptions.hpp`，声明以下拟用类型：

```cpp
enum class CpuCutMode {
  LegacyDag,          // 当前 Pointer Trie + DAG + CalcDP
  PointerFused,       // Pointer Trie + 融合 DP，仅用于开发验证
  DatRawFused         // 位图 raw DAT + 融合 DP
};

struct CpuCutOptions {
  CpuCutMode mode;
  CpuCutOptions() : mode(CpuCutMode::LegacyDag) {}
};
```

把选项作为 `Jieba` 和自建词典的 `DictTrie` 构造函数末尾可选参数，原构造调用保持有效。共享词典的各 `Segment` 从 `DictTrie` 取得生效模式，不能各自选择不同词典快照。

所有非 `LegacyDag` 模式均使用第 1.3 节的冻结契约。该契约取决于请求模式，不因 DAT 构建失败而取消。

调用者通过 `Jieba` 或 `DictTrie` 的构造选项选择模式，同一对象的各分词入口保持一致。`PointerFused` 仅用于分离融合 DP 的收益和排查差异；DAT 实现只有 `DatRawFused`。默认切换必须经过第 13 节验收，初始兼容默认值仍为 `LegacyDag`。

### 4.2 模型与游标

一个 Unit 表示一个 DAT 槽位，存储为 64 位整数，具体字段见第 5 节。`DatModel` 负责只读数组；`DatCursor` 保存一次匹配当前位置。游标缓存当前 Unit，下一轮无需重新加载当前槽位。

```cpp
struct DatCursor {
  uint32_t state;
  uint64_t unit;
};

class DatModel {
 public:
  DatCursor Root() const;
  bool StepRaw(Rune rune, DatCursor& cursor) const;
  bool IsTerminal(const DatCursor& cursor) const;
  double TerminalWeight(const DatCursor& cursor) const;
  size_t ActualMaxWordLen() const;
  double UnknownWeight() const;

 private:
  friend class DatBuilder;
  std::vector<uint64_t> units_;
  std::vector<double> uniqueWeights_;
  double unknownWeight_;
  size_t actualMaxWordLen_;
};
```

`DictTrie` 持有 `std::unique_ptr<const DatModel>`。构建器先在局部对象内完成构建与校验，成功后一次性移交所有权，不能发布半成品。`TerminalWeight()` 的前置条件是 `IsTerminal()==true`。

第一版用户单字查询继续使用 `DictTrie::IsUserDictSingleChineseWord()`，不在 CPU DAT 中重复存储一份标记。归档中的自包含模型则需要此标记，这是两种内存口径的区别。

### 4.3 工作区

```cpp
struct MPCutScratch {
  std::vector<double> dpRing;
  std::vector<uint16_t> bestLen;
};
```

工作区由一次最外层分词调用持有，同一句的多个 range 顺序复用容量。禁止把它放入共享 `Jieba`、`DictTrie` 或 `Segment` 的 `mutable` 成员。

新增私有或内部 `CutWithScratch(..., MPCutScratch&)` 重载，供 `MPSegment`、`MixSegment`、`QuerySegment` 逐层传递。现有公开的迭代器重载自行创建局部工作区，然后调用内部重载；现有字符串重载在 range 循环外创建一次工作区。`KeywordExtractor` 通过 `MixSegment` 的字符串入口自然复用。

第一版不使用线程局部静态工作区，避免同线程嵌套调用覆盖状态和超大请求长期保留容量。若以后增加线程缓存，必须另行定义重入、容量上限和回收策略。

## 5. DAT 表示与查询

### 5.1 槽位布局

采用归档已验证的 64 位 Unit，用显式掩码和位移处理：

| 位区间 | 字段 | 解释 |
|---|---|---|
| 0～21 | `base` | 22 位有符号偏移 |
| 22～42 | `check` | 21 位父状态下标，`2097151` 表示空槽 |
| 43～55 | `weightCode` | 13 位权重索引，`8191` 表示非终点 |
| 56～63 | 保留位 | 必须为 0 |

根占用槽位 0，`check=0`、非终点。普通转移必须满足 `0<t<M`。空槽统一设 `base=0, check=NO_CHECK, weightCode=NO_WEIGHT`，不能全部清零，否则从 root 查询可能把空槽误判为合法孩子。

叶节点 `base=0`。终点和是否有孩子相互独立，不用 `base` 的正负区分终点。

```cpp
constexpr uint32_t BASE_MASK = (1U << 22) - 1;
constexpr uint32_t NO_CHECK = (1U << 21) - 1;
constexpr uint32_t NO_WEIGHT = (1U << 13) - 1;

inline int32_t DecodeBase(uint64_t unit) {
  uint32_t raw = static_cast<uint32_t>(unit & BASE_MASK);
  return (raw & (1U << 21))
      ? static_cast<int32_t>(raw) - (1 << 22)
      : static_cast<int32_t>(raw);
}

inline uint32_t DecodeCheck(uint64_t unit) {
  return static_cast<uint32_t>((unit >> 22) & NO_CHECK);
}

inline uint32_t DecodeWeightCode(uint64_t unit) {
  return static_cast<uint32_t>((unit >> 43) & NO_WEIGHT);
}
```

模型成立条件为 `M<=NO_CHECK`、所有真实父下标 `<NO_CHECK`、`-2^21<=base<2^21`，以及唯一权重数 `U<=NO_WEIGHT`。最后一个合法权重下标是 `U-1`，必须小于哨兵。超限时第一版不压缩截断，直接保留旧后端；宽格式可独立扩展。

### 5.2 一次转移

```cpp
bool StepLabel(uint32_t label, DatCursor& cursor) const {
  int64_t t = static_cast<int64_t>(DecodeBase(cursor.unit))
            + static_cast<int64_t>(label);
  if (t <= 0 || static_cast<uint64_t>(t) >= units_.size()) {
    return false;
  }
  uint64_t next = units_[static_cast<size_t>(t)];
  if (DecodeCheck(next) != cursor.state) {
    return false;
  }
  cursor.state = static_cast<uint32_t>(t);
  cursor.unit = next;
  return true;
}
```

该函数是 `DatModel` 的内部辅助函数，输入游标必须来自本模型。失败不改变游标，调用方立即结束当前起点的匹配。`StepRaw` 直接把原始 Rune 作为 label 传入。

一次成功转移新增读取一个 64 位目标 Unit；终点再读取 `uniqueWeights`。这描述的是逻辑加载，不能解释为“一次访问一定命中缓存”或“访问地址连续”。DAT 仍有沿前缀串行依赖的非连续访问。

## 6. C++ 构建器

### 6.1 接入时机与输入

保留 `DictTrie::Init()` 当前顺序：

```text
LoadDict
  → CalcFreqSum / CalculateWeight
  → SetStaticWordWeights
  → 加载启动用户词典
  → Shrink(static_node_infos_)
  → CreateTrie（保留原结构）
  → 尝试构建 DAT / 记录实际后端
  → 冻结并发布对象
```

新增 `include/cppjieba/DatBuilder.hpp` 和实现文件 `src/DatBuilder.cpp`。构建器输入为稳定的 `static_node_infos_` 和原 `min_weight_`，边标签固定使用原始 Rune。第一版直接在 C++ 初始化阶段构建，不依赖 Python、不读取归档的 Python 权重、不引入运行时模型文件格式。

先保存 `{const Unicode* word, size_t sourceIndex}`，不复制每个词的 Rune 数组。按原始 Rune 序列排序，同词保留 `sourceIndex` 最大的记录。终点权重来自该记录的 `DictUnit::weight`。空词沿用旧 Trie 不插入的行为，不能把 root 变成完整词。

扫描生效词得到 `actualMaxWordLen`。从原 `GetMinWeight()` 复制 `unknownWeight`，不能对“覆盖后的生效词”重新求最小值，因为旧值是在加载启动用户词典前计算的。

按 FP64 的 64 位位模式排序去重权重，再为每个终点记录索引。对非有限权重，优化模型构建返回“不支持”，保留旧路径，不擅自修改权重。目标平台须满足 `sizeof(double)==8` 且支持所需 IEEE 浮点表示。

### 6.2 逻辑 Trie 与物理槽位分离

逻辑 Trie 只表示父子关系，不包含最终 DAT 下标。建议构建中使用以下结构：

```cpp
struct LogicalNode {
  uint32_t firstEdge;
  uint32_t edgeCount;
  uint32_t weightCode;
};

struct LogicalEdge {
  uint32_t rune;
  uint32_t child;
};
```

构建步骤：

1. 依次处理按原始 Rune 排序的生效词。
2. 维护前一词的前缀节点栈，计算相邻词最长公共前缀，栈保留到公共前缀。
3. 为剩余字符依次新建节点，暂存 `{parent, rune, child}` 边记录。
4. 为完整词节点写入 `weightCode`。
5. 按父节点计数并计算偏移，把边记录整理到连续 `LogicalEdge` 数组。

采用显式栈，避免长用户词造成递归栈过深。节点编号与“按原始 Rune 排序词表逐词插入”一致。

每个父节点的子边按原始 Rune 升序排列。逻辑节点编号在后续布局中保持不变，用作排序的最后一个决胜条件，使构建结果可重复。

收集非叶节点，按以下键放置逻辑行：

```text
直接孩子数降序
原始 Rune 跨度 max(Rune)-min(Rune) 降序
逻辑节点编号升序
```

一行就是某个父节点的全部直接子边。数组 `logicalBase[u]` 保存该行的偏移，`logicalState[u]` 保存逻辑节点最后落到的物理槽位。初始化只有 `logicalState[root]=0`。

### 6.3 位图与最低地址优先

构建期使用空槽位图 `F`，`F[q]=1` 表示槽位 q 空闲；0 号位固定为 0。位图不进入运行模型。

设当前行标签升序为 `c[0..k)`，令 `d[j]=c[j]-c[0]`。一个锚点 q 可放置整行，当且仅当所有 `F[q+d[j]]` 均为 1。数学表示为：

```text
V = (F >> d[0]) & (F >> d[1]) & ... & (F >> d[k-1])
```

选择 V 的最低有效位 q，得到 `base=q-c[0]`。例如 `d={0,3,8}`，需要 q、q+3、q+8 三个槽同时空闲。

Python 归档用任意精度整数实现整体位运算。C++ 使用 `vector<uint64_t>`，按 64 个候选锚点一组扫描：

```text
FindBase(labels):
    确保 capacity >= span + 2，root 已占用
    loop:
        for q0 = 0, 64, 128, ...，且 q0 < capacity:
            mask = 64 个 1
            对所有偏移 d 求交，可先处理最大 d:
                mask &= Read64Bits(F, q0 + d)
                若 mask == 0，结束这一组
            若 mask != 0:
                q = q0 + CountTrailingZeros(mask)
                return int64(q) - minLabel
        按 65,536 个槽扩容，再从最低地址搜索
```

`Read64Bits(F,p)` 返回从位 p 开始的 64 位空闲状态，超出已分配容量的位为 0。实现必须处理跨机器字边界：

```cpp
// LoadWord(i) 在 i 超过位图长度时返回 0。
size_t word = p / 64;
unsigned shift = static_cast<unsigned>(p % 64);
uint64_t bits = LoadWord(word) >> shift;
if (shift != 0) {
  bits |= LoadWord(word + 1) << (64 - shift);
}
```

最后一个机器字的容量外位也必须为 0。对 0 不调用尾零计数；不执行移位 64 位；地址、扩容和字节数计算先检查整数溢出。

只有当前容量内不存在整行位置才扩容。构建容量可按 65,536 槽增长，但不能超过运行格式允许的槽位上限；最后一块允许不足 65,536。超过预算返回失败。

一次选中 q 后，检查整行目标槽位并全部占用，记录：

```text
logicalBase[parent] = q - minLabel
logicalState[child] = logicalBase[parent] + label
```

此时不需要知道父节点本身最终的物理位置。孩子只有一个父节点，各行的目标槽位不得重复，root 位置不得被占用。

### 6.4 物化、打包和发布

全部行放置后，令 `M=max(logicalState)+1`。叶节点的 `logicalBase` 取 0。初始化 M 个空 Unit，再执行：

```text
对每个逻辑节点 u:
    s = logicalState[u]
    写入 base[s] = logicalBase[u]
    写入 weightCode[s] = logicalWeightCode[u]

对每条 u → v:
    写入 check[logicalState[v]] = logicalState[u]

写入 root 的 check=0
校验所有宽字段的范围
按位打包为 uint64_t
```

至少完成以下发布前验证：

- 每个逻辑节点对应唯一非空槽位；每条逻辑边可以从父节点正确转移。
- 每个生效词可完整查询，终点权重与原 `DictUnit::weight` 位级相同。
- 非终点、空槽、root 和高 8 位保留位符合格式。
- 解包结果与构建期宽字段一致；所有权重索引有效。

第一版保留全词典验证，记录其初始化耗时。可在后续测量后区分必要结构检查和调试差分检查，不能先删掉验证再解释错误输出。

构建函数以结构化状态返回成功、字段超限、资源预算不足或验证失败。资源预算包括槽位数、临时内存和构建时间，由构建选项传入；实验程序允许调整，正式默认值需随初始化基准一起确定。时间预算在行边界及大行的分块扫描间检查，只用于放弃优化，不发布不同语义的模型。构建临时分配失败在构建边界内转换为资源不足状态，使用自动管理所有权的局部对象保证原 Trie 不受影响。

构建失败仅丢弃局部 DAT，原词典继续可用。记录请求模式、实际后端、失败原因和耗时，避免性能测试把回退误算成优化结果。

## 7. 融合主词典内核

### 7.1 Walker 抽象

为使融合 DP 可以独立于 DAT 验证，定义编译期的 Walker 接口。Walker 表示从某个起点逐字向右匹配的游标：

```cpp
void Reset();                  // 回到 root
bool Step(size_t runeOffset);  // 消费 range 内一个位置，失败不再扩展
bool IsTerminal() const;
double TerminalWeight() const;
```

提供 `RawDatWalker`，直接读取原 `RuneStrArray` 中的 Rune。开发验证另外提供 `PointerWalker`，经拟新增的只读根节点访问器取得 `const TrieNode*`，不暴露可写节点。

融合函数用模板实例化 Walker，在 range 入口按模式分派一次。不要在每个字符上调用虚函数或 `std::function`，也不要为每个候选构造容器。

### 7.2 输入准备与状态

逻辑上的 `dp[i]` 表示从位置 i 到 range 末尾的最高累计分数，`dp[n]=0`。正常词权重下的递推为 `dp[i]=max(w(i,len)+dp[i+len])`，其中 len 遍历当前位置的全部合法候选。实现仍保留第 3.3 节的有限初值与严格比较规则，`bestLen[i]` 记录获胜候选的长度。

前置条件：range 非空、模型可用、`1<=L<=min(n,UINT16_MAX)`、工作区独占。入口先完成：

```cpp
scratch.dpRing.resize(L + 1);
scratch.bestLen.resize(n);
```

当前 range 只需把 `dpRing[n % (L+1)]` 写为 0。其他槽位会在首次有效读取前由反向递推写入，不需要每个位置清空整个环。调试版本可以额外记录槽位代表的绝对下标，检查读取的确是所需未来位置。

设输出容器当前长度为 `outStart`，先检查 `outStart+n` 溢出并为最多 n 个追加词预留空间。工作区分配和输出预留都在生成结果之前完成。分配失败沿用现有异常传播，不能把内存不足当成正常 miss。

### 7.3 接近实现的主循环

下列 `begin` 引用原 Rune 数组，`out` 是追加式输出，`walker` 已绑定当前 range。`unknownWeight` 来自当前词典，`R=L+1`。

```cpp
template<class Walker>
void CutRangeFused(RuneStrArray::const_iterator begin,
                   size_t n, size_t L, double unknownWeight,
                   Walker& walker, MPCutScratch& scratch,
                   std::vector<WordRange>& out) {
  const size_t R = L + 1;
  scratch.dpRing[n % R] = 0.0;

  for (size_t i = n; i-- > 0;) {
    double best = MIN_DOUBLE;
    uint16_t chosenLen = 1;
    size_t futureSlot = (i + 1) % R;
    const size_t limit = std::min(L, n - i);
    walker.Reset();
    bool alive = true;

    for (size_t len = 1; len <= limit; ++len) {
      alive = walker.Step(i + len - 1);
      const bool terminal = alive && walker.IsTerminal();

      // 首字必有候选，后续只有词典终点参与打分。
      if (len == 1 || terminal) {
        const double weight = terminal
            ? walker.TerminalWeight() : unknownWeight;
        double score = 0.0;
        if (i + len < n) {
          score += scratch.dpRing[futureSlot];
        }
        score += weight;
        if (score > best) {
          best = score;
          chosenLen = static_cast<uint16_t>(len);
        }
      }

      if (!alive) {
        break;
      }
      if (++futureSlot == R) {
        futureSlot = 0;
      }
    }

    scratch.dpRing[i % R] = best;
    scratch.bestLen[i] = chosenLen;
  }

  for (size_t i = 0; i < n;) {
    const size_t len = scratch.bestLen[i];
    assert(len >= 1 && len <= n - i);
    out.push_back(WordRange(begin + i, begin + i + len - 1));
    i += len;
  }
}
```

`futureSlot` 每扩展一个字符都推进一次，包括经过非终点前缀的情况。不能仅在命中终点时推进，否则长度 1、2、4 的候选会读错 DP 槽。

每个起点独立从 root 开始；起点按 `n-1...0` 处理，匹配方向仍向右。失败的首字先完成单字兜底再退出；失败的后续字符不产生候选。

该片段假设入口已完成分配和后端检查。生产实现应使 Walker 的查询接口无分配、无写模型，并在入口处理空 range。

### 7.4 环形数组正确性

计算位置 i 时，候选结束后的位置只可能在 `i+1...min(i+L,n)`，这些分数已经完成。`R=L+1` 保证 i 与未来 L 个位置的槽位互不相同。

写入 `dpRing[i%R]` 覆盖的是更远、后续不会再读取的旧状态。`bestLen` 必须保留整个 range，因为正向恢复会访问此前选中的任意起点，不能把它也循环覆盖。

当 n 大于 R 时，上述关系依然成立。验收必须包含远长于 R 的文本，避免测试只覆盖尚未发生环回的情况。

### 7.5 入口分派与回退

将现有 `MPSegment::Cut` 主体整理为 `CutRangeLegacy()`，完整保留 `Find → CalcDP → CutByDag`。新的内部入口执行：

```text
空 range → 直接返回
LegacyDag，或所选模式需要 DAT 但 DAT 未就绪 → CutRangeLegacy
计算本次 L
L 不能由 uint16_t 表示 → CutRangeLegacy
准备 scratch 和输出容量
按 Pointer 验证模式或 Raw DAT 优化模式分派模板内核
```

PointerFused 从启动词典元数据取得真实最长词长，不需要 DAT 模型。DAT 构建失败时采用 LegacyDag，便于明确记录回退原因。

正常的“转移失败”“未登录字”“非终点”都是分词过程，不触发后端回退。不得在已追加部分输出后再调用旧内核造成重复结果。可预测的不支持情况全部在入口处理；内部不变量失败应作为错误暴露，不能用宽泛 `catch (...)` 静默掩盖。

## 8. Mix、搜索模式和关键词的接入

### 8.1 MixSegment

保留 `mpSeg_.Cut()` 返回 `vector<WordRange>` 的边界。新内核只替换这一步内部实现。

现有规则继续适用：多 Rune 词直接输出；用户词典保护的单 Rune 直接输出；其余连续单 Rune 组成一个片段交给 `HMMSegment::Cut()`。即使单字本身在主词典中，也可能进入 HMM。

`hmm=false` 直接返回主词典结果。第一版保留现有 `words` 和 `hmmRes` 临时容器，不把“省掉 DAG”扩大解释为已经消除了 Mix 的所有缓冲。

### 8.2 QuerySegment

`CutForSearch` 的最终输出允许重叠，不能只用一组互不重叠的最佳边界代替它。保持当前顺序：

1. 对每个 Mix 词，长度大于 2 时按位置顺序查找并输出二元词。
2. 长度大于 3 时按位置顺序查找并输出三元词。
3. 输出原 Mix 词。

第一版这些精确查词仍走原 `DictTrie::Find()`。后续可新增只返回存在性的 `Contains()`，用同一 DAT 服务这部分查询；不要为仅判断存在的调用重造 `DictUnit`。

### 8.3 KeywordExtractor

`Jieba` 的各 Segment 和 `extractor` 已共享同一个 `DictTrie`，因此通过共享词典选择后端，避免关键词路径另建第二份 DAT。

保留关键词提取中的停用词过滤、词频统计、逆文档频率加权、结果数量限制和排序行为。主词典优化只改变内部获得分词边界的方式。

验证需分别覆盖普通分词、搜索 token 顺序、关键词及其权重。对于关键词排名相同或分数相同的情况，继续使用原实现的排序和返回规则，不在本次优化中另行定义顺序。

## 9. 第二阶段：HMM 的 CPU 内存优化

### 9.1 与第一版分开提交

主词典输出已与旧实现一致后，再优化 HMM，使用独立选项和独立测试。这样可以定位差异发生在候选、主词典 DP、单字分流还是 HMM 状态递推。

HMM 仍先执行原 ASCII 规则：字母或数字起始均收集连续字母和数字，再可选收集一个小数点及后续数字；其他 ASCII 单独输出。此处按当前 C++ 基线修正设计初稿的描述。只对其余连续片段执行 Viterbi，规则边界不变。

### 9.2 滚动分数和回溯

设进入 Viterbi 的片段长为 X。当前实现保存 `weight[4X]`、`path[4X]` 和 `status[X]`，分别是分数、前驱状态和最终状态。递推只读取上一列，因此改为：

```cpp
struct HmmScratch {
  double prev[4];
  double cur[4];
  std::vector<uint8_t> path;   // X*4，按 path[x*4+y] 存储
  std::vector<uint8_t> status; // X，供原输出循环消费
};
```

`path` 中前驱只需表示 0～3；第 0 列使用 `0xFF` 表示无前驱。分配 `4*X` 前检查乘法溢出。初始化及递推必须严格保留：

```text
对 y 按 B/E/M/S:
    prev[y] = startProb[y] + emit(y, rune[0])

对 x 从 1 到 X-1:
    对 y 按 B/E/M/S:
        cur[y] = MIN_DOUBLE
        path[x*4+y] = E
        e = emit(y, rune[x])
        对 p 按 B/E/M/S:
            score = (prev[p] + transProb[p][y]) + e
            若 score > cur[y]:
                cur[y] = score
                path[x*4+y] = p
    交换 prev 和 cur

state = prev[E] >= prev[S] ? E : S
对 x 从 X-1 到 0:
    status[x] = state
    若 x > 0: state = path[x*4+state]
```

空片段直接返回。片段长度为 1 时只执行初始化和终态选择，不读第 0 列前驱。输出仍沿用 `status[x] % 2` 判断词尾。

不能跳过所谓“非法”前驱或把 `MIN_DOUBLE` 当负无穷：它是有限值，旧实现遍历全部 4 个前驱，并在没有严格更优候选时保留默认前驱 E。维持括号中的加法顺序，不能提前把转移概率与发射概率相加。

在 `int=4 B、size_t=8 B` 的平台，原三组数组约为 `56X B`，上述数据约为 `5X+64 B`；均不包含输入和 `WordRange` 输出。先完成这种状态压缩，再决定是否进一步把 path 压成位字段。

### 9.3 发射概率的连续数组

发射概率表示“某个状态产生当前 Rune 的分数”。当前四个哈希表可以派生出按 Rune 排列的只读表，每行保存 B/E/M/S 四个 `double`。

从已加载的 C++ `HMMModel` 统计 `firstRune` 与 `lastRune`，令 `span=lastRune-firstRune+1`。分配 `span*4` 个 `double` 并预填 `MIN_DOUBLE`，再逐项复制原模型值，不重新解析概率文件。

```text
Emit4(rune):
    若 rune < firstRune，返回四个 MIN_DOUBLE
    idx = uint64(rune) - firstRune
    若 idx >= span，返回四个 MIN_DOUBLE
    否则读取 emission[idx*4 ... idx*4+3]
```

当前 HMM 的范围为 U+2236～U+9FA2，span=32,109，概率数组为 1,027,488 B。该规模是当前快照，不写死成通用上限。对于空发射表，全部查询返回默认值；对于跨度过大、乘法溢出或预算不足的模型，继续使用原哈希表查询，滚动 Viterbi 仍可独立启用。

派生表和原 HMM 模型必须在整个调用期间一致。仅为冻结模型构建它，禁止加载新概率后继续使用旧数组。第一版保留哈希表时，新增数组会增加模型内存；只有后续替换原存储才能计入模型节省。

## 10. 冻结、并发与异常

### 10.1 冻结模式的可执行规则

`DictTrie` 增加初始化完成标记，启动词典加载时尚未冻结。初始化完成后，非 Legacy 模式的以下入口在修改任何数据前检查冻结状态：

| 接口 | 冻结后的行为 |
|---|---|
| 两个 `InsertUserWord` 重载 | 返回 false |
| `DeleteUserWord` | 返回 false |
| 所有公开 `LoadUserDict` 重载 | 抛出 `std::logic_error`，明确对象词典已冻结 |
| 公开 `InserUserDictNode` | 同样拒绝，不能留下绕过冻结检查的入口 |

启动加载应调用内部加载函数，或者在构造结束前保持初始化状态。即使 DAT 构建回退，已选择的冻结契约也保持一致。调用者可通过拟新增 `IsDictionaryFrozen()` 查询该契约。

这里采用拒绝更新，是因为“先更新旧 Trie，再禁用 DAT”并不能覆盖当前所有运行时接口：

- `InsertUserWord()` 没有更新启动用户单字保护集合，不能假定新增单字自动形成 HMM 保护边界。
- 运行期 `LoadUserDict()` 追加 `static_node_infos_`，没有同步重建 Trie，还可能因 vector 扩容使旧指针失效。
- `Trie::DeleteNode(key,...)` 在 erase 后使用失效迭代器，且只处理第一条边，存在独立缺陷。

这些是当前源码事实，不能作为新算法应仿真的合法语义。需要动态词典的后续版本应先单独修正接口，再定义新旧一致的更新测试；第一版不通过调用上述缺陷路径来实现自动回退。

### 10.2 并发读

同一个初始化完成的 `Jieba` 对象可供多个分词调用共享。新模型只读，每个调用有自己的游标、DP 环和词长数组，因此多个调用之间不共享写入状态。

模型由 `DictTrie` 持有、生命期跟随所属 `Jieba` 对象。调用方负责保证所有活动分词调用结束前，对象不会被销毁或替换。仅在构造和验证全部完成后，才把对象发布给其他线程。

`ResetSeparators()` 会改变预切分配置，和正在运行的查询也必须由调用方串行化。它不修改词典，DAT 不需要重建。

DAT 构建只发生在初始化阶段，不在每次分词调用中重建。初始化耗时和峰值内存应单独测量，多个调用复用同一份只读模型。

### 10.3 异常与输出完整性

- 模型构建失败：保留原对象及旧后端，释放构建临时内存。
- 不支持本次 L：在输出前进入旧路径。
- 请求 scratch 分配失败：沿用 C++ 异常，由现有外层处理，不循环尝试分配更大的旧 DAG。
- 普通词典 miss：按候选规则处理，不报错。
- 调试差分不一致或结构不变量失败：报告具体输入和阶段，阻止默认启用。

优化函数不静默吞掉分配失败或程序错误，调用方继续按照既有异常约定处理失败。

## 11. 文件级任务与实施顺序

### 11.1 文件级改动

下表采用 cppjieba 库根目录下的建议路径，名称为拟新增或拟修改文件。若目标工程采用其他源码目录布局，保持职责划分并调整路径即可。

| 文件 | 具体职责 |
|---|---|
| `include/cppjieba/CpuCutOptions.hpp` | 后端枚举、选项及冻结契约说明 |
| `include/cppjieba/DatModel.hpp` | Unit 解包、只读模型、游标、原始 Rune 查询 |
| `include/cppjieba/DatBuilder.hpp` | 构建输入、资源限制、结构化结果和统计 |
| `src/DatBuilder.cpp` | 权重去重、逻辑 Trie、位图布局、打包和验证 |
| `include/cppjieba/MPCutScratch.hpp` | 主词典调用工作区 |
| `include/cppjieba/FusedMPCut.hpp` | Walker 及融合模板内核 |
| `include/cppjieba/Trie.hpp` | 增加只读遍历入口，保留现有 Find 和节点类型 |
| `include/cppjieba/DictTrie.hpp` | 拥有 DAT、记录最长词长及后端状态、冻结检查 |
| `include/cppjieba/MPSegment.hpp` | 保留旧内核，增加预检查、分派及 scratch 重载 |
| `include/cppjieba/MixSegment.hpp` | 传递工作区，保留混合分词规则 |
| `include/cppjieba/QuerySegment.hpp` | 传递工作区，保留搜索扩展 |
| `include/cppjieba/Jieba.hpp` | 透传构造选项，保持现有公开调用形式 |
| `CMakeLists.txt` | 将构建器编译为可链接目标，提供独立测试和基准目标 |
| `tests/` | 独立差分、边界、并发读测试及性能程序 |

第二阶段另外修改 `HMMSegment.hpp`，新增 HMM scratch 和派生发射表；不要把它作为第一版主词典内核的依赖。

新增实现沿用项目 C++ 标准和两空格缩进，不依赖 `std::span` 等更高版本接口。构建器放在 `.cpp` 中，避免大型构建逻辑在多个头文件引用处重复实例化。采用新版头文件、实例化词典初始化路径的独立程序应链接构建器目标，即使运行时选择 Legacy，也不能依赖编译器恰好消除未执行分支来避免链接依赖。若需继续提供纯头文件的 Legacy 用法，应以显式编译开关排除 DAT 构建引用，并拒绝该构建中不可用的模式。

### 11.2 可分别验证的提交

| 顺序 | 内容 | 完成条件 |
|---|---|---|
| 1 | 提取 Legacy 内核，增加选项、工作区和冻结检查 | 默认路径所有输出不变 |
| 2 | PointerWalker + 融合 DP | 与 Legacy 的逐位置决策及最终结果一致 |
| 3 | raw DAT 构建与独立查词 | 全词典、结构、未命中及权重位模式校验通过 |
| 4 | RawDatWalker 接入融合内核 | 同一测试集对 PointerFused 和 Legacy 均一致 |
| 5 | Jieba 各公开入口的完整验证 | Cut/Search/CutSmall/关键词及兼容接口均一致 |
| 6 | CPU 基准与默认选择 | 正确性、冷启动、模型内存及吞吐证据齐备 |
| 7 | 独立 HMM 优化 | 状态递推、最终边界及全部上层结果一致 |

默认选择属于第 6 步，不能在第 3 步仅根据“DAT 已能查词”就替换生产默认路径。

## 12. 内存与已有证据

### 12.1 归档布局结果

位图 raw 归档使用主词典加 4 条启动用户词，共 348,986 个生效词、497,996 个逻辑节点、5,087 个唯一权重。表中统计自包含模型段，不含 HMM、构建临时内存和清单：

| 指标 | 结果 |
|---|---:|
| DAT 槽位数 M | 802,401 |
| 填充率 N/M | 62.0632% |
| `units[M]`，每槽 8 B | 6,419,208 B |
| `uniqueWeights[5087]`，每项 8 B | 40,696 B |
| `unknownWeight` | 8 B |
| 用户单字集合 | 当前快照为空，0 B |
| 模型段合计 | 6,459,912 B ≈ 6.161 MiB |

MiB 表示 `2^20` 字节。实际附加模型内存还受容器容量和对象元数据影响，不能直接把归档文件大小写成进程内存。CPU 第一版继续使用原用户单字集合，不重复保存一份。

归档权重由 Python 计算，仅证明其参考环境下的位级一致性。CPU 实现使用目标 C++ `DictTrie` 结果；布局可对照归档，权重验收必须对照本次 C++ 基线。

### 12.2 主词典工作区

典型 64 位布局下 `sizeof(Dag)` 约为 328 B，须在目标编译器上实测。旧 `Dag` 自身已经包含 DP 分数和回溯指针，软件基线不能再额外加一份完整 DP 数组来放大收益。

只比较主词典内核临时状态：

```text
旧实现：n * sizeof(Dag) + 候选溢出容量
融合 raw：2n + 8(L+1)
```

当 `n=65,536、L=16`：

| 项目 | 大小 |
|---|---:|
| 旧 DAG，仅按 328 B/Rune | 21,495,808 B = 20.5 MiB |
| raw 融合工作区 | 131,208 B ≈ 128.13 KiB |

这些数值不含原 Rune 数组、输出、Mix 临时容器和 HMM 工作区。第一版 HMM 不变时，总调用内存不能直接按上述主词典比例宣称下降。

第一版进程模型内存为“原模型 + 新 DAT”，不是两者相减。应同时报告初始化峰值、稳定模型内存和多并发调用临时内存，才能判断总体收益。

### 12.3 运行成本

融合内核消除候选写入 DAG、DP 回读候选、DAG 对象构造和销毁。它不消除所有起点的前缀匹配，也不改变必要的候选打分次数。

位图 raw DAT 用整数寻址和父状态比较代替哈希及指针追踪。CPU 上的实际收益取决于缓存行为、文本分布和请求长度，必须通过完整分词路径实测。

## 13. 验证与验收

### 13.1 分层差分

“差分”指对相同输入运行保留的旧实现和新实现，比较它们的结果。旧内核必须独立保留，不把它也重写为融合算法后再互相比较。

1. **模型层**：比较全部生效词的终点、权重位模式、未知单字权重、用户单字集合和真实最长词长。
2. **候选层**：每个起点比较 `(len, 是否词典终点, weight bits)` 序列，包含单字兜底。
3. **DP 层**：用测试专用观察接口比较每个位置的最佳分数位模式和最佳长度，不只比较最终 token 集合。
4. **恢复层**：比较有序 WordRange 的 Rune 起止、byte offset、Unicode offset/length。
5. **完整算法层**：比较 Mix、Search、关键词和 HMM 开关的输出。

DP 观察接口只用于测试，不能在生产路径为了验证而重新分配完整分数数组。HMM 第二阶段另比较每列 4 个分数、前驱、最终状态序列和词边界。

### 13.2 必测用例

| 类别 | 覆盖点 |
|---|---|
| 常规中文 | “小明在南京市长江大桥”、长文章、多前缀歧义 |
| 单字与前缀 | 空 range、单 Rune、未登录 Rune、仅前缀不是词、root miss |
| 候选顺序 | 同一起点命中长度 1、2、4，中间有非终点，候选超过 16 个 |
| 词长参数 | 0、1、2、默认 512、大于 512、65535、65536、`SIZE_MAX`；配套构造长词 |
| 环回 | n 小于、等于、大于 L+1，多个周期以及同一 scratch 连续处理不同 n/L |
| 浮点 | 完全同分、非常接近、正负零、极小有限权重、没有候选严格超过 `MIN_DOUBLE` |
| 启动用户词 | 覆盖主词、多文件覆盖顺序、缺省权重、单字保护、比主词典更长的词 |
| 字符 | 中英数字混合、小数点、标点、非 BMP、模型外 Rune、既有解码器的失败输入 |
| DAT 格式 | 负 base、合法边界值、越界目标、空槽、父校验不匹配、保留位、权重哨兵 |
| 位图构建 | 位偏移 0/63/64/65、跨字、尾部不足 64 位、扩容、最低可行锚点、仅一个孩子 |
| 接口兼容 | 预先非空的追加输出、Search 重叠词顺序、Tag、CutAll、关键词数量限制及排序 |
| 冻结与失败 | 所有写入口拒绝且对象未变，DAT 构建失败后只读结果仍与 Legacy 一致 |
| 并发读 | 同一词典对象不同文本同时调用，工作区与输出彼此独立 |
| HMM 第二阶段 | 缺失发射、全部候选未更新默认前驱、E/S 同分、ASCII 规则、X=0/1 |

长词用例不要调用已有缺陷的动态删除接口制造模型，应通过临时启动词典或测试构建输入生成。非有限权重用于检查 DAT 不支持状态及回退，不把这类输入作为优化浮点内核必须接受的模型。

### 13.3 基准设计

在同一机器、编译器、优化参数、词典和语料上比较以下版本：

| 版本 | 用途 |
|---|---|
| A：LegacyDag | 原始基线 |
| B：PointerFused | 开发对照，衡量消除 DAG 和压缩 DP 的收益 |
| C：DatRawFused | 在 B 基础上衡量位图 raw DAT 收益 |
| D：DatRawFused + HMM 优化 | 单独衡量 HMM 改动 |

至少包含短文本、常规文档、长 range，分别测试主词典内核、`Cut`、`CutForSearch`、`CutSmall` 和关键词提取。区分单线程和多个线程共享模型的吞吐，记录线程数及绑定方式。

冷启动指首次加载模型和构建 DAT；暖态指模型已准备好后的重复调用。分别报告：

- 冷启动时间及其中加载、布局、验证所占时间。
- 每模型稳定内存、初始化峰值、每调用工作区与分配次数。
- 每秒处理的原始 UTF-8 字节数和 Rune 数，明确两种分母。
- 单请求中位数及 P95/P99，即 95%/99% 请求低于该值的延迟。
- DAT 转移次数、候选次数和实际后端/回退次数。

逐字符计数会影响热路径，使用专门的诊断构建采集，正式性能构建关闭计数。记录重复运行的原始数据和波动，不以单次最快值选默认后端。

### 13.4 启用门槛

默认切换前必须满足：

1. 必测差分全部通过，在项目支持的目标编译配置上没有 token、坐标或权重差异。
2. 默认模型无需回退，自定义模型触发回退时行为可解释且测试通过。
3. 冷启动与多模型内存成本已有测量，构建预算已有正式默认值。
4. 面向目标应用的主要文本分布，完整 Jieba 路径收益稳定，短文本没有超过预先约定的回归门槛。
5. 并发只读测试通过，存在一处可恢复 Legacy 的统一模式选择。

不在设计文档中预设“必然提升几倍”。位图 raw DAT 未达到验收要求时，保留 Legacy 默认路径，利用 PointerFused 对照定位问题，完成改进和复测后再决定启用。

## 14. 后续演进

### 14.1 释放旧模型

要降低 CPU 模型常驻内存，必须先处理返回 `DictUnit*` 的兼容接口。可分两步：

1. 将只需存在性的 Search 查询迁移到 `Contains()`；将需要候选的消费者迁移到长度和权重接口。
2. 为仍需 `word/tag` 的接口设计独立的终点元数据，再决定是否保留原返回类型。

如果继续保留 `const DictUnit* Find(...)`，必须保留地址稳定的词典载荷，并额外建立终点到载荷的映射。`weightCode` 不能兼作词条编号，因为多个不同词可以共享同一个权重。所有兼容消费者迁移完成后，才能释放 Pointer Trie；不能仅凭分词主循环不读它就删除。

### 14.2 动态词典

后续动态版本先修正第 10 节的既有接口缺陷，再采用完整快照更新：在独立对象中应用更新，重新构建并验证，等待原有读者结束或以共享所有权保留旧快照，然后整体发布新对象。

一次调用必须始终使用同一代的词典、最长词长、用户单字集合及相关模型。不能中途切换新 DAT，也不能只修改旧 Trie 导致不同接口看到不同词典。

小型增量 Trie 可在重建成本过高时进一步引入，但届时要定义覆盖、删除标记、同长度去重和快照一致性；不作为第一版的隐含依赖。

### 14.3 进一步优化的次序

先测量候选数与访问分布，再评估减少取模、预取、同位置多候选并行打分或跨文档并行。单个 range 的相邻 DP 位置存在依赖，不能直接用并行循环替换反向循环。

也不能按固定长度随意切断长 range，即使保留 L-1 个重叠字符，尾部最优分数仍可能沿单字路径影响前部。第一版的并行单位采用独立文档或完整 `PreFilter` range，输出按原顺序合并。

## 15. 参考材料与维护规则

- [融合 DP 设计](jieba_optimized.md)：反向枚举、环形数组和 bestLen。
- [位图 raw DAT 设计](dat_optimization/02_bitmap_raw/DESIGN.md)：空槽位图、最低地址优先布局和模型格式。
- [位图 raw 构建代码](dat_optimization/02_bitmap_raw/code/layout.py)：逻辑 Trie 和位图布局参考。
- [位图 raw 查询与验证代码](dat_optimization/02_bitmap_raw/code/dat_model.py)：权重保存、查询和差分校验参考。
- [初版 Trie.hpp](https://github.com/lbsswhu/cppjieba/blob/8b8a3f7/include/cppjieba/Trie.hpp)、[DictTrie.hpp](include/cppjieba/DictTrie.hpp)、[MPSegment.hpp](include/cppjieba/MPSegment.hpp)：主词典行为基线。
- [MixSegment.hpp](include/cppjieba/MixSegment.hpp)、[QuerySegment.hpp](include/cppjieba/QuerySegment.hpp)、[HMMSegment.hpp](include/cppjieba/HMMSegment.hpp)：后处理及 HMM 基线。

实现过程中修改接口、支持范围或默认后端时，应同步更新本文对应章节。模型大小只引用明确输入和构造算法下的结果；性能数字必须标明实际软件版本及测试边界。


## 16. 本仓库实施记录（2026-09-18）

- 选项使用 `CpuCutOptions::mode`、`optimize_hmm`、`hmm_dense_budget_bytes` 和 `dat_build_options`。默认仍为 `LegacyDag`，HMM 优化默认关闭。用户在构造中明确选择优化；没有自动改变默认值。
- 当前 `DictTrie` 已缓存主词典，构建器按“缓存主词典、启动用户词典”的顺序接收稳定的 `const DictUnit*`，不重复解析权重。临时 Rune 副本在构建 DAT 前释放。
- DAT 默认预算为 2,097,151 槽、256 MiB 构建临时内存、10,000 ms 和 8,191 个唯一权重。`DatBuildStats` 记录失败类别/原因、构建/布局/验证时间和模型数组字节数；数组字节数不等于进程 RSS。基准二进制提供预算调整参数。
- Pointer Walker 通过 `Trie` 的友元访问获取内部节点，外部只取得 `const Trie&`；`Trie` 禁止浅拷贝，避免绕过冻结或双重释放。
- 65,536 Rune 回退测试暴露旧 Trie 递归析构栈溢出。释放过程改为复用节点载荷存储的侵入式工作链，不增加节点大小、不分配内存、不递归。
- 所有公开分词入口复用调用内 MP/HMM 工作区。HMM 从原 C++ 概率派生稠密表，预算失败时仍可使用滚动 Viterbi 和原哈希表。优化模型拒绝加载接口修改；为兼容保留的公开概率字段，在优化模式下也禁止调用方直接写入。
- CMake/Bazel 均编译并链接 `src/DatBuilder.cpp`。独立消费者必须链接 `cppjieba`，包括 Legacy 初始化；本次不提供纯头文件编译开关。CMake 对调用方传播严格浮点选项，GCC/Clang fast-math 构建被内核头文件拒绝。
- 当前工作区的 `LocalVector` 已使用 `std::vector`，GCC 11 实测 `sizeof(Dag)=72 B`；第 12 节的 328 B 是设计举例，不用于本次内存收益计算。
- 当前实现有独立 DAT、逐位置 MP 决策/分数、逐列 HMM 前驱/分数测试，以及全接口差分、超长词、预算回退和共享模型并发读取测试。新增测试不改变原 UTF-8 解码语义。
- 基准 A/B/C/D、原始 HEAD 独立二进制、分配诊断和重复运行原始数据见 [CPU 性能报告](docs/cpu-performance.md)。默认切换和发布判断以该报告的初始化、内存和完整路径指标为依据。


## 17. 删除 Pointer Trie，统一只读 DAT（2026-09-20）

用户明确选择所有词典初始化后只读，通过启动用户词典提供自定义词。本阶段覆盖第 14.1 节的旧查询结构释放，不引入运行时重建或增量结构。

1. 删除 `include/cppjieba/Trie.hpp` 及 `Trie`、`TrieNode`、`PointerWalker`；`DictTrie` 不再构造、持有或释放旧树。`DictUnit`、`Dag` 与 `MAX_WORD_LENGTH` 移入 `DictTypes.hpp`，仅保留公开查询所需数据类型。
2. `DatModel` 增加按槽位排列的 `uint32_t terminalSourceIndices`，占每槽 4 字节。索引指向构建输入中原始加载序号，主词典条目在前，启动用户条目在后。非终点使用 `UINT32_MAX`。重复词保留最后加载的索引；共享 `weightCode` 的不同词仍对应各自的词、词性和地址稳定载荷。
3. `DictTrie::Find` 的精确查询和 DAG 候选枚举均由 DAT 完成。新增 `Contains` 避免存在性查询访问词条元数据；Search 的二/三元词扩展使用该接口，输出顺序不变。`FullSegment` 和 `PosTagger` 保持公开输出语义。
4. `CpuCutMode` 仅保留 `DatRawFused`，默认即为该模式。启动加载完成后所有词典冻结。两种 `InsertUserWord` 及 `DeleteUserWord` 返回 false；所有公开用户词加载接口拒绝修改并抛出 `logic_error`。HMM 开关和存储在本阶段保持不变，以单独测量去除 Trie 的效果。
5. 删除 Legacy MP 内核。`L<=65535` 仍用 `uint16_t bestLen`；更长请求在同一融合递推中使用 `size_t bestLenWide`，不截断词长。两条路径都保持 FP64、严格大于比较、有限 `MIN_DOUBLE` 和相同加法顺序。
6. DAT 构建失败时构造函数抛出 `DatBuildError`，`GetStats()` 返回结构化状态和原因，不发布半成品。构建预算计入源索引表，统计分别报告 `topology_bytes`、`source_index_bytes`、`weight_bytes` 及总 `model_bytes`。
7. 对照程序分别链接提交 `8b8a3f7` 与当前库，在独立进程中逐字节比较 token、坐标、搜索、全模式、查词、词性和关键词权重。小词典另用独立词表穷举检查全部候选及 DP；测试目录不保留旧 Pointer Trie 实现。
8. 内存与性能基准同时测量上一版默认 A、上一版 DAT+旧 Trie 的 D，以及本版 DAT-only+D；新旧 HMM 设置保持一致。常规 RSS、峰值 RSS 和单独的空闲页归还诊断分开记录。方法、原始数据和发布评估见 [DAT 独立模型报告](docs/dat-only-performance.md)。
