#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

DagEdge FullWordEdge(const DictTrie& trie, const std::string& word) {
  std::vector<Dag> dags;
  RuneStrArray runes = DecodeText(word);
  trie.BuildDag(runes.begin(), runes.end(), dags);
  if (dags.size() != runes.size() || dags.empty()) {
    ADD_FAILURE() << "DAG size does not match word length for " << word;
    return DagEdge();
  }
  for (LocalVector<DagEdge>::const_iterator iter = dags[0].edges.begin();
       iter != dags[0].edges.end(); ++iter) {
    if (iter->in_dict && iter->end + 1 == runes.size()) {
      return *iter;
    }
  }
  ADD_FAILURE() << "full-word dictionary edge is absent for " << word;
  return DagEdge();
}

std::string NumberedWord(size_t index) {
  std::ostringstream stream;
  stream << "word" << std::setw(5) << std::setfill('0') << index;
  return stream.str();
}

std::string NumberedTag(size_t index) {
  std::ostringstream stream;
  stream << "tag" << std::setw(5) << std::setfill('0') << index;
  return stream.str();
}

std::string BuildTaggedUserDictionary(size_t count) {
  std::ostringstream stream;
  for (size_t i = 0; i < count; ++i) {
    stream << NumberedWord(i) << ' ' << NumberedTag(i) << '\n';
  }
  return stream.str();
}

}  // namespace

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

TEST(DictTrieTest, RuntimeMutationIsRejectedWithoutStateChange) {
  TempFile main_dict("immutable_find", "甲 100 n\n");
  DictTrie trie(main_dict.path());
  RuneStrArray runes = DecodeText("乙");

  EXPECT_FALSE(trie.Contains(runes.begin(), runes.end()));
  EXPECT_FALSE(trie.Find("乙"));

  testing::internal::CaptureStderr();
  EXPECT_FALSE(trie.InsertUserWord("乙", "runtime"));
  const std::string log = testing::internal::GetCapturedStderr();

  EXPECT_NE(std::string::npos,
            log.find("static dictionary is immutable"));
  EXPECT_FALSE(trie.Contains(runes.begin(), runes.end()));
  EXPECT_FALSE(trie.Find("乙"));
}

