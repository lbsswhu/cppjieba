#include "cppjieba/DictTrie.hpp"
#include "gtest/gtest.h"
#include "test_paths.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace cppjieba;

static const char* const DICT_FILE = TEST_DATA_DIR "/extra_dict/jieba.dict.small.utf8";

TEST(DictTrieTest, NewAndDelete) {
  DictTrie * trie;
  trie = new DictTrie(DICT_FILE);
  delete trie;
}

TEST(DictTrieTest, Test1) {
  string s1, s2;
  DictTrie trie(DICT_FILE);
  ASSERT_LT(trie.GetMinWeight() + 15.6479, 0.001);
  string word("来到");
  cppjieba::RuneStrArray uni;
  ASSERT_TRUE(DecodeUTF8RunesInString(word, uni));
  const DictUnit* du = trie.Find(uni.begin(), uni.end());
  ASSERT_TRUE(du != NULL);
  ASSERT_EQ(2u, du->word.size());
  ASSERT_EQ(26469u, du->word[0]);
  ASSERT_EQ(21040u, du->word[1]);
  ASSERT_EQ("v", du->tag);
  ASSERT_NEAR(-8.870, du->weight, 0.001);

  word = "清华大学";
  LocalVector<pair<size_t, const DictUnit*> > res;
  const char * words[] = {"清", "清华", "清华大学"};
  for (size_t i = 0; i < sizeof(words)/sizeof(words[0]); i++) {
    ASSERT_TRUE(DecodeUTF8RunesInString(words[i], uni));
    res.push_back(make_pair(uni.size() - 1, trie.Find(uni.begin(), uni.end())));
  }
  vector<pair<size_t, const DictUnit*> > vec;
  vector<struct Dag> dags;
  ASSERT_TRUE(DecodeUTF8RunesInString(word, uni));
  trie.Find(uni.begin(), uni.end(), dags);
  ASSERT_EQ(dags.size(), uni.size());
  ASSERT_NE(dags.size(), 0u);
  s1 << res;
  s2 << dags[0].nexts;
  ASSERT_EQ(s1, s2);
}

TEST(DictTrieTest, UserDict) {
  DictTrie trie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8");
  string word = "云计算";
  cppjieba::RuneStrArray unicode;
  ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
  const DictUnit * unit = trie.Find(unicode.begin(), unicode.end());
  ASSERT_TRUE(unit != NULL);
  ASSERT_NEAR(unit->weight, -14.100, 0.001);

  word = "蓝翔";
  ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
  unit = trie.Find(unicode.begin(), unicode.end());
  ASSERT_TRUE(unit != NULL);
  ASSERT_EQ(unit->tag, "nz");
  ASSERT_NEAR(unit->weight, -14.100, 0.001);

  word = "区块链";
  ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
  unit = trie.Find(unicode.begin(), unicode.end());
  ASSERT_TRUE(unit != NULL);
  ASSERT_EQ(unit->tag, "nz");
  ASSERT_NEAR(unit->weight, -15.6478, 0.001);
}

TEST(DictTrieTest, UserDictWithMaxWeight) {
  DictTrie trie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8", DictTrie::WordWeightMax);
  string word = "云计算";
  cppjieba::RuneStrArray unicode;
  ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
  const DictUnit * unit = trie.Find(unicode.begin(), unicode.end());
  ASSERT_TRUE(unit);
  ASSERT_NEAR(unit->weight, -2.975, 0.001);
}

