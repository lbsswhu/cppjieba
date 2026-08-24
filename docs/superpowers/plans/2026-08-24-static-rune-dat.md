# Static Rune Double Array Trie Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the heap-node dictionary trie with a repository-owned, immutable C++11 static double-array trie keyed directly by Unicode `Rune` values while preserving Jieba segmentation, tagging, lookup, and keyword-extraction results.

**Architecture:** Parse the main dictionary and constructor-time user dictionaries into stable Rune-lexicographic records, collapse duplicates with the last user occurrence winning, intern tags, and build five equal-length DAT arrays owned by an immutable `DictionaryData`. `DictTrie` instances share that data through a mutex-protected `weak_ptr` cache, while segmenters consume value-based `DagEdge` records and perform only read-only lookups.

**Tech Stack:** C++11 header-only library, `std::vector`, `std::shared_ptr`/`std::weak_ptr`, `std::mutex`, GoogleTest, CMake/CTest, Bazel, and the existing `test/benchmark.cpp` harness.

---

## Fixed design and compatibility boundary

- The new implementation is a classic static double-array trie in `include/cppjieba/DoubleArrayTrie.hpp`. Labels are the original `uint32_t` Unicode `Rune` values. Do not add Rune compaction, a packed Darts unit, DAWG minimization, TAIL storage, dynamic CEDAR relocation, a binary on-disk format, a prebuilder, or memory mapping.
- Slot `0` is the root. The five arrays are `std::vector<int32_t> base_`, `std::vector<int32_t> check_`, `std::vector<uint8_t> terminal_`, `std::vector<double> weight_`, and `std::vector<uint16_t> tag_id_`; they always have identical lengths. `check_[i] == -1` means empty, and `check_[0] == 0` marks the occupied root.
- A transition is `base_[state] + rune`. `base_` may be negative so sparse raw code points do not force an array indexed directly by Unicode scalar value. Slot `0` is root-only: every build-time and lookup-time transition address must be strictly greater than `0`, preventing a negative root base from turning Rune `-base_[0]` into a root self-loop. Perform candidate-base, transition, capacity, and cast calculations in `int64_t`; return a build error before any non-positive address or value not representable by `int32_t` is stored.
- Build from strictly ordered unique keys. Define one `RuneLexicographicLess` helper with `std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end())`; reuse it for core strict-order validation, dictionary stable sorting, and equality-by-two-comparisons. Extract siblings from contiguous ordered-key intervals at a given depth, scan free candidate slots for the first sibling, validate the full sibling span, claim all child slots, and recurse. After construction, remove every empty trailing slot from all five arrays and release the temporary word/record vectors used only during construction.
- The main dictionary determines a finite positive `freq_sum` and retains the mathematically equivalent existing `log(freq / freq_sum)` weight as `log(freq) - log(freq_sum)`, avoiding ratio underflow. A constructor-time user row with an explicit frequency uses the same main `freq_sum`; a row without one uses the selected main-dictionary minimum, median, or maximum finite log weight. Reject aggregate frequency overflow and any non-finite computed weight before DAT construction, so DAG edges and MP dynamic programming never receive NaN or infinity. Stable Rune sorting plus source ordinals makes the last occurrence from the ordered user-path list win duplicate words.
- Main rows are exactly `word frequency tag`. User rows are exactly `word`, `word tag`, or `word frequency tag`. Reject blank or wrong-column rows, invalid UTF-8, empty words, nonnumeric/trailing-junk/zero/negative/NaN/infinite frequencies, non-finite/non-positive aggregate frequency, non-finite weights, an empty effective dictionary, more than 65,535 distinct non-empty stored tags, and DAT address overflow. Use fatal `XCHECK` diagnostics at the `DictTrie` construction boundary; keep `DoubleArrayTrie::Build` independently testable through its `bool` plus error string.
- `DictionaryData` is immutable after construction. Cache keys contain the exact main path, the ordered constructor user-path vector split on `|` or `;`, and `UserWordWeightOption`. A process-wide mutex protects a map of `weak_ptr<const DictionaryData>`; build outside the lock, then recheck under the lock so racing constructors converge on one live shared object.
- The deduplicated tag table has `tags[0] == ""`; non-empty tags use IDs `1..65535`. `user_single_runes` contains sorted unique single-Rune words originating from constructor user dictionaries, and membership uses `std::binary_search`.
- `DictionaryStats` exposes `slot_count`, `occupied_state_count`, `terminal_count`, `load_factor`, `array_bytes`, and `tag_bytes`. `array_bytes` is exactly `slot_count * (sizeof(int32_t) * 2 + sizeof(uint8_t) + sizeof(double) + sizeof(uint16_t))`; `tag_bytes` is the sum of byte lengths of unique stored tag payloads, excluding `std::string` object/capacity overhead.
- Replace pointer-bearing DAG data with `DagEdge { size_t end; double weight; bool in_dict; }`. Each `Dag` contains `runestr`, shortest-first `edges`, DP `weight`, and chosen `next_pos`. `MPSegment` evaluates `edge.weight + suffix.weight`, updates only on strict `>`, and advances to `next_pos + 1`; this preserves shortest-edge tie selection.
- `FullSegment` bases its old inclusion rules on `edge.end - start + 1` and `edge.in_dict`. `QuerySegment` calls the boolean exact lookup `Contains`. `PosTagger` calls `FindTag(begin, end, std::string*)`. `MixSegment` checks constructor user single-Rune membership through binary search.
- Preserve the public high-level `Jieba::Cut`, `CutAll`, `CutForSearch`, `CutHMM`, `CutSmall`, `Tag`, `LookupTag`, and string `Find` results. Keep both `InsertUserWord` overload signatures and `DeleteUserWord`; they log `ERROR` and return `false`. Keep all three post-construction `LoadUserDict` overload signatures; they log `ERROR` and do nothing. Constructor-time user dictionary paths remain supported. The old bottom-level `Trie`, `DictTrie`, and `Dag` source interfaces require no compatibility adapters.
- Treat the change as a major-version candidate: post-construction dictionary mutation changes from effective to explicitly unsupported, and low-level source APIs change. The release assessment target is `v6.0.0`, gated by the verification and RSS criteria in Task 5.

## Proposed types and names

Use these names without synonyms in later tasks:

```cpp
struct DagEdge {
  size_t end;
  double weight;
  bool in_dict;
};

struct Dag {
  RuneStr runestr;
  LocalVector<DagEdge> edges;
  double weight;
  size_t next_pos;
  Dag() : runestr(), edges(), weight(0.0), next_pos(0) {}
};

struct DictionaryStats {
  size_t slot_count;
  size_t occupied_state_count;
  size_t terminal_count;
  double load_factor;
  size_t array_bytes;
  size_t tag_bytes;
};
```

`DoubleArrayTrie` owns `BuildEntry`, `Match`, `BuildOptions`, and the five-array portion of `Stats`. `DictTrie` exposes these read-only methods:

```cpp
bool Contains(RuneStrArray::const_iterator begin,
              RuneStrArray::const_iterator end) const;
bool Find(const std::string& word) const;
bool FindTag(RuneStrArray::const_iterator begin,
             RuneStrArray::const_iterator end,
             std::string* tag) const;
void BuildDag(RuneStrArray::const_iterator begin,
              RuneStrArray::const_iterator end,
              std::vector<Dag>& result,
              size_t max_word_len = MAX_WORD_LENGTH) const;
bool IsUserDictSingleRune(Rune rune) const;
double GetMinWeight() const;
const DictionaryStats& GetStats() const;
const void* GetDictionaryDataIdentity() const;
```

`FindTag` returns `true` for an exact dictionary terminal even when its tag is ID `0`/empty; callers distinguish “not found” from “found without a tag” by the boolean. `GetDictionaryDataIdentity()` returns `data_.get()` only as a read-only identity token for cache tests.

### Task 1: Test contract and `DoubleArrayTrie` core

**Files:**

- Create: `include/cppjieba/DoubleArrayTrie.hpp`
- Create: `test/unittest/double_array_trie_test.cpp`
- Modify: `test/unittest/CMakeLists.txt`

