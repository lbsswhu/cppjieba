#include "gtest/gtest.h"
#include "cppjieba/FusedMPCut.hpp"
#include "cppjieba/DatBuilder.hpp"
#include <cstring>
#include <cmath>
#include <type_traits>

using namespace cppjieba;

static_assert(!std::is_copy_constructible<Trie>::value,
              "copying a read-only Trie must not expose mutable shared nodes");

TEST(FusedCPU, EveryPositionMatchesDagIncludingTiesAndRingWrap) {
  std::vector<DictUnit> values(4);
  const char* keys[] = {"a", "ab", "abcd", "b"};
  const double weights[] = {-1.0, -2.0, -4.0, -1.0};
  std::vector<Unicode> words;
  std::vector<const DictUnit*> ptrs;
  for (size_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(DecodeUTF8RunesInString(keys[i], values[i].word));
    values[i].weight = weights[i];
    words.push_back(values[i].word);
    ptrs.push_back(&values[i]);
  }
  Trie trie(words, ptrs);
  DatBuildResult dat = DatBuilder::Build(ptrs, -1.0);
  ASSERT_TRUE(dat.model.get() != NULL) << dat.stats.reason;
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("abcdabcdxxxabcdabcdabcd", runes));
  for (size_t limit = 1; limit <= 4; ++limit) {
    std::vector<Dag> dag;
    trie.Find(runes.begin(), runes.end(), dag, limit);
    std::vector<double> scores(dag.size() + 1, 0.0);
    std::vector<uint16_t> lengths(dag.size(), 1);
    for (size_t i = dag.size(); i-- > 0;) {
      scores[i] = MIN_DOUBLE;
      for (size_t j = 0; j < dag[i].nexts.size(); ++j) {
        const std::pair<size_t, const DictUnit*>& edge = dag[i].nexts[j];
        double score = 0.0;
        if (edge.first + 1 < dag.size()) score += scores[edge.first + 1];
        score += edge.second ? edge.second->weight : -1.0;
        if (score > scores[i]) {
          scores[i] = score;
          lengths[i] = static_cast<uint16_t>(edge.first - i + 1);
        }
      }
    }
    MPCutScratch scratch;
    std::vector<WordRange> out(1, WordRange(runes.begin(), runes.begin()));
    PointerWalker walker(trie, runes.begin());
    CutRangeFused(runes.begin(), runes.size(), limit, -1.0, walker, scratch, out);
    EXPECT_EQ(lengths, scratch.bestLen);
    for (size_t i = 0; i < limit + 1; ++i)
      EXPECT_EQ(0, std::memcmp(&scores[i], &scratch.dpRing[i], sizeof(double)));
    EXPECT_EQ(runes.begin(), out.front().left);
    // Recompute every suffix to expose each position's score before its ring
    // slot can be overwritten. This compares against the independent DAG DP.
    for (size_t i = 0; i < runes.size(); ++i) {
      PointerWalker suffix(trie, runes.begin() + i);
      std::vector<WordRange> suffixOut;
      CutRangeFused(runes.begin() + i, runes.size() - i,
          std::min(limit, runes.size() - i), -1.0, suffix, scratch, suffixOut);
      EXPECT_EQ(0, std::memcmp(&scores[i], &scratch.dpRing[0], sizeof(double)));
    }
    RawDatWalker raw(*dat.model, runes.begin());
    std::vector<WordRange> datOut;
    CutRangeFused(runes.begin(), runes.size(), limit, -1.0, raw, scratch, datOut);
    EXPECT_EQ(lengths, scratch.bestLen);
    for (size_t i = 0; i < runes.size(); ++i) {
      RawDatWalker suffix(*dat.model, runes.begin() + i);
      std::vector<WordRange> suffixOut;
      CutRangeFused(runes.begin() + i, runes.size() - i,
          std::min(limit, runes.size() - i), -1.0, suffix, scratch, suffixOut);
      EXPECT_EQ(0, std::memcmp(&scores[i], &scratch.dpRing[0], sizeof(double)));
    }
  }
}

TEST(FusedCPU, FiniteMinimumDoesNotUnconditionallyChooseFirstCandidate) {
  DictUnit value;
  DecodeUTF8RunesInString("aa", value.word);
  value.weight = MIN_DOUBLE * 2;
  std::vector<Unicode> words(1, value.word);
  std::vector<const DictUnit*> ptrs(1, &value);
  Trie trie(words, ptrs);
  RuneStrArray runes;
  DecodeUTF8RunesInString("aaaaaa", runes);
  PointerWalker walker(trie, runes.begin());
  MPCutScratch scratch;
  std::vector<WordRange> out;
  CutRangeFused(runes.begin(), runes.size(), 2, MIN_DOUBLE * 2, walker, scratch, out);
  EXPECT_EQ(std::vector<uint16_t>(runes.size(), 1), scratch.bestLen);
  EXPECT_EQ(runes.size(), out.size());
}

