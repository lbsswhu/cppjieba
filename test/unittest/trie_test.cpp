#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cppjieba/DictTrie.hpp"
#include "cppjieba/MPSegment.hpp"
#include "gtest/gtest.h"
#include "test_paths.h"

using namespace cppjieba;

namespace {

static const char* const DICT_FILE =
    TEST_DATA_DIR "/extra_dict/jieba.dict.small.utf8";

class TempFile {
 public:
  TempFile(const std::string& name, const std::string& bytes)
      : path_(MakePath(name)) {
    std::ofstream stream(path_.c_str(), std::ios::out | std::ios::binary);
    if (!stream.is_open()) {
      throw std::runtime_error("failed to create temporary dictionary");
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream.good()) {
      throw std::runtime_error("failed to write temporary dictionary");
    }
  }

  ~TempFile() {
    std::remove(path_.c_str());
  }

  const std::string& path() const {
    return path_;
  }

 private:
  static std::string MakePath(const std::string& name) {
    static std::atomic<unsigned long> counter(0);
    std::ostringstream stream;
    stream << testing::TempDir();
    if (!stream.str().empty() && stream.str()[stream.str().size() - 1] != '/') {
      stream << '/';
    }
    stream << "cppjieba_" << name << '_'
           << std::chrono::high_resolution_clock::now()
                  .time_since_epoch().count()
           << '_' << counter.fetch_add(1) << ".dict";
    return stream.str();
  }

  std::string path_;
};

RuneStrArray DecodeText(const std::string& text) {
  RuneStrArray runes;
  EXPECT_TRUE(DecodeUTF8RunesInString(text, runes));
  return runes;
}

const DagEdge& OnlyEdge(const DictTrie& trie, const std::string& word) {
  static DagEdge fallback;
  static std::vector<Dag> dags;
  RuneStrArray runes = DecodeText(word);
  dags.clear();
  trie.BuildDag(runes.begin(), runes.end(), dags);
  EXPECT_EQ(runes.size(), dags.size());
  if (dags.empty() || dags[0].edges.empty()) {
    return fallback;
  }
  for (LocalVector<DagEdge>::const_iterator iter = dags[0].edges.begin();
       iter != dags[0].edges.end(); ++iter) {
    if (iter->in_dict && iter->end + 1 == runes.size()) {
      return *iter;
    }
  }
  return dags[0].edges[0];
}

}  // namespace

TEST(TrieTest, Empty) {
  vector<Unicode> keys;
  vector<const DictUnit*> values;
  Trie trie(keys, values);
}

TEST(TrieTest, Construct) {
  vector<Unicode> keys;
  vector<const DictUnit*> values;
  keys.push_back(DecodeUTF8RunesInString("你"));
  values.push_back((const DictUnit*)(NULL));
  Trie trie(keys, values);
}

TEST(DictTrieTest, NewAndDelete) {
  DictTrie* trie = new DictTrie(DICT_FILE);
  delete trie;
}

TEST(DictTrieTest, ExactLookupAndDagUseValueContracts) {
  DictTrie trie(DICT_FILE);
  EXPECT_NEAR(-15.6479, trie.GetMinWeight(), 0.001);

  RuneStrArray runes = DecodeText("来到");
  EXPECT_TRUE(trie.Contains(runes.begin(), runes.end()));
  EXPECT_TRUE(trie.Find("来到"));
  std::string tag;
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("v", tag);

  runes = DecodeText("清华大");
  EXPECT_FALSE(trie.Contains(runes.begin(), runes.end()));
  EXPECT_FALSE(trie.FindTag(runes.begin(), runes.end(), &tag));

  runes = DecodeText("清华大学");
  std::vector<Dag> dags;
  trie.BuildDag(runes.begin(), runes.end(), dags);
  ASSERT_EQ(runes.size(), dags.size());
  const size_t edge_counts[] = {3, 2, 2, 1};
  for (size_t i = 0; i < dags.size(); ++i) {
    EXPECT_EQ(edge_counts[i], dags[i].edges.size());
  }
  ASSERT_EQ(3u, dags[0].edges.size());
  EXPECT_EQ(0u, dags[0].edges[0].end);
  EXPECT_EQ(1u, dags[0].edges[1].end);
  EXPECT_EQ(3u, dags[0].edges[2].end);
  EXPECT_TRUE(dags[0].edges[0].in_dict);
}