- [ ] **Step 1: Register and write the core contract tests before the header exists**

Add `double_array_trie_test.cpp` immediately after `gtest_main.cpp` in `test/unittest/CMakeLists.txt`. The new test file must include a helper that strictly decodes test words and the following complete behavioral cases:

```cpp
#include <stdint.h>
#include <limits>
#include <string>
#include <vector>
#include "cppjieba/DoubleArrayTrie.hpp"
#include "gtest/gtest.h"

using cppjieba::DoubleArrayTrie;
using cppjieba::RuneStrArray;
using cppjieba::Unicode;

namespace {

DoubleArrayTrie::BuildEntry Entry(const std::string& word,
                                  double weight,
                                  uint16_t tag_id) {
  Unicode runes;
  EXPECT_TRUE(cppjieba::DecodeUTF8RunesInString(word, runes));
  return DoubleArrayTrie::BuildEntry(runes, weight, tag_id);
}

RuneStrArray Runes(const std::string& word) {
  RuneStrArray runes;
  EXPECT_TRUE(cppjieba::DecodeUTF8RunesInString(word, runes));
  return runes;
}

}  // namespace

TEST(DoubleArrayTrieTest, FindsPrefixesInShortestFirstOrder) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("你", -3.0, 1));
  entries.push_back(Entry("你好", -2.0, 2));
  entries.push_back(Entry("你好啊", -1.0, 3));
  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(entries, &error)) << error;

  RuneStrArray input = Runes("你好啊呀");
  std::vector<DoubleArrayTrie::Match> matches;
  trie.CommonPrefixSearch(input.begin(), input.end(), 512, &matches);
  ASSERT_EQ(3u, matches.size());
  EXPECT_EQ(1u, matches[0].length);
  EXPECT_EQ(2u, matches[1].length);
  EXPECT_EQ(3u, matches[2].length);
  EXPECT_DOUBLE_EQ(-1.0, matches[2].weight);
  EXPECT_EQ(3u, matches[2].tag_id);
}

TEST(DoubleArrayTrieTest, StoresSparseRunesWithoutFalseTransitions) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("一a", -4.0, 1));
  entries.push_back(Entry("一b", -3.0, 1));
  entries.push_back(Entry("😀a", -2.0, 2));
  entries.push_back(Entry("😀b", -1.0, 2));
  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(entries, &error)) << error;
  EXPECT_LT(trie.base_at(0), 0);

  RuneStrArray present = Runes("😀b");
  RuneStrArray absent = Runes("一😀");
  EXPECT_TRUE(trie.ExactMatch(present.begin(), present.end(), NULL, NULL));
  EXPECT_FALSE(trie.ExactMatch(absent.begin(), absent.end(), NULL, NULL));
  EXPECT_GT(trie.stats().occupied_state_count, trie.stats().terminal_count);
}

TEST(DoubleArrayTrieTest, RootSlotCannotBeReachedAsATransition) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("一a", -2.0, 1));
  entries.push_back(Entry("😀b", -1.0, 2));
  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(entries, &error)) << error;
  ASSERT_LT(trie.base_at(0), 0);

  const int64_t phantom_value = -static_cast<int64_t>(trie.base_at(0));
  ASSERT_GT(phantom_value, 0);
  ASSERT_LE(phantom_value,
            static_cast<int64_t>(std::numeric_limits<cppjieba::Rune>::max()));
  const cppjieba::Rune phantom = static_cast<cppjieba::Rune>(phantom_value);
  RuneStrArray present = Runes("一a");

  RuneStrArray prefixed;
  prefixed.push_back(cppjieba::RuneStr(phantom, 0, 1));
  prefixed.insert(prefixed.end(), present.begin(), present.end());
  EXPECT_FALSE(trie.ExactMatch(prefixed.begin(), prefixed.end(), NULL, NULL));
  std::vector<DoubleArrayTrie::Match> matches;
  trie.CommonPrefixSearch(prefixed.begin(), prefixed.end(), 512, &matches);
  EXPECT_TRUE(matches.empty());

  RuneStrArray appended = present;
  appended.push_back(cppjieba::RuneStr(phantom, 0, 1));
  EXPECT_FALSE(trie.ExactMatch(appended.begin(), appended.end(), NULL, NULL));
}

TEST(DoubleArrayTrieTest, RejectsUnsortedOrDuplicateBuildKeys) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("甲", -1.0, 1));
  entries.push_back(Entry("乙", -2.0, 2));
  DoubleArrayTrie trie;
  std::string error;
  EXPECT_FALSE(trie.Build(entries, &error));
  EXPECT_NE(std::string::npos, error.find("strict Rune order"));

  entries.clear();
  entries.push_back(Entry("甲", -1.0, 1));
  entries.push_back(Entry("甲", -2.0, 2));
  error.clear();
  EXPECT_FALSE(trie.Build(entries, &error));
  EXPECT_NE(std::string::npos, error.find("strict Rune order"));
}
```

The four keys in the sparse test force sibling-range placement and overlapping descendant candidates; successful exact matches plus the missing cross-branch transition exercise `check_` collision protection. Keep the source order Rune-lexicographic (`一...` before `😀...`).

- [ ] **Step 2: Configure and run the new target to observe RED**

Run:

```bash
cmake -S . -B build-static-rune-dat -DCMAKE_BUILD_TYPE=Release
cmake --build build-static-rune-dat --target test.run -j2
```

Expected: compilation fails at `test/unittest/double_array_trie_test.cpp` because `cppjieba/DoubleArrayTrie.hpp` does not exist. Preserve that failure in the task log before adding the header.

- [ ] **Step 3: Add the complete public core contract and five arrays**

Create `include/cppjieba/DoubleArrayTrie.hpp` with self-contained C++11 includes (`<algorithm>`, `<climits>`, `<cmath>`, `<cstddef>`, `<cstdint>`, `<limits>`, `<string>`, `<vector>`, and `Unicode.hpp`) and this exact comparator/public surface. `Unicode` is `LocalVector<Rune>`, so do not assume it defines relational operators:

```cpp
inline bool RuneLexicographicLess(const Unicode& lhs, const Unicode& rhs) {
  return std::lexicographical_compare(
      lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

class DoubleArrayTrie {
 public:
  struct BuildEntry {
    Unicode word;
    double weight;
    uint16_t tag_id;
    BuildEntry(const Unicode& w, double value, uint16_t id)
        : word(w), weight(value), tag_id(id) {}
  };

  struct Match {
    size_t length;
    double weight;
    uint16_t tag_id;
    Match(size_t n, double value, uint16_t id)
        : length(n), weight(value), tag_id(id) {}
  };

  struct BuildOptions {
    int64_t max_address;
    BuildOptions() : max_address(INT32_MAX) {}
    explicit BuildOptions(int64_t limit) : max_address(limit) {}
  };

  struct Stats {
    size_t slot_count;
    size_t occupied_state_count;
    size_t terminal_count;
    double load_factor;
    size_t array_bytes;
  };

  DoubleArrayTrie();
  bool Build(const std::vector<BuildEntry>& entries,
             std::string* error,
             const BuildOptions& options = BuildOptions());
  bool ExactMatch(RuneStrArray::const_iterator begin,
                  RuneStrArray::const_iterator end,
                  double* weight,
                  uint16_t* tag_id) const;
  void CommonPrefixSearch(RuneStrArray::const_iterator begin,
                          RuneStrArray::const_iterator end,
                          size_t max_word_len,
                          std::vector<Match>* matches) const;
  const Stats& stats() const { return stats_; }
  int32_t base_at(size_t slot) const { return base_.at(slot); }
  bool last_slot_occupied() const {
    return !check_.empty() && check_.back() != -1;
  }

 private:
  std::vector<int32_t> base_;
  std::vector<int32_t> check_;
  std::vector<uint8_t> terminal_;
  std::vector<double> weight_;
  std::vector<uint16_t> tag_id_;
  Stats stats_;
  int64_t next_check_pos_;
};
```