TEST(DictTrieTest, Dag) {
  DictTrie trie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8");

  {
    string word = "清华大学";
    cppjieba::RuneStrArray unicode;
    ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
    vector<struct Dag> res;
    trie.Find(unicode.begin(), unicode.end(), res);

    size_t nexts_sizes[] = {3, 2, 2, 1};
    ASSERT_EQ(res.size(), sizeof(nexts_sizes)/sizeof(nexts_sizes[0]));
    for (size_t i = 0; i < res.size(); i++) {
      ASSERT_EQ(res[i].nexts.size(), nexts_sizes[i]);
    }
  }

  {
    string word = "北京邮电大学";
    cppjieba::RuneStrArray unicode;
    ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
    vector<struct Dag> res;
    trie.Find(unicode.begin(), unicode.end(), res);

    size_t nexts_sizes[] = {3, 1, 2, 2, 2, 1};
    ASSERT_EQ(res.size(), sizeof(nexts_sizes)/sizeof(nexts_sizes[0]));
    for (size_t i = 0; i < res.size(); i++) {
      ASSERT_EQ(res[i].nexts.size(), nexts_sizes[i]);
    }
  }

  {
    string word = "长江大桥";
    cppjieba::RuneStrArray unicode;
    ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
    vector<struct Dag> res;
    trie.Find(unicode.begin(), unicode.end(), res);

    size_t nexts_sizes[] = {3, 1, 2, 1};
    ASSERT_EQ(res.size(), sizeof(nexts_sizes)/sizeof(nexts_sizes[0]));
    for (size_t i = 0; i < res.size(); i++) {
      ASSERT_EQ(res[i].nexts.size(), nexts_sizes[i]);
    }
  }

  {
    string word = "长江大桥";
    cppjieba::RuneStrArray unicode;
    ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
    vector<struct Dag> res;
    trie.Find(unicode.begin(), unicode.end(), res, 3);

    size_t nexts_sizes[] = {2, 1, 2, 1};
    ASSERT_EQ(res.size(), sizeof(nexts_sizes)/sizeof(nexts_sizes[0]));
    for (size_t i = 0; i < res.size(); i++) {
      ASSERT_EQ(res[i].nexts.size(), nexts_sizes[i]);
    }
  }

  {
    string word = "长江大桥";
    cppjieba::RuneStrArray unicode;
    ASSERT_TRUE(DecodeUTF8RunesInString(word, unicode));
    vector<struct Dag> res;
    trie.Find(unicode.begin(), unicode.end(), res, 4);

    size_t nexts_sizes[] = {3, 1, 2, 1};
    ASSERT_EQ(res.size(), sizeof(nexts_sizes)/sizeof(nexts_sizes[0]));
    for (size_t i = 0; i < res.size(); i++) {
      ASSERT_EQ(res[i].nexts.size(), nexts_sizes[i]);
    }
  }
}

TEST(DictTrieTest, RepeatedDagEnumerationReplacesExistingCandidates) {
  const DictTrie trie(DICT_FILE);
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("清华大学", runes));
  vector<Dag> result(7);
  for (size_t i = 0; i < result.size(); ++i)
    result[i].nexts.push_back(make_pair(size_t(999), static_cast<const DictUnit*>(NULL)));
  const size_t expected[] = {3, 2, 2, 1};
  for (size_t repeat = 0; repeat < 3; ++repeat) {
    trie.Find(runes.begin(), runes.end(), result);
    ASSERT_EQ(runes.size(), result.size());
    for (size_t i = 0; i < result.size(); ++i) {
      EXPECT_EQ(expected[i], result[i].nexts.size());
      EXPECT_EQ(runes[i].rune, result[i].runestr.rune);
      for (size_t edge = 0; edge < result[i].nexts.size(); ++edge)
        EXPECT_LT(result[i].nexts[edge].first, runes.size());
    }
  }
  trie.Find(runes.begin(), runes.end(), result, 0);
  ASSERT_EQ(runes.size(), result.size());
  for (size_t i = 0; i < result.size(); ++i) {
    ASSERT_EQ(size_t(1), result[i].nexts.size());
    EXPECT_EQ(i, result[i].nexts[0].first);
    EXPECT_EQ(trie.Find(runes.begin() + i, runes.begin() + i + 1), result[i].nexts[0].second);
  }
  trie.Find(runes.begin(), runes.begin() + 1, result);
  ASSERT_EQ(size_t(1), result.size());
  ASSERT_EQ(size_t(1), result[0].nexts.size());
  trie.Find(runes.begin(), runes.begin(), result);
  EXPECT_TRUE(result.empty());
}

namespace {
class TempDictionary {
 public:
  explicit TempDictionary(const string& contents) {
    static std::atomic<unsigned> serial(0);
    std::ostringstream name;
    name << testing::TempDir() << "cppjieba_dat_payload_"
         << std::chrono::high_resolution_clock::now().time_since_epoch().count()
         << '_' << serial++ << ".dict";
    path_ = name.str();
    std::ofstream out(path_.c_str());
    if (!out) throw std::runtime_error("cannot create DAT dictionary test fixture");
    out << contents;
  }
  ~TempDictionary() { std::remove(path_.c_str()); }
  const string& path() const { return path_; }
 private:
  string path_;
};

const DictUnit* Lookup(const DictTrie& dictionary, const string& word) {
  RuneStrArray runes;
  if (!DecodeUTF8RunesInString(word, runes)) return NULL;
  return dictionary.Find(runes.begin(), runes.end());
}
} // namespace