TEST(DictTrieTest, StringFindUsesImmutableDictionaryData) {
  TempFile main_dict("immutable_find", "甲 100 n\n");
  DictTrie trie(main_dict.path());
  RuneStrArray runes = DecodeText("乙");

  EXPECT_FALSE(trie.Contains(runes.begin(), runes.end()));
  EXPECT_FALSE(trie.Find("乙"));

  ASSERT_TRUE(trie.InsertUserWord("乙", "runtime"));
  EXPECT_TRUE(trie.Find(runes.begin(), runes.end()) != NULL);
  EXPECT_FALSE(trie.Contains(runes.begin(), runes.end()));
  EXPECT_FALSE(trie.Find("乙"));
}

TEST(DictTrieTest, UserDictionaryPreservesTagsAndWeightOptions) {
  DictTrie trie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8");

  RuneStrArray runes = DecodeText("云计算");
  std::string tag("not-empty");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_TRUE(tag.empty());
  const DagEdge& cloud = OnlyEdge(trie, "云计算");
  EXPECT_TRUE(cloud.in_dict);
  EXPECT_EQ(2u, cloud.end);
  EXPECT_NEAR(-14.100, cloud.weight, 0.001);

  runes = DecodeText("蓝翔");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("nz", tag);
  EXPECT_NEAR(-14.100, OnlyEdge(trie, "蓝翔").weight, 0.001);

  runes = DecodeText("区块链");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("nz", tag);
  EXPECT_NEAR(-15.6478, OnlyEdge(trie, "区块链").weight, 0.001);

  runes = DecodeText("A");
  ASSERT_EQ(1u, runes.size());
  EXPECT_TRUE(trie.IsUserDictSingleRune(runes[0].rune));

  DictTrie max_weight(DICT_FILE, TEST_DATA_DIR "/userdict.utf8",
                      DictTrie::WordWeightMax);
  EXPECT_NEAR(-2.975, OnlyEdge(max_weight, "云计算").weight, 0.001);
}

TEST(DictTrieTest, DagEdgesMatchLegacyCountsAndMaximumLength) {
  DictTrie trie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8");

  struct DagCase {
    const char* text;
    size_t max_word_len;
    std::vector<size_t> edge_counts;
  };
  const DagCase cases[] = {
      {"清华大学", MAX_WORD_LENGTH, std::vector<size_t>{3, 2, 2, 1}},
      {"北京邮电大学", MAX_WORD_LENGTH,
       std::vector<size_t>{3, 1, 2, 2, 2, 1}},
      {"长江大桥", MAX_WORD_LENGTH, std::vector<size_t>{3, 1, 2, 1}},
      {"长江大桥", 3, std::vector<size_t>{2, 1, 2, 1}},
      {"长江大桥", 4, std::vector<size_t>{3, 1, 2, 1}},
  };

  for (size_t case_index = 0;
       case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
    RuneStrArray runes = DecodeText(cases[case_index].text);
    std::vector<Dag> dags;
    trie.BuildDag(runes.begin(), runes.end(), dags,
                  cases[case_index].max_word_len);
    ASSERT_EQ(cases[case_index].edge_counts.size(), dags.size());
    for (size_t i = 0; i < dags.size(); ++i) {
      EXPECT_EQ(cases[case_index].edge_counts[i], dags[i].edges.size());
    }
  }

  RuneStrArray runes = DecodeText("长江大桥");
  std::vector<Dag> limited;
  trie.BuildDag(runes.begin(), runes.end(), limited, 3);
  ASSERT_EQ(2u, limited[0].edges.size());
  EXPECT_EQ(1u, limited[0].edges[1].end);

  const DagEdge& known_single = OnlyEdge(trie, "的");
  EXPECT_TRUE(known_single.in_dict);

  TempFile prefix_dict("prefix_main", "甲乙 100 n\n丙 50 n\n");
  DictTrie prefix_trie(prefix_dict.path());
  const DagEdge& prefix_only = OnlyEdge(prefix_trie, "甲");
  EXPECT_FALSE(prefix_only.in_dict);
  EXPECT_EQ(0u, prefix_only.end);
  EXPECT_DOUBLE_EQ(prefix_trie.GetMinWeight(), prefix_only.weight);

  const DagEdge& missing = OnlyEdge(prefix_trie, "丁");
  EXPECT_FALSE(missing.in_dict);
}

