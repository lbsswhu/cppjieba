#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include "cppjieba/HMMSegment.hpp"
#include "cppjieba/HmmScratch.hpp"
#include "gtest/gtest.h"
#include "test_paths.h"

using namespace cppjieba;

namespace {

class ModelFile {
 public:
  explicit ModelFile(const string& contents) {
    static std::atomic<unsigned> serial(0);
    std::ostringstream name;
    name << testing::TempDir() << "cppjieba_hmm_cpu_"
         << std::chrono::high_resolution_clock::now().time_since_epoch().count()
         << '_' << serial++ << ".model";
    path_ = name.str();
    std::ofstream stream(path_.c_str());
    stream << contents;
  }
  ~ModelFile() { std::remove(path_.c_str()); }
  const string& path() const { return path_; }
 private:
  string path_;
};

string UniformModel(const string& start, const string& transition,
                    const string& emission) {
  std::ostringstream stream;
  for (size_t i = 0; i < 4; ++i) stream << start << (i == 3 ? '\n' : ' ');
  for (size_t p = 0; p < 4; ++p) {
    for (size_t y = 0; y < 4; ++y)
      stream << transition << (y == 3 ? '\n' : ' ');
  }
  for (size_t y = 0; y < 4; ++y) stream << emission << '\n';
  return stream.str();
}

// Deliberately retain the baseline's full, state-major DP matrix as an oracle.
// This does not call the production optimized recurrence or emission accessors.
struct ReferenceMatrix {
  vector<uint8_t> states;
  vector<double> scores;
  vector<int> predecessors;
};

ReferenceMatrix ReferenceViterbi(const HMMModel& model,
                                 const RuneStrArray& runes) {
  const size_t n = runes.size();
  ReferenceMatrix result;
  result.states.resize(n);
  result.scores.resize(4 * n);
  result.predecessors.assign(4 * n, -1);
  vector<uint8_t>& states = result.states;
  vector<double>& scores = result.scores;
  vector<int>& predecessors = result.predecessors;
  if (!n) return result;
  for (size_t state = 0; state < 4; ++state) {
    EmitProbMap::const_iterator it = model.emitProbVec[state]->find(runes[0].rune);
    const double emission = it == model.emitProbVec[state]->end() ? MIN_DOUBLE : it->second;
    scores[state * n] = model.startProb[state] + emission;
  }
  for (size_t column = 1; column < n; ++column) {
    for (size_t state = 0; state < 4; ++state) {
      const size_t index = column + state * n;
      scores[index] = MIN_DOUBLE;
      predecessors[index] = HMMModel::E;
      EmitProbMap::const_iterator it = model.emitProbVec[state]->find(runes[column].rune);
      const double emission = it == model.emitProbVec[state]->end() ? MIN_DOUBLE : it->second;
      for (size_t previous = 0; previous < 4; ++previous) {
        const double candidate = scores[column - 1 + previous * n]
            + model.transProb[previous][state] + emission;
        if (candidate > scores[index]) {
          scores[index] = candidate;
          predecessors[index] = static_cast<int>(previous);
        }
      }
    }
  }
  size_t state = scores[n - 1 + HMMModel::E * n] >= scores[n - 1 + HMMModel::S * n]
      ? HMMModel::E : HMMModel::S;
  for (size_t column = n; column-- > 0;) {
    states[column] = static_cast<uint8_t>(state);
    if (column) state = predecessors[column + state * n];
  }
  return result;
}

void CheckNonAscii(const HMMModel& model, const string& sentence,
                   HmmScratch& scratch) {
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString(sentence, runes));
  const ReferenceMatrix reference = ReferenceViterbi(model, runes);
  const vector<uint8_t>& expected = reference.states;
  HMMSegment segment(&model);
  vector<WordRange> actual;
  segment.CutWithScratch(runes.begin(), runes.end(), actual, scratch);
  if (!runes.empty()) {
    EXPECT_EQ(expected, scratch.status) << sentence;
  }
  size_t left = 0;
  size_t word = 0;
  for (size_t right = 0; right < expected.size(); ++right) {
    if (expected[right] % 2) {
      ASSERT_LT(word, actual.size());
      EXPECT_EQ(runes.begin() + left, actual[word].left);
      EXPECT_EQ(runes.begin() + right, actual[word].right);
      left = right + 1;
      ++word;
    }
  }
  EXPECT_EQ(word, actual.size());