namespace {
uint64_t CpuWeightBits(double weight) {
  uint64_t bits;
  std::memcpy(&bits, &weight, sizeof(bits));
  return bits;
}

template <class Walker>
void CompareCandidatesAndDP(Walker& walker, RuneStrArray::const_iterator begin,
                            const std::vector<Dag>& dag, size_t requested,
                            double unknownWeight) {
  const size_t limit = std::max(size_t(1), std::min(requested, dag.size()));
  std::vector<double> scores(dag.size() + 1, 0.0);
  std::vector<uint16_t> lengths(dag.size(), 1);
  for (size_t i = dag.size(); i-- > 0;) {
    walker.Reset();
    size_t candidate = 0;
    for (size_t len = 1; len <= std::min(limit, dag.size() - i); ++len) {
      const bool alive = walker.Step(i + len - 1);
      const bool terminal = alive && walker.IsTerminal();
      if (len == 1 || terminal) {
        ASSERT_LT(candidate, dag[i].nexts.size());
        const std::pair<size_t, const DictUnit*>& expected = dag[i].nexts[candidate++];
        EXPECT_EQ(expected.first - i + 1, len);
        EXPECT_EQ(expected.second != NULL, terminal);
        EXPECT_EQ(CpuWeightBits(expected.second ? expected.second->weight : unknownWeight),
                  CpuWeightBits(terminal ? walker.TerminalWeight() : unknownWeight));
      }
      if (!alive) break;
    }
    EXPECT_EQ(dag[i].nexts.size(), candidate);
    scores[i] = MIN_DOUBLE;
    for (size_t j = 0; j < dag[i].nexts.size(); ++j) {
      const std::pair<size_t, const DictUnit*>& edge = dag[i].nexts[j];
      double score = 0.0;
      if (edge.first + 1 < dag.size()) score += scores[edge.first + 1];
      score += edge.second ? edge.second->weight : unknownWeight;
      if (score > scores[i]) {
        scores[i] = score;
        lengths[i] = static_cast<uint16_t>(edge.first - i + 1);
      }
    }
  }
  MPCutScratch scratch;
  std::vector<WordRange> out;
  CutRangeFused(begin, dag.size(), limit, unknownWeight, walker, scratch, out);
  EXPECT_EQ(lengths, scratch.bestLen);
  for (size_t i = 0; i < std::min(dag.size(), limit + 1); ++i)
    EXPECT_EQ(CpuWeightBits(scores[i]), CpuWeightBits(scratch.dpRing[i]));
}
} // namespace

TEST(FusedCPU, EveryCandidateMatchesIncludingPrefixesSignedZeroAndCloseWeights) {
  std::vector<std::string> keys = {"a", "ab", "abcd", "qwer"};
  for (size_t n = 1; n <= 24; ++n) keys.push_back(std::string(n, 'x'));
  std::vector<DictUnit> values(keys.size());
  std::vector<Unicode> words;
  std::vector<const DictUnit*> ptrs;
  for (size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(DecodeUTF8RunesInString(keys[i], values[i].word));
    values[i].weight = i == 0 ? -0.0 : i == 1 ? 0.0 :
        i == 2 ? std::nextafter(-2.0, 0.0) : -double(i);
    words.push_back(values[i].word);
    ptrs.push_back(&values[i]);
  }
  Trie trie(words, ptrs);
  DatBuildResult dat = DatBuilder::Build(ptrs, -40.0);
  ASSERT_TRUE(dat.model.get() != NULL) << dat.stats.reason;
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("abcd!qwe?" + std::string(28, 'x'), runes));
  const size_t limits[] = {0, 1, 2, 4, 16, 24, 512};
  for (size_t k = 0; k < sizeof(limits) / sizeof(*limits); ++k) {
    std::vector<Dag> dag;
    trie.Find(runes.begin(), runes.end(), dag, limits[k]);
    PointerWalker pointer(trie, runes.begin());
    RawDatWalker raw(*dat.model, runes.begin());
    CompareCandidatesAndDP(pointer, runes.begin(), dag, limits[k], -40.0);
    CompareCandidatesAndDP(raw, runes.begin(), dag, limits[k], -40.0);
  }
}