Definitions remain inline in the class/header. Empty input is a valid empty core trie containing only the root; `DictTrie` separately rejects an empty effective dictionary.

- [ ] **Step 4: Implement checked address math and synchronized resizing**

Use one transition helper for every build-time and lookup-time address calculation. It must reject address `0` as well as negative sums, sums above the configured address limit, sums above `INT32_MAX`, and bases that cannot fit in `int32_t`:

```cpp
bool CheckedTransitionAddress(int64_t base, Rune rune, int64_t limit,
                              int32_t* address, std::string* error) const {
  const int64_t sum = base + static_cast<int64_t>(rune);
  if (base < INT32_MIN || base > INT32_MAX ||
      sum <= 0 || sum > limit || sum > INT32_MAX) {
    if (error != NULL) {
      *error = "DAT transition address must be positive and fit int32_t";
    }
    return false;
  }
  *address = static_cast<int32_t>(sum);
  return true;
}

bool EnsureSize(int64_t address, const BuildOptions& options,
                std::string* error) {
  if (address <= 0 || address > options.max_address || address > INT32_MAX) {
    if (error != NULL) *error = "DAT address exceeds configured limit";
    return false;
  }
  const size_t required = static_cast<size_t>(address) + 1;
  if (required <= base_.size()) return true;
  base_.resize(required, 0);
  check_.resize(required, -1);
  terminal_.resize(required, 0);
  weight_.resize(required, 0.0);
  tag_id_.resize(required, 0);
  return true;
}
```

Initialize all five arrays to length one, set `check_[0] = 0`, and leave every new `check_` entry at `-1`. Root creation is the only path that may occupy address `0`; it does not call either transition helper or `EnsureSize`. Validate `BuildOptions::max_address` is in `[1, INT32_MAX]` before placement.

- [ ] **Step 5: Implement ordered interval extraction and free-base placement**

Use a private `Sibling { Rune label; size_t begin; size_t end; }`. For an interval `[begin, end)` at `depth`, first mark the current state terminal when `entries[begin].word.size() == depth`, advance past that entry, then group the remaining records by `word[depth]`. Because keys are strictly sorted and unique, each group is contiguous.

For a non-empty sibling vector, scan `first_slot` from `max<int64_t>(1, next_check_pos_)`. Compute `candidate_base = first_slot - siblings.front().label` in `int64_t`. For every sibling, call `CheckedTransitionAddress(candidate_base, label, options.max_address, ...)`, grow all arrays with `EnsureSize`, and require `check_[address] == -1`. When the entire set fits, store `base_[state]`, claim every child with `check_[address] = state`, update `next_check_pos_` to the first still-empty slot, and recursively place each child interval. If `first_slot` exceeds `options.max_address`, stop scanning and return an address-exhaustion error.

The recursion must set terminal payload on the state reached by the whole word:

```cpp
if (entries[cursor].word.size() == depth) {
  terminal_[state] = 1;
  weight_[state] = entries[cursor].weight;
  tag_id_[state] = entries[cursor].tag_id;
  ++cursor;
}
```

Reject an empty word, reject a non-finite `BuildEntry::weight` with `DAT build entry requires finite weight`, and reject adjacent keys where `!RuneLexicographicLess(previous.word, current.word)`, using the diagnostic substring `strict Rune order`. Never use `Unicode::operator<` or derive an address in `size_t` before the signed checks pass.

- [ ] **Step 6: Implement exact and prefix traversal, trimming, and stats**

Traversal starts at state `0`, calls `CheckedTransitionAddress(base_[state], iterator->rune, INT32_MAX, ...)`, and accepts the transition only when the strictly positive address is in range and `check_[address] == state`. An address of `0` is always a failed transition even though `check_[0] == 0`; this is what blocks Rune `-base_[0]` from looping back to the root. `ExactMatch` returns true only when the final state has `terminal_ != 0`, and copies payloads only into non-null output pointers. `CommonPrefixSearch` clears its output, visits at most `max_word_len` runes, and appends a `Match(consumed, weight_[state], tag_id_[state])` each time it reaches a terminal; traversal order therefore guarantees shortest-first matches.

After placement, repeatedly pop the same last index from all arrays while the length exceeds one and `check_.back() == -1`. Compute stats after trimming:

```cpp
stats_.slot_count = check_.size();
stats_.occupied_state_count = 0;
stats_.terminal_count = 0;
for (size_t i = 0; i < check_.size(); ++i) {
  if (check_[i] != -1) ++stats_.occupied_state_count;
  if (terminal_[i] != 0) ++stats_.terminal_count;
}
stats_.load_factor = stats_.slot_count == 0 ? 0.0 :
    static_cast<double>(stats_.occupied_state_count) / stats_.slot_count;
stats_.array_bytes = stats_.slot_count *
    (sizeof(int32_t) * 2 + sizeof(uint8_t) + sizeof(double) + sizeof(uint16_t));
```

- [ ] **Step 7: Run the focused core tests to observe GREEN**

Run:

```bash
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run --gtest_filter='DoubleArrayTrieTest.*'
```

Expected: all `DoubleArrayTrieTest` cases pass, with no sanitizer, assertion, or logging output.

- [ ] **Step 8: Commit and immediately push Task 1**

```bash
git add include/cppjieba/DoubleArrayTrie.hpp \
        test/unittest/double_array_trie_test.cpp \
        test/unittest/CMakeLists.txt
git commit -m "feat: add static Rune double-array trie"
git push origin feat/static-rune-dat
```

Record the release assessment as “major release candidate; publishing remains gated by Task 5 RSS evidence.”

### Task 2: Immutable dictionary parsing, tag interning, cache, and exact/DAG APIs

**Files:**

- Modify: `include/cppjieba/Trie.hpp`
- Modify: `include/cppjieba/DictTrie.hpp`
- Modify: `test/unittest/trie_test.cpp`

- [ ] **Step 1: Replace pointer-oriented dictionary assertions with value/API contract tests**

Retain `DICT_FILE`, remove direct construction of `Trie`, `TrieNode`, and `DictUnit`, and replace pointer-return assertions with exact lookup, tag lookup, DAG edge, duplicate precedence, constructor user single-Rune, stats, and cache identity checks. Use an RAII test helper that writes exact bytes under `testing::TempDir()` and calls `std::remove(path.c_str())` in its destructor.

Representative complete duplicate/user test:

```cpp
TEST(DictTrieTest, ConstructorUserRowsOverrideInLastOccurrenceOrder) {
  TempFile main_dict("dat-main", "词 100 n\n甲 50 n\n");
  TempFile user_one("dat-user-one", "词 5 first\n单 first_single\n");
  TempFile user_two("dat-user-two", "词 10 last\n单 last_single\n");
  DictTrie trie(main_dict.path(), user_one.path() + ";" + user_two.path());

  RuneStrArray word;
  ASSERT_TRUE(DecodeUTF8RunesInString("词", word));
  std::string tag;
  ASSERT_TRUE(trie.FindTag(word.begin(), word.end(), &tag));
  EXPECT_EQ("last", tag);

  std::vector<Dag> dags;
  trie.BuildDag(word.begin(), word.end(), dags);
  ASSERT_EQ(1u, dags.size());
  ASSERT_EQ(1u, dags[0].edges.size());
  EXPECT_TRUE(dags[0].edges[0].in_dict);
  EXPECT_NEAR(std::log(10.0 / 150.0), dags[0].edges[0].weight, 1e-12);

  RuneStrArray single;
  ASSERT_TRUE(DecodeUTF8RunesInString("单", single));
  EXPECT_TRUE(trie.IsUserDictSingleRune(single[0].rune));
  ASSERT_TRUE(trie.FindTag(single.begin(), single.end(), &tag));
  EXPECT_EQ("last_single", tag);
}
```

Representative cache/stats test:

```cpp
TEST(DictTrieTest, SharesOnlyIdenticalConstructorInputs) {
  DictTrie first(DICT_FILE, TEST_DATA_DIR "/userdict.utf8",
                 DictTrie::WordWeightMedian);
  DictTrie second(DICT_FILE, TEST_DATA_DIR "/userdict.utf8",
                  DictTrie::WordWeightMedian);
  DictTrie different_option(DICT_FILE, TEST_DATA_DIR "/userdict.utf8",
                            DictTrie::WordWeightMax);
  EXPECT_EQ(first.GetDictionaryDataIdentity(), second.GetDictionaryDataIdentity());
  EXPECT_NE(first.GetDictionaryDataIdentity(),
            different_option.GetDictionaryDataIdentity());

  const DictionaryStats& stats = first.GetStats();
  EXPECT_GT(stats.slot_count, 0u);
  EXPECT_GT(stats.occupied_state_count, 0u);
  EXPECT_GT(stats.terminal_count, 0u);
  EXPECT_LE(stats.load_factor, 1.0);
  EXPECT_EQ(stats.slot_count *
                (sizeof(int32_t) * 2 + sizeof(uint8_t) +
                 sizeof(double) + sizeof(uint16_t)),
            stats.array_bytes);
}
```

- [ ] **Step 2: Build and run the new dictionary tests to observe RED**

Run:

```bash
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run --gtest_filter='DictTrieTest.*'
```

Expected: compilation fails because `Contains`, `BuildDag`, `FindTag`, `IsUserDictSingleRune`, `DictionaryStats`, and cache identity introspection are not yet defined.

- [ ] **Step 3: Add value DAG types beside legacy types for one migration commit**

Keep `MAX_WORD_LENGTH = 512` and the current `DictUnit`, `TrieNode`, `Trie`, `Dag::nexts`, `Dag::pInfo`, and `Dag::nextPos` unchanged for this one intermediate commit so the not-yet-migrated segmenters still compile. Add `DagEdge`, `Dag::edges`, and `Dag::next_pos` exactly as in “Proposed types and names”; initialize both the legacy and new fields in the constructor. New DAT code uses only `DagEdge`, while the existing node trie continues serving old consumers until Task 3. Task 4 removes all legacy types and members after every consumer has migrated.

- [ ] **Step 4: Define immutable dictionary data and the exact cache key**

In `DictTrie.hpp`, include `DoubleArrayTrie.hpp` and define the private records as follows:

```cpp
struct ParsedWord {
  Unicode word;
  double weight;
  std::string tag;
  bool from_user;
  size_t source_ordinal;
};

struct DictionaryData {
  DoubleArrayTrie trie;
  std::vector<std::string> tags;
  std::vector<Rune> user_single_runes;
  double freq_sum;
  double min_weight;
  double max_weight;
  double median_weight;
  double user_word_default_weight;
  DictionaryStats stats;
};

struct CacheKey {
  std::string main_path;
  std::vector<std::string> user_paths;
  UserWordWeightOption weight_option;
  bool operator==(const CacheKey& rhs) const;
};
```

`CacheKeyHash` must combine every path in order and the enum value. Add `std::shared_ptr<const DictionaryData> data_` beside the current node-trie fields for this intermediate task and initialize both representations in the constructor so the existing segmenters and the new tests compile together. Do not add pointers from the DAT into the legacy `DictUnit` storage. Task 3 removes the old fields and construction path; the final `DictTrie` stores only `data_`.

Use this double-checked cache flow so file I/O and DAT construction do not hold the global lock:

```cpp
static std::shared_ptr<const DictionaryData> GetDictionaryData(
    const CacheKey& key) {
  typedef std::unordered_map<CacheKey,
      std::weak_ptr<const DictionaryData>, CacheKeyHash> CacheMap;
  static CacheMap cache;
  static std::mutex cache_mutex;
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    CacheMap::iterator it = cache.find(key);
    if (it != cache.end()) {
      std::shared_ptr<const DictionaryData> live = it->second.lock();
      if (live) return live;
      cache.erase(it);
    }
  }
  std::shared_ptr<const DictionaryData> built = BuildDictionaryData(key);
  std::lock_guard<std::mutex> lock(cache_mutex);
  std::shared_ptr<const DictionaryData> live = cache[key].lock();
  if (live) return live;
  cache[key] = built;
  return built;
}
```

Declare a `CacheMap` typedef so the iterator compiles in C++11. The only writes after publication are to the cache map itself under its mutex.

- [ ] **Step 5: Parse, weight, stable-sort, collapse, intern, and build**

Split the constructor user path string on both `|` and `;`, preserving non-empty path order in `CacheKey::user_paths`. Parse all main rows first and retain their original positive finite frequencies. Accumulate with a checked next value, reject aggregate overflow, then compute every weight without dividing first:

```cpp
double freq_sum = 0.0;
for (size_t i = 0; i < main_frequencies.size(); ++i) {
  const double next_sum = freq_sum + main_frequencies[i];
  XCHECK(std::isfinite(next_sum) && next_sum > 0.0)
      << "aggregate frequency must be finite and greater than zero";
  freq_sum = next_sum;
}
const double log_freq_sum = std::log(freq_sum);
const auto calculate_log_weight = [log_freq_sum](double frequency) {
  const double weight = std::log(frequency) - log_freq_sum;
  XCHECK(std::isfinite(weight)) << "dictionary weight must be finite";
  return weight;
};
```

Call `calculate_log_weight` for every main and explicit user frequency. Compute the sorted main log-weight vector and select `front()`, `[size / 2]`, or `back()` exactly as the existing implementation does. Parse each user file in path order; rows without a frequency receive the selected finite default log weight. This remains mathematically equivalent to the existing weighting while keeping `DBL_MIN / DBL_MAX` from becoming zero before `log`.

Assign monotonically increasing `source_ordinal` values across main then user records. Reuse the sole comparator from `DoubleArrayTrie.hpp` for stable sorting and equality; do not use relational operators on `Unicode`/`LocalVector`:

```cpp
std::stable_sort(parsed_words.begin(), parsed_words.end(),
    [](const ParsedWord& lhs, const ParsedWord& rhs) {
      return RuneLexicographicLess(lhs.word, rhs.word);
    });
const bool same_word =
    !RuneLexicographicLess(previous.word, current.word) &&
    !RuneLexicographicLess(current.word, previous.word);
```

Collapse equal words by selecting the greatest `source_ordinal`; therefore any user record overrides the main record and the last user occurrence wins across and within files.

Initialize `tags` with one empty string. Intern only tags surviving duplicate collapse; reuse an existing ID for duplicate payloads. Create one `DoubleArrayTrie::BuildEntry` per collapsed word. Add each single-Rune winning user word to `user_single_runes`, then sort and unique it. Build the DAT, copy its five-array stats, compute `tag_bytes`, and release construction-only storage before publishing:

```cpp
std::vector<ParsedWord>().swap(parsed_words);
std::vector<DoubleArrayTrie::BuildEntry>().swap(build_entries);
std::vector<double>().swap(sorted_weights);
```

These swaps occur after `DoubleArrayTrie::Build` and before conversion to `shared_ptr<const DictionaryData>` so peak parsing allocations do not remain in cached objects.

- [ ] **Step 6: Add exact, tag, DAG, membership, stats, and identity methods**

`Contains` delegates to `DoubleArrayTrie::ExactMatch`. `Find(std::string)` decodes and returns false with an error log when decoding fails. `FindTag` exact-matches to a local `tag_id`, verifies it is below `data_->tags.size()`, assigns `*tag` when non-null, and returns the exact-match boolean.

`BuildDag` creates one `Dag` per input Rune. For each start position, first append fallback `DagEdge{start, data_->min_weight, false}`. Prefix-search from that position with the requested maximum length. Replace the fallback when the first match has length one; append every longer match. This produces one shortest-first edge for a known single Rune, or one non-dictionary fallback edge otherwise:

```cpp
result.assign(end - begin, Dag());
for (size_t start = 0; start < result.size(); ++start) {
  result[start].runestr = *(begin + start);
  result[start].edges.push_back(DagEdge{start, data_->min_weight, false});
  std::vector<DoubleArrayTrie::Match> matches;
  data_->trie.CommonPrefixSearch(begin + start, end, max_word_len, &matches);
  for (size_t i = 0; i < matches.size(); ++i) {
    DagEdge edge = {start + matches[i].length - 1,
                    matches[i].weight, true};
    if (matches[i].length == 1) result[start].edges[0] = edge;
    else result[start].edges.push_back(edge);
  }
}
```