  // The rolling kernel stores time-major byte predecessors. Check every cell,
  // including unreachable states and the first-column no-predecessor sentinel.
  const size_t n = runes.size();
  if (!n) return;
  ASSERT_EQ(4 * n, scratch.path.size());
  for (size_t column = 0; column < n; ++column) {
    for (size_t state = 0; state < 4; ++state) {
      const int predecessor = reference.predecessors[column + state * n];
      EXPECT_EQ(predecessor < 0 ? uint8_t(0xff) : static_cast<uint8_t>(predecessor),
                scratch.path[column * 4 + state])
          << "column=" << column << ", state=" << state << ", input=" << sentence;
    }
  }

  // Running every prefix exposes its final rolling scores without adding a
  // tracing branch or a full score matrix to the production hot path. The last
  // prefix restores the full-input scratch state for callers' assertions.
  for (size_t column = 0; column < n; ++column) {
    vector<WordRange> prefix_words;
    segment.CutWithScratch(runes.begin(), runes.begin() + column + 1,
                           prefix_words, scratch);
    for (size_t state = 0; state < 4; ++state) {
      const double expected_score = reference.scores[column + state * n];
      if (std::isnan(expected_score)) {
        EXPECT_TRUE(std::isnan(scratch.prev[state]));
      } else {
        EXPECT_EQ(0, std::memcmp(&expected_score, &scratch.prev[state], sizeof(double)))
            << "column=" << column << ", state=" << state << ", input=" << sentence;
      }
    }
  }
}

void ExpectWordsEqual(const vector<Word>& expected, const vector<Word>& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(expected[i].word, actual[i].word);
    EXPECT_EQ(expected[i].offset, actual[i].offset);
    EXPECT_EQ(expected[i].unicode_offset, actual[i].unicode_offset);
    EXPECT_EQ(expected[i].unicode_length, actual[i].unicode_length);
  }
}

}  // namespace

TEST(HmmCpuTest, DenseAndBudgetFallbackPreserveStatesAndTies) {
  ModelFile file(UniformModel("0", "0", "甲:0,乙:0,丙:0,丁:0"));
  HMMModel dense(file.path(), true);
  HMMModel sparse(file.path(), true, 0);
  ASSERT_TRUE(dense.HasDenseEmissions());
  ASSERT_FALSE(sparse.HasDenseEmissions());
  ASSERT_TRUE(sparse.IsOptimizationEnabled());
  HmmScratch scratch;
  CheckNonAscii(dense, "甲乙丙丁", scratch);
  const uint8_t tie_states[] = {HMMModel::B, HMMModel::B, HMMModel::B, HMMModel::E};
  EXPECT_EQ(vector<uint8_t>(tie_states, tie_states + 4), scratch.status);
  CheckNonAscii(sparse, "甲乙丙丁", scratch);
  CheckNonAscii(dense, "甲", scratch);
  ASSERT_EQ(size_t(1), scratch.status.size());
  EXPECT_EQ(HMMModel::E, scratch.status[0]);
  EXPECT_EQ(uint8_t(0xff), scratch.path[0]);
  CheckNonAscii(dense, "", scratch);
}

TEST(HmmCpuTest, MissingEmissionsAndFiniteSentinelKeepDefaultPredecessor) {
  ModelFile file(UniformModel("-3.14e100", "-3.14e100", "中:-3.14e100"));
  HMMModel model(file.path(), true);
  HmmScratch scratch;
  CheckNonAscii(model, "甲乙丙丁", scratch);
  EXPECT_EQ(vector<uint8_t>(4, HMMModel::E), scratch.status);
  for (size_t i = 4; i < scratch.path.size(); ++i)
    EXPECT_EQ(HMMModel::E, scratch.path[i]);
  CheckNonAscii(model, "㐀中龿𠀀", scratch);
}

