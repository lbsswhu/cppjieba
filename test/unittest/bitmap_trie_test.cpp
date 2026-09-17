#include "cppjieba/BitmapTrie.hpp"
#include "gtest/gtest.h"
#include <cmath>
#include <deque>
#include <random>

using namespace cppjieba;

namespace {
std::vector<size_t> ReferencePath(const Trie& trie, const RuneStrArray& runes,
                                size_t limit, double unknown) {
  std::vector<Dag> dags;
  trie.Find(runes.begin(), runes.end(), dags, limit);
  std::vector<double> scores(runes.size() + 1, 0.0);
  std::vector<size_t> lengths(runes.size(), 1);
  for (size_t i = runes.size(); i-- > 0;) {
    double best = MIN_DOUBLE;
    for (size_t j = 0; j < dags[i].nexts.size(); ++j) {
      const auto& edge = dags[i].nexts[j];
      const double score = scores[edge.first + 1] + (edge.second ? edge.second->weight : unknown);
      if (score > best) {
        best = score;
        lengths[i] = edge.first - i + 1;
      }
    }
    scores[i] = best;
  }
  return lengths;
}
}

TEST(BitmapTrieTest, RingMatchesLegacyCandidatesAndTieBreaking) {
  const char* words[] = {"a", "b", "ab", "abc", "bc", "中", "中文", "文", "😀", "a中文"};
  const double weights[] = {-1, -1, -2, -3, -2, -2, -3, -1, -4, -3};
  std::vector<DictUnit> units(10);
  std::vector<Unicode> keys;
  std::vector<const DictUnit*> values;
  for (size_t i = 0; i < 10; ++i) {
    units[i].word = DecodeUTF8RunesInString(words[i]);
    units[i].weight = weights[i];
    keys.push_back(units[i].word);
    values.push_back(&units[i]);
  }
  Trie legacy(keys, values);
  BitmapTrie packed(keys, values);
  std::mt19937 rng(20260917);
  const char* alphabet[] = {"a", "b", "c", "中", "文", "😀", "?"};
  const size_t limits[] = {0, 1, 2, 3, 512};
  for (size_t trial = 0; trial < 100; ++trial) {
    std::string text;
    for (size_t j = 0, length = rng() % 100; j < length; ++j) text += alphabet[rng() % 7];
    RuneStrArray runes;
    ASSERT_TRUE(DecodeUTF8RunesInString(text, runes));
    for (size_t limit : limits) {
      std::vector<size_t> actual;
      packed.FindBestPath(runes.begin(), runes.end(), -10, limit, actual);
      EXPECT_EQ(ReferencePath(legacy, runes, limit, -10), actual) << text << " limit=" << limit;
      std::vector<Dag> expected_dag, actual_dag;
      legacy.Find(runes.begin(), runes.end(), expected_dag, limit);
      packed.Find(runes.begin(), runes.end(), actual_dag, limit);
      ASSERT_EQ(expected_dag.size(), actual_dag.size());
      for (size_t i = 0; i < expected_dag.size(); ++i) EXPECT_EQ(expected_dag[i].nexts, actual_dag[i].nexts);
    }
  }
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("abc", runes));
  std::vector<size_t> actual;
  packed.FindBestPath(runes.begin(), runes.end(), -10, 512, actual);
  EXPECT_EQ(1u, actual[0]); // a+bc ties with ab+c only if c is known; a+bc ties with abc here.
}

TEST(BitmapTrieTest, WeightOverflowFallsBackAndStillHonorsOverlay) {
  std::vector<DictUnit> units(8200);
  std::vector<Unicode> keys;
  std::vector<const DictUnit*> values;
  for (size_t i = 0; i < units.size(); ++i) {
    units[i].word = DecodeUTF8RunesInString("word" + std::to_string(i));
    units[i].weight = -static_cast<double>(i + 1);
    keys.push_back(units[i].word);
    values.push_back(&units[i]);
  }
  BitmapTrie packed(keys, values);
  EXPECT_FALSE(packed.UsesPackedDAT());
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("word8199", runes));
  EXPECT_EQ(&units.back(), packed.Find(runes.begin(), runes.end()));
  packed.DeleteNode(units.back().word, NULL);
  EXPECT_EQ(nullptr, packed.Find(runes.begin(), runes.end()));
  packed.InsertNode(units.back().word, &units.back());
  EXPECT_EQ(&units.back(), packed.Find(runes.begin(), runes.end()));
  Trie legacy(keys, values);
  std::vector<size_t> actual;
  packed.FindBestPath(runes.begin(), runes.end(), -10000, 512, actual);
  EXPECT_EQ(ReferencePath(legacy, runes, 512, -10000), actual);
}