`IsUserDictSingleRune` is exactly a `std::binary_search` over the immutable sorted vector. `GetStats` returns `data_->stats`; `GetDictionaryDataIdentity` returns `data_.get()`; `GetMinWeight` returns `data_->min_weight`.

- [ ] **Step 7: Run the focused dictionary tests to observe GREEN**

Run:

```bash
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run \
  --gtest_filter='DoubleArrayTrieTest.*:DictTrieTest.*'
```

Expected: all core and dictionary tests pass. Confirm the duplicate test returns tag `last`, its DAG edge weight is `log(10/150)`, and identical cache keys expose the same identity.

- [ ] **Step 8: Commit and immediately push Task 2**

```bash
git add include/cppjieba/Trie.hpp include/cppjieba/DictTrie.hpp \
        test/unittest/trie_test.cpp
git commit -m "feat: build immutable cached dictionaries"
git push origin feat/static-rune-dat
```

Record the release assessment as unchanged: major release candidate, with publication still blocked on Task 5.

### Task 3: Migrate segmenters and freeze runtime dictionary mutation

**Files:**

- Modify: `include/cppjieba/MPSegment.hpp`
- Modify: `include/cppjieba/FullSegment.hpp`
- Modify: `include/cppjieba/QuerySegment.hpp`
- Modify: `include/cppjieba/PosTagger.hpp`
- Modify: `include/cppjieba/MixSegment.hpp`
- Modify: `include/cppjieba/Jieba.hpp`
- Modify: `include/cppjieba/DictTrie.hpp`
- Modify: `test/unittest/segments_test.cpp`
- Modify: `test/unittest/pos_tagger_test.cpp`
- Modify: `test/unittest/jieba_test.cpp`
- Verify unchanged golden coverage: `test/unittest/keyword_extractor_test.cpp`

- [ ] **Step 1: Add segmentation-route and frozen-runtime tests before migrating consumers**

In `segments_test.cpp`, retain every existing MP/Mix/Full/Query expected string and add a constructor-user single-Rune case proving Mix does not pass that Rune to HMM. Add a maximum-length case that expects a four-Rune dictionary word to be excluded at limit three and included at limit four.

In `jieba_test.cpp`, replace the old successful runtime insertion expectations with the exact frozen behavior:

```cpp
TEST(JiebaTest, RuntimeDictionaryMutationIsRejected) {
  Jieba jieba(DICT_DIR "/jieba.dict.utf8",
              DICT_DIR "/hmm_model.utf8",
              DICT_DIR "/user.dict.utf8",
              DICT_DIR "/idf.utf8",
              DICT_DIR "/stop_words.utf8");
  EXPECT_FALSE(jieba.Find("静态运行时新词"));

  testing::internal::CaptureStderr();
  EXPECT_FALSE(jieba.InsertUserWord("静态运行时新词"));
  EXPECT_FALSE(jieba.InsertUserWord("静态运行时新词", 100, "nz"));
  EXPECT_FALSE(jieba.DeleteUserWord("云计算"));
  std::vector<std::string> vector_rows(1, "静态运行时新词 nz");
  std::set<std::string> set_rows(vector_rows.begin(), vector_rows.end());
  jieba.LoadUserDict(vector_rows);
  jieba.LoadUserDict(set_rows);
  jieba.LoadUserDict(TEST_DATA_DIR "/userdict.2.utf8");
  const std::string logs = testing::internal::GetCapturedStderr();

  EXPECT_NE(std::string::npos, logs.find("static dictionary is immutable"));
  EXPECT_FALSE(jieba.Find("静态运行时新词"));
  EXPECT_TRUE(jieba.Find("云计算"));
}
```

Keep the existing constructor-time user dictionary tests and all `Jieba::Cut`, `Tag`, `LookupTag`, and `Find` goldens. `keyword_extractor_test.cpp` needs no edit because its existing expected values exercise behavior through `MixSegment`; it must be included in every focused run below.

- [ ] **Step 2: Run migrated-test filters to observe RED**

Run:

```bash
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run \
  --gtest_filter='MPSegmentTest.*:MixSegmentTest.*:FullSegment.*:QuerySegment.*:PosTagger*:JiebaTest.*:KeywordExtractorTest.*'
```

Expected: the new runtime mutation test fails because insertion and deletion still succeed and post-construction loads still alter legacy state. Preserve that behavioral failure before installing immutable stubs; the pre-existing segmenter goldens continue compiling through the temporary legacy path from Task 2.

- [ ] **Step 3: Rework MP dynamic programming and route reconstruction**

Change `MPSegment::Cut` to call `dictTrie_->BuildDag`. Replace `CalcDP` with shortest-first value-edge logic:

```cpp
for (std::vector<Dag>::reverse_iterator it = dags.rbegin();
     it != dags.rend(); ++it) {
  it->weight = MIN_DOUBLE;
  it->next_pos = 0;
  for (LocalVector<DagEdge>::const_iterator edge = it->edges.begin();
       edge != it->edges.end(); ++edge) {
    assert(std::isfinite(edge->weight));
    double candidate = edge->weight;
    if (edge->end + 1 < dags.size()) candidate += dags[edge->end + 1].weight;
    assert(std::isfinite(candidate));
    if (candidate > it->weight) {
      it->weight = candidate;
      it->next_pos = edge->end;
    }
  }
}
```

Add `<cmath>` to `MPSegment.hpp`. Dictionary construction and `DoubleArrayTrie::Build` reject every non-finite payload, fallback edges use the validated finite minimum weight, and the assertions above enforce that DP receives and produces only finite values. Do not use `>=`: because `BuildDag` orders edges shortest first, strict `>` preserves the first/shortest edge on a tie. Reconstruct with `WordRange(begin + pos, begin + dags[pos].next_pos)` and then `pos = dags[pos].next_pos + 1`; no word pointer or word-length lookup remains.

Rename the pass-through membership method to `IsUserDictSingleRune` and delegate to `DictTrie::IsUserDictSingleRune`.

- [ ] **Step 4: Reproduce Full, Query, Mix, and POS behavior on value APIs**

In `FullSegment`, iterate `dags[start].edges`. Compute `word_len = edge.end - start + 1`. Preserve the old inclusion conditions exactly:

```cpp
const bool only_edge = dags[start].edges.size() == 1;
if (edge.in_dict) {
  if (word_len >= 2 || (only_edge && max_end_exclusive <= start)) {
    res.push_back(WordRange(begin + start, begin + edge.end));
  }
} else if (only_edge && max_end_exclusive <= start) {
  res.push_back(WordRange(begin + start, begin + edge.end));
}
max_end_exclusive = std::max(max_end_exclusive, start + word_len);
```

Initialize `max_end_exclusive = 0`. This preserves inclusion of an uncovered single fallback, exclusion of redundant single terminals, and inclusion of every dictionary edge of length at least two.

In `QuerySegment`, replace both pointer comparisons with `trie_->Contains(wr.left, wr.right + 1)`. In `MixSegment`, replace both calls to `IsUserDictSingleChineseWord` with `IsUserDictSingleRune`. In `PosTagger::LookupTag`, use:

```cpp
std::string tag;
if (!dict->FindTag(runes.begin(), runes.end(), &tag) || tag.empty()) {
  return SpecialRule(runes);
}
return tag;
```

Do not change HMM handoff, separator handling, word offsets, or KeywordExtractor code.

- [ ] **Step 5: Preserve runtime signatures as immutable error stubs**

In `DictTrie`, keep the existing signatures and default arguments. Each bool mutation logs the same stable phrase and returns false:

```cpp
bool InsertUserWord(const std::string&, const std::string& = UNKNOWN_TAG) {
  XLOG(ERROR) << "static dictionary is immutable; pass user words to the constructor";
  return false;
}

bool InsertUserWord(const std::string&, int,
                    const std::string& = UNKNOWN_TAG) {
  XLOG(ERROR) << "static dictionary is immutable; pass user words to the constructor";
  return false;
}

bool DeleteUserWord(const std::string&,
                    const std::string& = UNKNOWN_TAG) {
  XLOG(ERROR) << "static dictionary is immutable; pass user words to the constructor";
  return false;
}
```

The `vector<string>`, `set<string>`, and path-string `LoadUserDict` overloads each log that phrase and return without changing `data_`. `Jieba.hpp` retains its current method signatures and continues delegating to `dict_trie_`, so public callers receive the new false/no-op behavior and one error log per call. Constructor initialization must call the private builder directly and must never call these public no-op overloads.

After all consumers use `Contains`, `BuildDag`, `FindTag`, and `IsUserDictSingleRune`, remove the legacy `DictTrie` node/unit fields, pointer-returning exact lookup, pointer-DAG builder, and second constructor build. Leave the now-unreferenced node-trie declarations and legacy `Dag` members in `Trie.hpp` for the dedicated cleanup in Task 4; no runtime object constructs them after this step.

- [ ] **Step 6: Run all behavior goldens to observe GREEN**

Run:

```bash
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run \
  --gtest_filter='DoubleArrayTrieTest.*:DictTrieTest.*:MPSegmentTest.*:MixSegmentTest.*:FullSegment.*:QuerySegment.*:PosTagger*:JiebaTest.*:KeywordExtractorTest.*'
```

Expected: every selected test passes. Specifically confirm existing MP, Mix with/without HMM, Full, Query, POS, Jieba word/offset, and KeywordExtractor expected strings are byte-for-byte unchanged; only the runtime mutation expectations change.

- [ ] **Step 7: Commit and immediately push Task 3**

```bash
git add include/cppjieba/MPSegment.hpp include/cppjieba/FullSegment.hpp \
        include/cppjieba/QuerySegment.hpp include/cppjieba/PosTagger.hpp \
        include/cppjieba/MixSegment.hpp include/cppjieba/Jieba.hpp \
        include/cppjieba/DictTrie.hpp test/unittest/segments_test.cpp \
        test/unittest/pos_tagger_test.cpp test/unittest/jieba_test.cpp
git commit -m "feat: migrate segmenters to static dictionary APIs"
git push origin feat/static-rune-dat
```

Record the release assessment as major because runtime mutators now reject calls and low-level DAG APIs are value-based.

### Task 4: Harden malformed/overflow/concurrency/stats coverage and remove the node trie

**Files:**

- Modify: `include/cppjieba/DoubleArrayTrie.hpp`
- Modify: `include/cppjieba/DictTrie.hpp`
- Modify: `include/cppjieba/Trie.hpp`
- Modify: `test/unittest/double_array_trie_test.cpp`
- Modify: `test/unittest/trie_test.cpp`
- Modify: `test/unittest/CMakeLists.txt`

- [ ] **Step 1: Add strict parser, overflow, max-length, stats, and concurrent-read tests first**

Use `ASSERT_DEATH_IF_SUPPORTED` with stable diagnostic substrings for malformed dictionary construction. Generate temporary inputs as exact bytes so no persistent fixtures are needed:

```cpp
TEST(DictTrieDeathTest, RejectsMalformedMainAndUserRows) {
  TempFile bad_columns("bad-columns", "词 1 n extra\n");
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(bad_columns.path()); }, "expected exactly 3 columns");

  TempFile bad_frequency("bad-frequency", "词 nan n\n");
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(bad_frequency.path()); },
      "frequency must be finite and greater than zero");

  TempFile valid_main("valid-main", "词 1 n\n");
  TempFile bad_user("bad-user", "用户词 0 nz\n");
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(valid_main.path(), bad_user.path()); },
      "frequency must be finite and greater than zero");
  TempFile bad_user_columns("bad-user-columns", "用户词 1 nz extra\n");
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(valid_main.path(), bad_user_columns.path()); },
      "expected 1, 2, or 3 columns");
}

TEST(DictTrieDeathTest, RejectsInvalidUtf8AndEmptyEffectiveDictionary) {
  TempFile invalid_utf8("bad-utf8", std::string("\xFF 1 n\n", 6));
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(invalid_utf8.path()); }, "invalid UTF-8");
  TempFile empty("empty-main", "");
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(empty.path()); }, "effective dictionary is empty");
}

TEST(DictTrieDeathTest, RejectsAggregateFrequencyOverflow) {
  TempFile overflow("frequency-overflow",
      "large_a 1.7976931348623157e308 n\n"
      "large_b 1.7976931348623157e308 n\n");
  ASSERT_DEATH_IF_SUPPORTED(
      { DictTrie trie(overflow.path()); },
      "aggregate frequency must be finite and greater than zero");
}

TEST(DictTrieTest, ExtremeFrequencyRatioKeepsFiniteLogWeight) {
  TempFile extreme("extreme-ratio",
      "tiny 2.2250738585072014e-308 n\n"
      "large 1.7976931348623157e308 n\n");
  DictTrie trie(extreme.path());
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("tiny", runes));
  std::vector<Dag> dags;
  trie.BuildDag(runes.begin(), runes.end(), dags);
  ASSERT_FALSE(dags.empty());
  ASSERT_FALSE(dags[0].edges.empty());
  const DagEdge& edge = dags[0].edges.back();
  ASSERT_TRUE(edge.in_dict);

  const double tiny = std::numeric_limits<double>::min();
  const double large = std::numeric_limits<double>::max();
  EXPECT_EQ(0.0, tiny / large);
  const double expected = std::log(tiny) - std::log(large);
  EXPECT_TRUE(std::isfinite(expected));
  EXPECT_TRUE(std::isfinite(edge.weight));
  EXPECT_NEAR(expected, edge.weight, 1e-12);
}
```

Add table-driven cases for main and three-column user rows containing frequency tokens `0`, `-1`, `nan`, `inf`, and `1x`. Add a main row missing its frequency, a four-column user row, a blank main row, a blank user row, an invalid continuation sequence, an overlong encoding, a UTF-16 surrogate encoding, and a code point above U+10FFFF.

Generate 65,536 unique words with 65,536 unique non-empty tags in one death-test child and expect `more than 65535 non-empty tags`. Generate 65,535 unique tagged words in a non-death test and require successful construction, `terminal_count == 65535`, and a successful `FindTag` for the last word/tag; this exercises the accepted boundary through the production builder.

Add deterministic DAT address-limit coverage:

```cpp
TEST(DoubleArrayTrieTest, RejectsAddressBeyondConfiguredLimit) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("a", -2.0, 1));
  entries.push_back(Entry("😀", -1.0, 2));
  DoubleArrayTrie trie;
  std::string error;
  EXPECT_FALSE(trie.Build(entries, &error,
                          DoubleArrayTrie::BuildOptions(32)));
  EXPECT_NE(std::string::npos, error.find("address"));
}

TEST(DoubleArrayTrieTest, RejectsNonFinitePayloadWeight) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("a", std::numeric_limits<double>::infinity(), 1));
  DoubleArrayTrie trie;
  std::string error;
  EXPECT_FALSE(trie.Build(entries, &error));
  EXPECT_NE(std::string::npos, error.find("finite weight"));
}
```

Add a 513-Rune generated dictionary word and assert `BuildDag(..., 512)` omits it while `BuildDag(..., 513)` includes an edge ending at 512. Add exact stats assertions: terminal count equals collapsed unique word count, every array byte is included in `array_bytes`, `tag_bytes` counts each unique payload once, `occupied_state_count <= slot_count`, and `DoubleArrayTrie::last_slot_occupied()` is true after trimming.

Add concurrent construction plus querying:

```cpp
TEST(DictTrieTest, CacheAndQueriesAreConcurrentReadOnlySafe) {
  const size_t kThreads = 8;
  std::vector<std::shared_ptr<DictTrie> > tries(kThreads);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < kThreads; ++i) {
    threads.push_back(std::thread([&, i]() {
      tries[i].reset(new DictTrie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8"));
    }));
  }
  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
  for (size_t i = 1; i < tries.size(); ++i) {
    EXPECT_EQ(tries[0]->GetDictionaryDataIdentity(),
              tries[i]->GetDictionaryDataIdentity());
  }

  std::atomic<bool> ok(true);
  threads.clear();
  for (size_t i = 0; i < kThreads; ++i) {
    threads.push_back(std::thread([&, i]() {
      for (size_t n = 0; n < 1000; ++n) {
        if (!tries[i]->Find("清华大学") || tries[i]->Find("不存在词条")) {
          ok.store(false);
        }
      }
    }));
  }
  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
  EXPECT_TRUE(ok.load());
}
```

Include `<atomic>`, `<cmath>`, `<limits>`, `<memory>`, and `<thread>`. Add `find_package(Threads REQUIRED)` and link `Threads::Threads` to `test.run` in `test/unittest/CMakeLists.txt`.

- [ ] **Step 2: Run the hardening tests to observe RED**

Run:

```bash
cmake -S . -B build-static-rune-dat -DCMAKE_BUILD_TYPE=Release
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run \
  --gtest_filter='DoubleArrayTrieTest.*:DictTrieTest.*:DictTrieDeathTest.*'
```

Expected: malformed UTF-8/frequency/row tests fail or fail with the wrong diagnostic until strict validators are added. The aggregate-overflow test must expose the old infinite sum, while the extreme-ratio test must expose `log(0)` from divide-before-log weighting. Any cache race, root self-loop, non-finite core payload, untrimmed tail, incorrect byte count, tag-boundary, maximum-length, or address-bound error must also remain visible in this RED run.

- [ ] **Step 3: Add strict row/frequency/UTF-8 validation on the dictionary path**

Tokenize with `std::istringstream` so repeated ASCII whitespace is accepted but missing or extra columns are rejected. Parse frequency with `std::strtod`, reset/check `errno`, require the end pointer to reach the token terminator, require `std::isfinite(value)`, and require `value > 0.0` before summing or taking a logarithm. For each main frequency, compute `next_sum = freq_sum + value` and reject unless `next_sum` is finite and positive; validate the final aggregate again before `std::log(freq_sum)`. Calculate main and explicit user weights only as `std::log(value) - std::log(freq_sum)`, reject any non-finite result, and never evaluate `value / freq_sum`.

Dictionary words require a strict UTF-8 validator before the existing Rune decoder. Accept ASCII; accept leading bytes `C2..DF`, `E0..EF`, and `F0..F4` only with the required continuation bytes; enforce `E0` second byte `A0..BF`, `ED` second byte `80..9F`, `F0` second byte `90..BF`, and `F4` second byte `80..8F`. Reject truncated sequences, stray continuations, overlong forms, surrogates, code points above U+10FFFF, and empty decoded words. Include file path and one-based line number in every construction diagnostic.

Reject a zero-row main file as `effective dictionary is empty`. After duplicate collapse, reject zero build entries with the same diagnostic. When adding a new non-empty tag, allow `tags.size() <= UINT16_MAX` before assigning the next ID; if `tags.size() > UINT16_MAX`, reject with `more than 65535 non-empty tags`. This permits IDs `1..65535` and reserves ID `0` for no tag.

- [ ] **Step 4: Audit every DAT address and trim invariant**

Create the root directly at slot `0`, then route every child placement, sibling candidate, recursion, exact lookup, and prefix lookup through `CheckedTransitionAddress`, which rejects `address <= 0`. Never pass root creation through this helper and never resize from an unchecked address. On any placement failure, leave a useful error string and return false; `BuildDictionaryData` wraps it with `XCHECK` and the `DAT build failed` prefix. The root-self-loop regression must fail exact and prefix lookup for the prepended phantom Rune even though `check_[0] == 0` and `base_[0] < 0`.

After trimming, assert in debug builds that all five array sizes match and that either only the root exists or `check_.back() != -1`. Recompute stats only after trimming. Confirm the configured-limit test fails before allocation and that the normal sparse-code-point test still uses a negative root base.

- [ ] **Step 5: Remove all node-trie remnants**

Delete `TrieKey`, `TrieNode`, `Trie`, recursive node deletion, insert/delete methods, `DictUnit`, and pointer-based `Dag` members from `include/cppjieba/Trie.hpp`. Remove `<queue>` and any unordered-container include that existed only for node traversal. Verify no production or test source mentions the removed symbols:

```bash
rg -n 'TrieNode|DictUnit|ptValue|pInfo|\.nexts|InsertNode|DeleteNode' \
  include test
```

Expected: no matches. `Trie.hpp` contains only the include guard, required value-type includes, `MAX_WORD_LENGTH`, `DagEdge`, and `Dag`.

- [ ] **Step 6: Run focused and complete unit tests to observe GREEN**

Run:

```bash
cmake --build build-static-rune-dat --target test.run -j2
./build-static-rune-dat/test/test.run \
  --gtest_filter='DoubleArrayTrieTest.*:DictTrieTest.*:DictTrieDeathTest.*'
./build-static-rune-dat/test/test.run
```

Expected: the focused hardening tests pass, followed by the entire GoogleTest binary with zero failed tests. Death-test children must identify the intended validation failure rather than abort from an unrelated assertion.

- [ ] **Step 7: Commit and immediately push Task 4**

```bash
git add include/cppjieba/DoubleArrayTrie.hpp include/cppjieba/DictTrie.hpp \
        include/cppjieba/Trie.hpp test/unittest/double_array_trie_test.cpp \
        test/unittest/trie_test.cpp test/unittest/CMakeLists.txt
git commit -m "test: harden static dictionary invariants"
git push origin feat/static-rune-dat
```

Record the release assessment as major and still gated on measured RSS reduction.

### Task 5: Full CMake/Bazel verification, benchmark comparison, and release gate

**Files:**

- Modify: `test/benchmark.cpp`
- Verify build integration: `CMakeLists.txt`
- Verify build integration: `test/CMakeLists.txt`
- Verify header packaging/smoke: `BUILD.bazel`
- Verify module release version: `MODULE.bazel`

- [ ] **Step 1: Add benchmark stats output before changing implementation details**

Add a formatter and emit one `DATStats` line immediately after `DictTrieLoad`. Keep the existing metric name `DictTrieLoad` so the new DAT build is directly comparable to the baseline:

```cpp
std::string FormatDATStats(const DictionaryStats& stats) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6)
      << "slots=" << stats.slot_count
      << " occupied=" << stats.occupied_state_count
      << " terminals=" << stats.terminal_count
      << " load_factor=" << stats.load_factor
      << " array_bytes=" << stats.array_bytes
      << " tag_bytes=" << stats.tag_bytes;
  return oss.str();
}

// Immediately after PrintMetric("DictTrieLoad", ...):
std::cout << "BENCH DATStats " << FormatDATStats(dict_trie->GetStats())
          << std::endl;
```

The benchmark output must therefore report DAT stats, DAT construction as `DictTrieLoad`, `MPCut`, `MixCut`, and `DictFind` in every run.

- [ ] **Step 2: Build and run the benchmark smoke once**

Run:

```bash
cmake --build build-static-rune-dat --target benchmark_bin -j2
./build-static-rune-dat/benchmark_bin
```

Expected: exactly one line for each of `DictTrieLoad`, `DATStats`, `HMMModelLoad`, `MPCut`, `MixCut`, and `DictFind`; `DATStats` values are nonzero except that `tag_bytes` may be zero only for an all-untagged synthetic dictionary, not for the repository main dictionary.

- [ ] **Step 3: Run clean Release CMake verification**

Use a new build directory so earlier RED/GREEN artifacts cannot mask missing includes:

```bash
cmake -S . -B build-static-rune-dat-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=11
cmake --build build-static-rune-dat-release -j2
ctest --test-dir build-static-rune-dat-release --output-on-failure
```

Expected: configuration/build exit zero and CTest reports `100% tests passed, 0 tests failed out of 2` for `./test/test.run` and `./load_test`.

- [ ] **Step 4: Run the Bazel smoke test**