TEST(DictTrieTest, UserDictionaryPreservesTagsAndWeightOptions) {
  DictTrie trie(DICT_FILE, TEST_DATA_DIR "/userdict.utf8");

  RuneStrArray runes = DecodeText("云计算");
  std::string tag("not-empty");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_TRUE(tag.empty());
  const DagEdge cloud = FullWordEdge(trie, "云计算");
  EXPECT_TRUE(cloud.in_dict);
  EXPECT_EQ(2u, cloud.end);
  EXPECT_NEAR(-14.100, cloud.weight, 0.001);

  runes = DecodeText("蓝翔");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("nz", tag);
  EXPECT_NEAR(-14.100, FullWordEdge(trie, "蓝翔").weight, 0.001);

  runes = DecodeText("区块链");
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ("nz", tag);
  EXPECT_NEAR(-15.6478, FullWordEdge(trie, "区块链").weight, 0.001);

  runes = DecodeText("A");
  ASSERT_EQ(1u, runes.size());
  EXPECT_TRUE(trie.IsUserDictSingleRune(runes[0].rune));

  DictTrie max_weight(DICT_FILE, TEST_DATA_DIR "/userdict.utf8",
                      DictTrie::WordWeightMax);
  EXPECT_NEAR(-2.975, FullWordEdge(max_weight, "云计算").weight, 0.001);
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

  const DagEdge known_single = FullWordEdge(trie, "的");
  EXPECT_TRUE(known_single.in_dict);

  TempFile prefix_dict("prefix_main", "甲乙 100 n\n丙 50 n\n");
  DictTrie prefix_trie(prefix_dict.path());
  RuneStrArray prefix_runes = DecodeText("甲");
  std::vector<Dag> prefix_dags;
  prefix_trie.BuildDag(prefix_runes.begin(), prefix_runes.end(), prefix_dags);
  ASSERT_EQ(1u, prefix_dags.size());
  ASSERT_EQ(1u, prefix_dags[0].edges.size());
  const DagEdge prefix_only = prefix_dags[0].edges[0];
  EXPECT_FALSE(prefix_only.in_dict);
  EXPECT_EQ(0u, prefix_only.end);
  EXPECT_DOUBLE_EQ(prefix_trie.GetMinWeight(), prefix_only.weight);

  RuneStrArray missing_runes = DecodeText("丁");
  std::vector<Dag> missing_dags;
  prefix_trie.BuildDag(missing_runes.begin(), missing_runes.end(),
                       missing_dags);
  ASSERT_EQ(1u, missing_dags.size());
  ASSERT_EQ(1u, missing_dags[0].edges.size());
  const DagEdge missing = missing_dags[0].edges[0];
  EXPECT_FALSE(missing.in_dict);
  EXPECT_EQ(0u, missing.end);
  EXPECT_DOUBLE_EQ(prefix_trie.GetMinWeight(), missing.weight);
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
  const DagEdge word = FullWordEdge(trie, "词");
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

TEST(DictTrieTest, ConcurrentConstructionAndReadsShareImmutableData) {
  TempFile user_dict("concurrent_user", "并发词 concurrent\n");
  const size_t thread_count = 8;
  std::vector<std::unique_ptr<DictTrie> > tries(thread_count);
  std::vector<std::thread> constructors;
  std::atomic<size_t> ready_count(0);
  std::atomic<bool> start(false);
  for (size_t i = 0; i < thread_count; ++i) {
    constructors.push_back(std::thread(
        [&tries, &user_dict, &ready_count, &start, i]() {
      ready_count.fetch_add(1);
      while (!start.load()) {
        std::this_thread::yield();
      }
      tries[i].reset(new DictTrie(DICT_FILE, user_dict.path()));
    }));
  }
  while (ready_count.load() != thread_count) {
    std::this_thread::yield();
  }
  start.store(true);
  for (size_t i = 0; i < constructors.size(); ++i) {
    constructors[i].join();
  }

  const void* identity = tries[0]->GetDictionaryDataIdentity();
  for (size_t i = 1; i < tries.size(); ++i) {
    ASSERT_EQ(identity, tries[i]->GetDictionaryDataIdentity());
  }

  std::atomic<bool> ok(true);
  std::vector<std::thread> readers;
  for (size_t i = 0; i < thread_count; ++i) {
    readers.push_back(std::thread([&tries, &ok, i]() {
      for (size_t iteration = 0; iteration < 1000; ++iteration) {
        if (!tries[i]->Find("来到") ||
            tries[i]->Find("肯定不存在的词")) {
          ok.store(false);
          return;
        }
      }
    }));
  }
  for (size_t i = 0; i < readers.size(); ++i) {
    readers[i].join();
  }
  EXPECT_TRUE(ok.load());
}

TEST(DictTrieTest, RepeatedAsciiWhitespaceIsAccepted) {
  TempFile main_dict("whitespace_main", "甲\t  100   n  \n");
  TempFile user_dict("whitespace_user", "乙  \t 10 \t user  \n");
  DictTrie trie(main_dict.path(), user_dict.path());

  EXPECT_TRUE(trie.Find("甲"));
  RuneStrArray user_word = DecodeText("乙");
  std::string tag;
  ASSERT_TRUE(trie.FindTag(user_word.begin(), user_word.end(), &tag));
  EXPECT_EQ("user", tag);
}

TEST(DictTrieTest, MaximumTagIdCapacityIsUsable) {
  const size_t tag_count = 65535;
  TempFile main_dict("tag_capacity_main", "word00000 100 main\n");
  TempFile user_dict("tag_capacity_user",
                     BuildTaggedUserDictionary(tag_count));
  DictTrie trie(main_dict.path(), user_dict.path());

  EXPECT_EQ(tag_count, trie.GetStats().terminal_count);
  const std::string last_word = NumberedWord(tag_count - 1);
  RuneStrArray runes = DecodeText(last_word);
  std::string tag;
  ASSERT_TRUE(trie.FindTag(runes.begin(), runes.end(), &tag));
  EXPECT_EQ(NumberedTag(tag_count - 1), tag);
}

TEST(DictTrieTest, DagHonorsConfiguredLimitAt513Runes) {
  const std::string long_word(513, 'a');
  TempFile main_dict("long_word", long_word + " 100 long\n");
  DictTrie trie(main_dict.path());
  RuneStrArray runes = DecodeText(long_word);
  ASSERT_EQ(513u, runes.size());

  std::vector<Dag> limited;
  trie.BuildDag(runes.begin(), runes.end(), limited, 512);
  ASSERT_EQ(513u, limited.size());
  for (LocalVector<DagEdge>::const_iterator iter = limited[0].edges.begin();
       iter != limited[0].edges.end(); ++iter) {
    EXPECT_FALSE(iter->in_dict && iter->end == 512u);
  }

  std::vector<Dag> complete;
  trie.BuildDag(runes.begin(), runes.end(), complete, 513);
  ASSERT_EQ(513u, complete.size());
  bool found_full_word = false;
  for (LocalVector<DagEdge>::const_iterator iter = complete[0].edges.begin();
       iter != complete[0].edges.end(); ++iter) {
    if (iter->in_dict && iter->end == 512u) {
      found_full_word = true;
    }
  }
  EXPECT_TRUE(found_full_word);
}

TEST(DictTrieTest, DuplicateCollapseProducesExactStats) {
  TempFile main_dict("stats_main", "甲 100 old\n乙 50 shared\n");
  TempFile user_dict("stats_user", "甲 10 final\n丙 shared\n");
  DictTrie trie(main_dict.path(), user_dict.path());

  const DictionaryStats& stats = trie.GetStats();
  EXPECT_EQ(3u, stats.terminal_count);
  EXPECT_LE(stats.occupied_state_count, stats.slot_count);
  EXPECT_EQ(stats.slot_count * 19u, stats.array_bytes);
  EXPECT_EQ(11u, stats.tag_bytes);
}

TEST(DictTrieTest, ExtremeFrequencyRatioKeepsFiniteLogWeight) {
  TempFile main_dict(
      "extreme_ratio",
      "微 2.2250738585072014e-308 n\n巨 1.7976931348623157e+308 n\n");
  DictTrie trie(main_dict.path());

  const DagEdge edge = FullWordEdge(trie, "微");
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
      "aggregate frequency must be finite and greater than zero at [^:]+:2");
}

