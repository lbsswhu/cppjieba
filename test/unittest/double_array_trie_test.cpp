#include <limits>
#include <string>
#include <vector>

#include "cppjieba/DoubleArrayTrie.hpp"
#include "cppjieba/Unicode.hpp"
#include "gtest/gtest.h"

namespace cppjieba {
namespace {

Unicode DecodeWord(const std::string& word) {
  Unicode decoded;
  EXPECT_TRUE(DecodeUTF8RunesInString(word, decoded));
  return decoded;
}

RuneStrArray DecodeText(const std::string& text) {
  RuneStrArray decoded;
  EXPECT_TRUE(DecodeUTF8RunesInString(text, decoded));
  return decoded;
}

DoubleArrayTrie::BuildEntry Entry(const std::string& word,
                                  double weight,
                                  uint16_t tag_id) {
  DoubleArrayTrie::BuildEntry entry;
  entry.word = DecodeWord(word);
  entry.weight = weight;
  entry.tag_id = tag_id;
  return entry;
}

TEST(DoubleArrayTrieTest, FindsPrefixesInShortestFirstOrder) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("你", -3.0, 1));
  entries.push_back(Entry("你好", -2.0, 2));
  entries.push_back(Entry("你好啊", -1.0, 3));

  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(entries, &error)) << error;

  RuneStrArray text = DecodeText("你好啊呀");
  std::vector<DoubleArrayTrie::Match> matches;
  trie.CommonPrefixSearch(text.begin(), text.end(), text.size(), &matches);

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

  RuneStrArray exact = DecodeText("😀b");
  double weight = 0.0;
  uint16_t tag_id = 0;
  EXPECT_TRUE(trie.ExactMatch(exact.begin(), exact.end(), &weight, &tag_id));
  EXPECT_DOUBLE_EQ(-1.0, weight);
  EXPECT_EQ(2u, tag_id);

  RuneStrArray cross_branch = DecodeText("一😀");
  EXPECT_FALSE(trie.ExactMatch(cross_branch.begin(), cross_branch.end(),
                               &weight, &tag_id));
  EXPECT_GT(trie.stats().occupied_state_count, trie.stats().terminal_count);
  EXPECT_TRUE(trie.last_slot_occupied());
}

TEST(DoubleArrayTrieTest, RootSlotCannotBeReachedAsATransition) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("一a", -2.0, 1));
  entries.push_back(Entry("😀b", -1.0, 2));

  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(entries, &error)) << error;
  ASSERT_LT(trie.base_at(0), 0);
  const Rune phantom = static_cast<Rune>(-trie.base_at(0));

  RuneStrArray present = DecodeText("一a");
  RuneStrArray phantom_then_present;
  phantom_then_present.push_back(RuneStr(phantom, 0, 1));
  phantom_then_present.insert(phantom_then_present.end(),
                              present.begin(), present.end());

  double weight = 0.0;
  uint16_t tag_id = 0;
  EXPECT_FALSE(trie.ExactMatch(phantom_then_present.begin(),
                               phantom_then_present.end(), &weight, &tag_id));
  std::vector<DoubleArrayTrie::Match> matches;
  trie.CommonPrefixSearch(phantom_then_present.begin(),
                          phantom_then_present.end(),
                          phantom_then_present.size(), &matches);
  EXPECT_TRUE(matches.empty());

  RuneStrArray present_then_phantom = present;
  present_then_phantom.push_back(RuneStr(phantom, 0, 1));
  EXPECT_FALSE(trie.ExactMatch(present_then_phantom.begin(),
                               present_then_phantom.end(), &weight, &tag_id));
}

TEST(DoubleArrayTrieTest, RejectsUnsortedOrDuplicateBuildKeys) {
  DoubleArrayTrie trie;
  std::string error;

  std::vector<DoubleArrayTrie::BuildEntry> descending;
  descending.push_back(Entry("甲", -1.0, 1));
  descending.push_back(Entry("乙", -2.0, 2));
  EXPECT_FALSE(trie.Build(descending, &error));
  EXPECT_NE(std::string::npos, error.find("strict Rune order"));

  std::vector<DoubleArrayTrie::BuildEntry> duplicate;
  duplicate.push_back(Entry("甲", -1.0, 1));
  duplicate.push_back(Entry("甲", -2.0, 2));
  EXPECT_FALSE(trie.Build(duplicate, &error));
  EXPECT_NE(std::string::npos, error.find("strict Rune order"));
}

TEST(DoubleArrayTrieTest, RejectsNonFinitePayloadWeight) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(
      Entry("甲", std::numeric_limits<double>::infinity(), 1));

  DoubleArrayTrie trie;
  std::string error;
  EXPECT_FALSE(trie.Build(entries, &error));
  EXPECT_NE(std::string::npos, error.find("finite weight"));
}

TEST(DoubleArrayTrieTest, AddressLimitFailurePreservesPriorBuild) {
  std::vector<DoubleArrayTrie::BuildEntry> initial_entries;
  initial_entries.push_back(Entry("a", -1.0, 7));

  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(initial_entries, &error)) << error;
  const DoubleArrayTrie::Stats initial_stats = trie.stats();

  RuneStrArray present = DecodeText("a");
  double weight = 0.0;
  uint16_t tag_id = 0;
  ASSERT_TRUE(trie.ExactMatch(present.begin(), present.end(),
                              &weight, &tag_id));
  EXPECT_DOUBLE_EQ(-1.0, weight);
  EXPECT_EQ(7u, tag_id);

  std::vector<DoubleArrayTrie::BuildEntry> limited_entries;
  limited_entries.push_back(Entry("a", -2.0, 8));
  limited_entries.push_back(Entry("😀", -3.0, 9));
  EXPECT_FALSE(trie.Build(limited_entries, &error,
                          DoubleArrayTrie::BuildOptions(32)));
  EXPECT_NE(std::string::npos, error.find("address"));

  EXPECT_EQ(initial_stats.slot_count, trie.stats().slot_count);
  EXPECT_EQ(initial_stats.occupied_state_count,
            trie.stats().occupied_state_count);
  EXPECT_EQ(initial_stats.terminal_count, trie.stats().terminal_count);
  EXPECT_DOUBLE_EQ(initial_stats.load_factor, trie.stats().load_factor);
  EXPECT_EQ(initial_stats.array_bytes, trie.stats().array_bytes);
  EXPECT_TRUE(trie.ExactMatch(present.begin(), present.end(),
                              &weight, &tag_id));
  EXPECT_DOUBLE_EQ(-1.0, weight);
  EXPECT_EQ(7u, tag_id);
  EXPECT_TRUE(trie.last_slot_occupied());
}

TEST(DoubleArrayTrieTest, TrimmedStatsMatchPackedArraysExactly) {
  std::vector<DoubleArrayTrie::BuildEntry> entries;
  entries.push_back(Entry("a", -3.0, 1));
  entries.push_back(Entry("ab", -2.0, 2));
  entries.push_back(Entry("b", -1.0, 1));

  DoubleArrayTrie trie;
  std::string error;
  ASSERT_TRUE(trie.Build(entries, &error)) << error;

  const DoubleArrayTrie::Stats& stats = trie.stats();
  EXPECT_EQ(3u, stats.terminal_count);
  EXPECT_LE(stats.occupied_state_count, stats.slot_count);
  EXPECT_EQ(stats.slot_count * 19u, stats.array_bytes);
  EXPECT_TRUE(trie.last_slot_occupied());
}

}  // namespace
}  // namespace cppjieba