TEST(HmmCpuTest, AllFourPredecessorsAndExtremeProbabilitiesMatchFullMatrix) {
  const char* values[] = {"0", "1e300", "-1e300", "-3.14e100", "inf", "-inf", "nan"};
  for (size_t trial = 0; trial < 28; ++trial) {
    std::ostringstream data;
    for (size_t y = 0; y < 4; ++y) data << values[(trial + y) % 7] << (y == 3 ? '\n' : ' ');
    for (size_t p = 0; p < 4; ++p) {
      for (size_t y = 0; y < 4; ++y)
        data << values[(trial + p * 3 + y * 2) % 7] << (y == 3 ? '\n' : ' ');
    }
    for (size_t y = 0; y < 4; ++y) {
      data << "甲:" << values[(trial + y * 2) % 7]
           << ",乙:" << values[(trial + y * 3 + 1) % 7]
           << ",丙:" << values[(trial + y + 2) % 7] << '\n';
    }
    ModelFile file(data.str());
    HMMModel dense(file.path(), true);
    HMMModel sparse(file.path(), true, 0);
    HmmScratch scratch;
    CheckNonAscii(dense, "甲乙丙甲丙乙丁甲乙", scratch);
    CheckNonAscii(sparse, "甲乙丙甲丙乙丁甲乙", scratch);
  }
}

TEST(HmmCpuTest, FreezeRejectsModelReloadAndEmissionMutation) {
  ModelFile file(UniformModel("0", "0", "甲:0,乙:0"));
  HMMModel model(file.path(), true);
  ASSERT_TRUE(model.IsFrozen());
  EXPECT_THROW(model.LoadModel(file.path()), std::logic_error);
  EXPECT_THROW(model.LoadEmitProb("甲:99", model.emitProbB), std::logic_error);
  HmmScratch scratch;
  CheckNonAscii(model, "甲乙", scratch);
  HMMModel legacy(file.path());
  ASSERT_FALSE(legacy.IsFrozen());
  EXPECT_NO_THROW(legacy.LoadModel(file.path()));
}

TEST(HmmCpuTest, SparseUnicodeModelFallsBackWithinBudget) {
  ModelFile file(UniformModel("0", "0", "一:0,𠀀:-1"));
  HMMModel model(file.path(), true, 1024);
  EXPECT_FALSE(model.HasDenseEmissions());
  EXPECT_TRUE(model.IsOptimizationEnabled());
  HmmScratch scratch;
  CheckNonAscii(model, "一𠀀一𠀀", scratch);
}

TEST(HmmCpuTest, RealCorpusAsciiRulesOffsetsAndScratchReuse) {
  HMMModel legacy(DICT_DIR "/hmm_model.utf8");
  HMMModel dense(DICT_DIR "/hmm_model.utf8", true);
  HMMModel sparse(DICT_DIR "/hmm_model.utf8", true, 0);
  ASSERT_TRUE(dense.HasDenseEmissions());
  HMMSegment baseline(&legacy), optimized(&dense), fallback(&sparse);
  const char* corpus[] = {
    "他来到了网易杭研大厦", "我来自北京邮电大学。。。学号123456，用AK47",
    "我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。",
    "中文abc1.2混合5G3.14文本123.456.789以及ABC123测试", "𠀀㐀罕见字与模型缺失字符🙂",
    "abc1.2 5G3.14 123abc.45 3.14.15 _\n", "甲", ""
  };
  HmmScratch scratch;
  for (size_t i = 0; i < sizeof(corpus) / sizeof(*corpus); ++i) {
    vector<Word> expected, actual, sparse_actual;
    baseline.Cut(corpus[i], expected);
    optimized.Cut(corpus[i], actual);
    fallback.Cut(corpus[i], sparse_actual);
    ExpectWordsEqual(expected, actual);
    ExpectWordsEqual(expected, sparse_actual);
  }
  CheckNonAscii(dense, "中华人民共和国北京邮电大学杭州网易研究院", scratch);
  const size_t path_capacity = scratch.path.capacity();
  const size_t status_capacity = scratch.status.capacity();
  CheckNonAscii(dense, "北京大学", scratch);
  EXPECT_EQ(path_capacity, scratch.path.capacity());
  EXPECT_EQ(status_capacity, scratch.status.capacity());
}