Run:

```bash
bazel test //:bazel_smoke_test
```

Expected: `//:bazel_smoke_test PASSED` and `Executed 1 out of 1 test`. Because `BUILD.bazel` already glob-includes `include/cppjieba/**/*.hpp`, no explicit header list change should be necessary; edit it only if the smoke test demonstrates a real packaging dependency.

- [ ] **Step 5: Run the Release benchmark five times and calculate medians**

Run from the repository root:

```bash
mkdir -p /tmp/cppjieba-static-rune-dat-bench
for run in 1 2 3 4 5; do
  ./build-static-rune-dat-release/benchmark_bin \
    | tee "/tmp/cppjieba-static-rune-dat-bench/run-${run}.txt"
done
python3 - <<'PY'
import glob
import re
import statistics

metrics = {name: [] for name in
           ("DictTrieLoad", "DictTrieRSSDelta", "MPCut", "MixCut", "DictFind")}
stats_lines = []
for path in sorted(glob.glob("/tmp/cppjieba-static-rune-dat-bench/run-*.txt")):
    text = open(path, encoding="utf-8").read()
    stats_lines.append(re.search(r"^BENCH DATStats .+$", text, re.M).group(0))
    for name in ("DictTrieLoad", "MPCut", "MixCut", "DictFind"):
        metrics[name].append(float(re.search(
            rf"^BENCH {name} ms=([0-9.]+)", text, re.M).group(1)))
    metrics["DictTrieRSSDelta"].append(float(re.search(
        r"^BENCH DictTrieLoad .* rss_delta_mb=([-0-9.]+)", text, re.M).group(1)))
for name, values in metrics.items():
    print(name, "values=", ",".join(f"{v:.3f}" for v in values),
          "median=", f"{statistics.median(values):.3f}")
print("DATStats")
print("\n".join(stats_lines))
PY
```

Expected: five values and one median for every comparison metric, plus five stable-shape `DATStats` lines. Investigate any differing slot/occupied/terminal/byte counts across identical runs because immutable builds must be deterministic.

Use this freshly measured baseline from commit `103e1a2` in the comparison:

| Metric | Baseline values (5 Release runs) | Baseline median |
|---|---|---:|
| `DictTrieLoad` | 198.336, 194.991, 172.617, 183.098, 176.056 ms | 183.098 ms |
| `DictTrie` RSS delta | 119.859, 119.828, 119.859, 119.859, 119.859 MiB | 119.859 MiB |
| `MPCut` | 3914.471, 3782.832, 3862.261, 3738.663, 3770.423 ms | 3782.832 ms |
| `MixCut` | 5076.716, 4771.695, 5114.093, 4975.172, 4952.506 ms | 4975.172 ms |
| `DictFind` | 44.162, 44.516, 46.604, 43.247, 44.414 ms | 44.414 ms |

Baseline verification was CTest `2/2` passed and Bazel `//:bazel_smoke_test` `1/1` passed.

- [ ] **Step 6: Apply the RSS release gate and diagnose before optimization**

Call the RSS decrease clear only when the new median is at least 1.000 MiB below 119.859 MiB and every new run is below the baseline minimum 119.828 MiB. Report timing medians and percentage changes even though they are not the memory release gate:

```text
percent_change = (new_median - baseline_median) / baseline_median * 100
```

If the RSS criterion is not met, do not publish a release. First inspect, in this order:

1. Construction-only `ParsedWord`, `BuildEntry`, and main-frequency vectors are swapped empty before `DictionaryData` publication and are not members of the cached object.
2. `DATStats.slot_count - occupied_state_count`, `load_factor`, and the final occupied-slot invariant show whether trailing holes were trimmed.
3. The sibling placement loop begins at the tracked first free slot, permits negative bases, checks the complete sibling span before claiming it, and does not monotonically append every branch at the tail.

Re-run the five measurements after any correction. Keep the no-release decision if the rerun still lacks a clear RSS decrease.

- [ ] **Step 7: Commit benchmark reporting and immediately push the final code commit**

```bash
git add test/benchmark.cpp
git commit -m "bench: report static dictionary stats"
git push origin feat/static-rune-dat
```

In the handoff or pull-request body, report all five new values, their medians, percentage changes, the five `DATStats` lines, CTest result, and Bazel result. State that the implementation is a major-version candidate because dynamic dictionary semantics and low-level APIs changed.

- [ ] **Step 8: Assess and, only after authorization and merge, publish the major release**

If all verification passes and the RSS gate is met, recommend `v6.0.0`; do not publish from the feature branch. After the change is merged and the maintainer authorizes publication, update `MODULE.bazel` from `5.6.7` to `6.0.0`, run the complete Release CTest and Bazel commands again, commit and push the version change, then publish the tag/release through the repository’s normal release process.

If a `v6.0.0` release is actually published, immediately create upgrade recommendation issues with the release URL in all three downstream projects:

```bash
gh issue create --repo yanyiwu/nodejieba \
  --title "Update cppjieba to v6.0.0" \
  --body "cppjieba v6.0.0 replaces the mutable node trie with the static Rune DAT and changes dynamic dictionary semantics. Please update the bundled cppjieba and verify constructor-time user dictionaries: https://github.com/yanyiwu/cppjieba/releases/tag/v6.0.0"
gh issue create --repo yanyiwu/gojieba \
  --title "Update cppjieba to v6.0.0" \
  --body "cppjieba v6.0.0 replaces the mutable node trie with the static Rune DAT and changes dynamic dictionary semantics. Please update the bundled cppjieba and verify constructor-time user dictionaries: https://github.com/yanyiwu/cppjieba/releases/tag/v6.0.0"
gh issue create --repo yanyiwu/simhash \
  --title "Update cppjieba to v6.0.0" \
  --body "cppjieba v6.0.0 replaces the mutable node trie with the static Rune DAT and changes dynamic dictionary semantics. Please update the bundled cppjieba and verify constructor-time user dictionaries: https://github.com/yanyiwu/cppjieba/releases/tag/v6.0.0"
```

If no release is published, create no downstream issues. Once this repository has no other active task, inspect open cppjieba issues and pull requests and address only items whose scope and authority are clear.

## Final implementation evidence checklist

- [ ] The new DAT is repository-owned, C++11, header-only, raw-Rune, five-array, static, and free of every excluded representation or persistence feature.
- [ ] All build and traversal addresses use signed 64-bit intermediates; slot `0` remains root-only, and failures occur before non-positive/unrepresentable transition allocation or casts.
- [ ] `RuneLexicographicLess` is the only word-order comparator and uses iterator-based `std::lexicographical_compare` for `LocalVector` in core validation and dictionary sorting/deduplication.
- [ ] Main plus constructor user dictionaries have finite positive aggregate frequency, finite `log(freq) - log(freq_sum)` weights, stable Rune sorting, deterministic duplicate precedence, tag interning, and sorted unique user single Runes.
- [ ] Cache identity includes main path, ordered user paths, and weight option; shared data is immutable and the cache stores weak references under a mutex.
- [ ] `DagEdge` has only `end`, `weight`, and `in_dict`; MP strict-tie, Full inclusion, Query exact lookup, POS tag lookup, and Mix single-Rune behavior match their specified algorithms.
- [ ] Runtime mutation signatures remain callable but reject/no-op with `ERROR`; constructor-time user dictionaries still affect Cut and Tag.
- [ ] Prefix, root-self-loop, sparse Rune, collision, missing transition, duplicate, maximum length, user single Rune, tag, malformed row, UTF-8, individual/aggregate frequency, ratio underflow, empty dictionary, tag/address boundary, stats, cache, and concurrent reads have explicit tests.
- [ ] Existing MP, Mix, Full, Query, POS, Jieba, and KeywordExtractor goldens pass unchanged.
- [ ] Fresh Release CTest and Bazel smoke pass; five-run DAT stats/build/cut/find evidence is reported against the recorded baseline.
- [ ] A release is withheld unless RSS clearly decreases; a published major release is followed by nodejieba, gojieba, and simhash upgrade issues.