TEST(DictTrieTest, ConstLookupUsesLastSourcePayloadForDuplicateWords) {
  TempDictionary base("甲 10 first\n甲乙 20 main_pair\n甲 30 main_last\n乙 40 second\n"
                      "同 10 same_weight_first\n异 10 same_weight_second\n前缀词 20 prefix\n");
  TempDictionary firstUser("甲 7 startup_first\n甲乙 13 startup_pair\n");
  TempDictionary lastUser("甲 11 startup_last\n独 10 protected\n");
  const DictTrie mainOnly(base.path());
  const DictUnit* mainWinner = Lookup(mainOnly, "甲");
  ASSERT_TRUE(mainWinner != NULL);
  EXPECT_EQ("main_last", mainWinner->tag);
  EXPECT_DOUBLE_EQ(std::log(30.0 / 140.0), mainWinner->weight);

  const DictTrie dictionary(base.path(), firstUser.path() + "|" + lastUser.path());
  EXPECT_TRUE(dictionary.Find("甲"));
  EXPECT_TRUE(dictionary.Contains("甲"));
  EXPECT_TRUE(dictionary.Find("甲乙"));
  EXPECT_TRUE(dictionary.Contains("甲乙"));
  EXPECT_FALSE(dictionary.Find("甲丙"));
  EXPECT_FALSE(dictionary.Contains("甲丙"));
  EXPECT_FALSE(dictionary.Find("前缀"));
  EXPECT_FALSE(dictionary.Contains("前缀"));
  EXPECT_FALSE(dictionary.Find(""));
  EXPECT_FALSE(dictionary.Contains(""));
  EXPECT_FALSE(dictionary.Find(string(1, '\xff')));
  EXPECT_FALSE(dictionary.Contains(string(1, '\xff')));
  const DictUnit* winner = Lookup(dictionary, "甲");
  ASSERT_TRUE(winner != NULL);
  EXPECT_EQ("startup_last", winner->tag);
  EXPECT_DOUBLE_EQ(std::log(11.0 / 140.0), winner->weight);
  ASSERT_EQ(size_t(1), winner->word.size());
  EXPECT_EQ(Rune(0x7532), winner->word[0]);
  EXPECT_TRUE(dictionary.IsUserDictSingleChineseWord(0x7532));
  const DictUnit* pairWinner = Lookup(dictionary, "甲乙");
  ASSERT_TRUE(pairWinner != NULL);
  EXPECT_EQ("startup_pair", pairWinner->tag);
  EXPECT_DOUBLE_EQ(std::log(13.0 / 140.0), pairWinner->weight);

  // Equal weight codes must not select another word's metadata payload.
  const DictUnit* sameWeightA = Lookup(dictionary, "同");
  const DictUnit* sameWeightB = Lookup(dictionary, "异");
  ASSERT_TRUE(sameWeightA != NULL);
  ASSERT_TRUE(sameWeightB != NULL);
  EXPECT_NE(sameWeightA, sameWeightB);
  EXPECT_DOUBLE_EQ(sameWeightA->weight, sameWeightB->weight);
  EXPECT_EQ("same_weight_first", sameWeightA->tag);
  EXPECT_EQ("same_weight_second", sameWeightB->tag);

  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("甲乙", runes));
  EXPECT_TRUE(dictionary.Contains(runes.begin(), runes.end()));
  vector<Dag> candidates;
  dictionary.Find(runes.begin(), runes.end(), candidates);
  ASSERT_EQ(size_t(2), candidates.size());
  ASSERT_EQ(size_t(2), candidates[0].nexts.size());
  EXPECT_EQ(winner, candidates[0].nexts[0].second);
  EXPECT_EQ(pairWinner, candidates[0].nexts[1].second);
  EXPECT_EQ(size_t(0), candidates[0].nexts[0].first);
  EXPECT_EQ(size_t(1), candidates[0].nexts[1].first);
}