TEST(DictTrieDeathTest, RejectsOutOfRangeMainFrequency) {
  TempFile main_dict("main_erange", "甲 1e-323 n\n");
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path());
      },
      "frequency must be finite and greater than zero");
}

TEST(DictTrieDeathTest, RejectsOutOfRangeExplicitUserFrequency) {
  TempFile main_dict("user_erange_main", "甲 100 n\n");
  TempFile user_dict("user_erange", "乙 1e-323 n\n");
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path(), user_dict.path());
      },
      "frequency must be finite and greater than zero");
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

TEST(DictTrieDeathTest, RejectsEveryInvalidMainFrequencyTokenAtSource) {
  const char* const invalid_frequencies[] = {
      "0", "-1", "nan", "inf", "1x", "1e-323"};
  for (size_t i = 0;
       i < sizeof(invalid_frequencies) / sizeof(invalid_frequencies[0]); ++i) {
    TempFile main_dict(
        "main_frequency",
        std::string("甲 ") + invalid_frequencies[i] + " n\n");
    EXPECT_DEATH(
        {
          DictTrie trie(main_dict.path());
        },
        "frequency must be finite and greater than zero at [^:]+:1");
  }
}

TEST(DictTrieDeathTest, RejectsMainColumnAndBlankRowErrorsAtSource) {
  const char* const invalid_rows[] = {
      "甲 n\n",
      "甲 10 n extra\n",
      "\n",
      " \t  \n",
  };
  for (size_t i = 0;
       i < sizeof(invalid_rows) / sizeof(invalid_rows[0]); ++i) {
    TempFile main_dict("main_row", invalid_rows[i]);
    EXPECT_DEATH(
        {
          DictTrie trie(main_dict.path());
        },
        "main dictionary row must contain exactly word frequency tag at [^:]+:1");
  }
}