TEST(DictTrieTest, LaterUserRowsOverrideMainAndEarlierUsers) {
  TempFile main_dict("override_main", "词 100 n\n甲 50 n\n");
  TempFile user1("override_user1", "词 5 first\n单 first_single\n");
  TempFile user2("override_user2", "词 10 last\n单 last_single\n");
  DictTrie trie(main_dict.path(), user1.path() + ";" + user2.path());

  RuneStrArray runes = DecodeText("词");
  std::string tag;
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("last", tag);
  const DagEdge& word = OnlyEdge(trie, "词");
  EXPECT_TRUE(word.in_dict);
  EXPECT_NEAR(std::log(10.0) - std::log(150.0), word.weight, 1e-12);

  runes = DecodeText("单");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("last_single", tag);
  EXPECT_TRUE(trie.IsUserDictSingleRune(runes[0].rune));
}

TEST(DictTrieTest, CacheIdentityIncludesPathsAndWeightOption) {
  TempFile main_dict("cache_main", "词 100 n\n甲 50 n\n");
  TempFile user1("cache_user1", "词 5 first\n单 first_single\n");
  TempFile user2("cache_user2", "词 10 last\n单 last_single\n");
  const std::string users = user1.path() + "|" + user2.path();

  DictTrie first(main_dict.path(), users, DictTrie::WordWeightMedian);
  DictTrie same(main_dict.path(), users, DictTrie::WordWeightMedian);
  DictTrie different_option(main_dict.path(), users, DictTrie::WordWeightMax);
  EXPECT_EQ(first.GetDictionaryDataIdentity(),
            same.GetDictionaryDataIdentity());
  EXPECT_NE(first.GetDictionaryDataIdentity(),
            different_option.GetDictionaryDataIdentity());

  const DictionaryStats& stats = first.GetStats();
  EXPECT_GT(stats.slot_count, 0u);
  EXPECT_GT(stats.occupied_state_count, 0u);
  EXPECT_EQ(3u, stats.terminal_count);
  EXPECT_GT(stats.load_factor, 0.0);
  EXPECT_LE(stats.load_factor, 1.0);
  EXPECT_EQ(stats.slot_count * 19u, stats.array_bytes);
  EXPECT_EQ(16u, stats.tag_bytes);
}

TEST(DictTrieTest, ExtremeFrequencyRatioKeepsFiniteLogWeight) {
  TempFile main_dict(
      "extreme_ratio",
      "微 2.2250738585072014e-308 n\n巨 1.7976931348623157e+308 n\n");
  DictTrie trie(main_dict.path());

  const DagEdge& edge = OnlyEdge(trie, "微");
  ASSERT_TRUE(edge.in_dict);
  EXPECT_TRUE(std::isfinite(edge.weight));
  const double expected =
      std::log(std::numeric_limits<double>::min()) -
      std::log(std::numeric_limits<double>::max());
  EXPECT_NEAR(expected, edge.weight, 1e-12);
}

TEST(DictTrieDeathTest, RejectsOverflowingAggregateFrequency) {
  TempFile main_dict(
      "overflow",
      "甲 1.7976931348623157e+308 n\n乙 1.7976931348623157e+308 n\n");
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path());
      },
      "aggregate frequency must be finite and greater than zero");
}

TEST(DictTrieDeathTest, RejectsMalformedOrInvalidMainRows) {
  TempFile malformed("malformed", "甲 10\n");
  EXPECT_DEATH(
      {
        DictTrie trie(malformed.path());
      },
      "main dictionary row must contain exactly word frequency tag");

  TempFile nonpositive("nonpositive", "甲 0 n\n");
  EXPECT_DEATH(
      {
        DictTrie trie(nonpositive.path());
      },
      "frequency must be finite and greater than zero");

  TempFile nonfinite("nonfinite", "甲 inf n\n");
  EXPECT_DEATH(
      {
        DictTrie trie(nonfinite.path());
      },
      "frequency must be finite and greater than zero");
}

TEST(DictTrieDeathTest, RejectsMalformedOrInvalidUserRows) {
  TempFile main_dict("user_validation_main", "甲 100 n\n");
  TempFile malformed("user_malformed", "乙 10 n extra\n");
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path(), malformed.path());
      },
      "user dictionary row must contain word, word tag, or word frequency tag");

  TempFile invalid_frequency("user_invalid_frequency", "乙 nan n\n");
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path(), invalid_frequency.path());
      },
      "frequency must be finite and greater than zero");
}