TEST(DictTrieDeathTest, RejectsInvalidUtf8MainWordsAtSource) {
  const std::string invalid_words[] = {
      std::string("\x80", 1),
      std::string("\xff", 1),
      std::string("\xc2", 1) + "A",
      std::string("\xe2\x82", 2),
      std::string("\xc0\xaf", 2),
      std::string("\xed\xa0\x80", 3),
      std::string("\xf4\x90\x80\x80", 4),
  };
  for (size_t i = 0;
       i < sizeof(invalid_words) / sizeof(invalid_words[0]); ++i) {
    TempFile main_dict("main_utf8", invalid_words[i] + " 10 n\n");
    EXPECT_DEATH(
        {
          DictTrie trie(main_dict.path());
        },
        "dictionary word must be nonempty valid UTF-8 at [^:]+:1");
  }
}

TEST(DictTrieDeathTest, RejectsEmptyMainDictionaryWithStableDiagnostic) {
  TempFile main_dict("empty_main", "");
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path());
      },
      "effective dictionary is empty");
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

TEST(DictTrieDeathTest, RejectsEveryInvalidUserFrequencyTokenAtSource) {
  TempFile main_dict("user_frequency_main", "甲 100 n\n");
  const char* const invalid_frequencies[] = {
      "0", "-1", "nan", "inf", "1x", "1e-323"};
  for (size_t i = 0;
       i < sizeof(invalid_frequencies) / sizeof(invalid_frequencies[0]); ++i) {
    TempFile user_dict(
        "user_frequency",
        std::string("乙 ") + invalid_frequencies[i] + " u\n");
    EXPECT_DEATH(
        {
          DictTrie trie(main_dict.path(), user_dict.path());
        },
        "frequency must be finite and greater than zero at [^:]+:1");
  }
}

TEST(DictTrieDeathTest, RejectsUserColumnAndBlankRowErrorsAtSource) {
  TempFile main_dict("user_row_main", "甲 100 n\n");
  const char* const invalid_rows[] = {
      "乙 10 u extra\n",
      "\n",
      " \t  \n",
  };
  for (size_t i = 0;
       i < sizeof(invalid_rows) / sizeof(invalid_rows[0]); ++i) {
    TempFile user_dict("user_row", invalid_rows[i]);
    EXPECT_DEATH(
        {
          DictTrie trie(main_dict.path(), user_dict.path());
        },
        "user dictionary row must contain word, word tag, or word frequency tag at [^:]+:1");
  }
}

TEST(DictTrieDeathTest, RejectsInvalidUtf8UserWordsAtSource) {
  TempFile main_dict("user_utf8_main", "甲 100 n\n");
  const std::string invalid_words[] = {
      std::string("\x80", 1),
      std::string("\xff", 1),
      std::string("\xc2", 1) + "A",
      std::string("\xe2\x82", 2),
      std::string("\xc0\xaf", 2),
      std::string("\xed\xa0\x80", 3),
      std::string("\xf4\x90\x80\x80", 4),
  };
  for (size_t i = 0;
       i < sizeof(invalid_words) / sizeof(invalid_words[0]); ++i) {
    TempFile user_dict("user_utf8", invalid_words[i] + " tag\n");
    EXPECT_DEATH(
        {
          DictTrie trie(main_dict.path(), user_dict.path());
        },
        "dictionary word must be nonempty valid UTF-8 at [^:]+:1");
  }
}

TEST(DictTrieDeathTest, RejectsMoreThan65535SurvivingNonEmptyTags) {
  TempFile main_dict("tag_overflow_main", "word00000 100 main\n");
  TempFile user_dict("tag_overflow_user",
                     BuildTaggedUserDictionary(65536));
  EXPECT_DEATH(
      {
        DictTrie trie(main_dict.path(), user_dict.path());
      },
      "more than 65535 non-empty tags");
}
