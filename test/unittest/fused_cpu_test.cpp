#include "gtest/gtest.h"
#include "cppjieba/FusedMPCut.hpp"
#include "cppjieba/DatBuilder.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

using namespace cppjieba;

namespace {
uint64_t WeightBits(double weight) {
  uint64_t bits;
  std::memcpy(&bits, &weight, sizeof(bits));
  return bits;
}

DictUnit MakeWord(const std::string& text, double weight) {
  DictUnit value;
  EXPECT_TRUE(DecodeUTF8RunesInString(text, value.word));
  value.weight = weight;
  return value;
}

DatBuildResult Build(const std::vector<DictUnit>& values, double unknownWeight) {
  std::vector<const DictUnit*> pointers;
  for (size_t i = 0; i < values.size(); ++i) pointers.push_back(&values[i]);
  DatBuildOptions options;
  options.max_build_ms = 60000;
  return DatBuilder::Build(pointers, unknownWeight, options);
}

struct Candidate {
  size_t length;
  bool terminal;
  double weight;
};

// Independent word-list oracle: examine complete words directly, retaining the
// last source entry at each matching length. There is no trie or DAT traversal.
std::vector<Candidate> WordListCandidates(const std::vector<DictUnit>& words,
                                          const RuneStrArray& runes,
                                          size_t start, size_t requested,
                                          double unknownWeight) {
  const size_t limit = std::max(size_t(1), std::min(requested, runes.size() - start));
  std::map<size_t, const DictUnit*> matches;
  matches[1] = NULL;
  for (size_t source = 0; source < words.size(); ++source) {
    const Unicode& word = words[source].word;
    if (word.empty() || word.size() > limit) continue;
    size_t rune = 0;
    while (rune < word.size() && word[rune] == runes[start + rune].rune) ++rune;
    if (rune == word.size()) matches[word.size()] = &words[source];
  }
  std::vector<Candidate> result;
  for (std::map<size_t, const DictUnit*>::const_iterator it = matches.begin();
       it != matches.end(); ++it) {
    result.push_back(Candidate{it->first, it->second != NULL,
                              it->second ? it->second->weight : unknownWeight});
  }
  return result;
}

struct Reference {
  std::vector<std::vector<Candidate> > candidates;
  std::vector<double> scores;
  std::vector<size_t> lengths;
};

Reference WordListDP(const std::vector<DictUnit>& words, const RuneStrArray& runes,
                     size_t requested, double unknownWeight) {
  Reference result;
  result.candidates.resize(runes.size());
  result.scores.assign(runes.size() + 1, 0.0);
  result.lengths.assign(runes.size(), 1);
  for (size_t i = runes.size(); i-- > 0;) {
    result.candidates[i] = WordListCandidates(words, runes, i, requested, unknownWeight);
    result.scores[i] = MIN_DOUBLE;
    for (size_t candidate = 0; candidate < result.candidates[i].size(); ++candidate) {
      const Candidate& edge = result.candidates[i][candidate];
      double score = 0.0;
      if (i + edge.length < runes.size()) score += result.scores[i + edge.length];
      score += edge.weight;
      if (score > result.scores[i]) {
        result.scores[i] = score;
        result.lengths[i] = edge.length;
      }
    }
  }
  return result;
}

size_t BestLength(const MPCutScratch& scratch, size_t i, size_t limit) {
  return limit <= UINT16_MAX ? scratch.bestLen[i] : scratch.bestLenWide[i];
}

void Compare(const std::vector<DictUnit>& words, const DatModel& model,
              const RuneStrArray& runes, size_t requested, double unknownWeight,
              MPCutScratch& scratch) {
  const Reference reference = WordListDP(words, runes, requested, unknownWeight);
  const size_t n = runes.size();
  const size_t limit = std::max(size_t(1), std::min(requested, n));
  RawDatWalker walker(model, runes.begin());
  for (size_t i = 0; i < n; ++i) {
    walker.Reset();
    size_t candidate = 0;
    for (size_t len = 1; len <= std::min(limit, n - i); ++len) {
      const bool alive = walker.Step(i + len - 1);
      const bool terminal = alive && walker.IsTerminal();
      if (len == 1 || terminal) {
        ASSERT_LT(candidate, reference.candidates[i].size());
        const Candidate& expected = reference.candidates[i][candidate++];
        EXPECT_EQ(expected.length, len) << "position=" << i;
        EXPECT_EQ(expected.terminal, terminal) << "position=" << i;
        EXPECT_EQ(WeightBits(expected.weight),
                  WeightBits(terminal ? walker.TerminalWeight() : unknownWeight));
      }
      if (!alive) break;
    }
    EXPECT_EQ(reference.candidates[i].size(), candidate);
  }

  std::vector<WordRange> out;
  if (n) out.push_back(WordRange(runes.begin(), runes.begin()));
  CutRangeFused(runes.begin(), n, limit, unknownWeight, walker, scratch, out);
  if (!n) {
    EXPECT_TRUE(out.empty());
    return;
  }
  ASSERT_EQ(n, limit <= UINT16_MAX ? scratch.bestLen.size() : scratch.bestLenWide.size());
  for (size_t i = 0; i < n; ++i) EXPECT_EQ(reference.lengths[i], BestLength(scratch, i, limit));
  for (size_t i = 0; i < std::min(n + 1, limit + 1); ++i)
    EXPECT_EQ(WeightBits(reference.scores[i]), WeightBits(scratch.dpRing[i]));
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(runes.begin(), out.front().left);
  EXPECT_EQ(runes.begin(), out.front().right);
  size_t output = 1;
  for (size_t i = 0; i < n; i += reference.lengths[i]) {
    ASSERT_LT(output, out.size());
    EXPECT_EQ(runes.begin() + i, out[output].left);
    EXPECT_EQ(runes.begin() + i + reference.lengths[i] - 1, out[output].right);
    ++output;
  }
  EXPECT_EQ(output, out.size());

  // Each suffix exposes its first score before ring reuse can overwrite it.
  for (size_t i = 0; i < n; ++i) {
    RawDatWalker suffix(model, runes.begin() + i);
    std::vector<WordRange> suffixOut;
    const size_t suffixLimit = std::min(limit, n - i);
    CutRangeFused(runes.begin() + i, n - i, suffixLimit,
                  unknownWeight, suffix, scratch, suffixOut);
    EXPECT_EQ(WeightBits(reference.scores[i]), WeightBits(scratch.dpRing[0]));
    EXPECT_EQ(reference.lengths[i], BestLength(scratch, 0, suffixLimit));
  }
}
} // namespace

TEST(FusedCPU, EveryPositionMatchesWordListIncludingTiesAndRingWrap) {
  const std::vector<DictUnit> words = {MakeWord("a", -1), MakeWord("ab", -2),
                                       MakeWord("abcd", -4), MakeWord("b", -1)};
  DatBuildResult dat = Build(words, -1);
  ASSERT_TRUE(dat.model.get() != NULL) << dat.stats.reason;
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("abcdabcdxxxabcdabcdabcd", runes));
  MPCutScratch scratch;
  for (size_t limit = 1; limit <= 4; ++limit) Compare(words, *dat.model, runes, limit, -1, scratch);
  ASSERT_TRUE(DecodeUTF8RunesInString("a", runes));
  Compare(words, *dat.model, runes, 512, -1, scratch);
  runes.clear();
  Compare(words, *dat.model, runes, 512, -1, scratch);
}

TEST(FusedCPU, FiniteMinimumDoesNotUnconditionallyChooseFirstCandidate) {
  const std::vector<DictUnit> words(1, MakeWord("aa", MIN_DOUBLE * 2));
  DatBuildResult dat = Build(words, MIN_DOUBLE * 2);
  ASSERT_TRUE(dat.model.get() != NULL) << dat.stats.reason;
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("aaaaaa", runes));
  MPCutScratch scratch;
  Compare(words, *dat.model, runes, 2, MIN_DOUBLE * 2, scratch);
}

TEST(FusedCPU, EveryCandidateMatchesPrefixesDuplicatesSignedZeroAndCloseWeights) {
  std::vector<DictUnit> words = {MakeWord("a", -9), MakeWord("ab", 0.0),
      MakeWord("abcd", std::nextafter(-2.0, 0.0)), MakeWord("qwer", -4), MakeWord("a", -0.0)};
  for (size_t n = 1; n <= 24; ++n) words.push_back(MakeWord(std::string(n, 'x'), -double(n)));
  DatBuildResult dat = Build(words, -40);
  ASSERT_TRUE(dat.model.get() != NULL) << dat.stats.reason;
  RuneStrArray runes;
  ASSERT_TRUE(DecodeUTF8RunesInString("abcd!qwe?" + std::string(28, 'x'), runes));
  ASSERT_GT(WordListCandidates(words, runes, 9, 512, -40).size(), size_t(16));
  const size_t limits[] = {0, 1, 2, 4, 16, 24, 512, std::numeric_limits<size_t>::max()};
  MPCutScratch scratch;
  for (size_t k = 0; k < sizeof(limits) / sizeof(*limits); ++k)
    Compare(words, *dat.model, runes, limits[k], -40, scratch);
}

TEST(FusedCPU, WideLengthsRetainLongWordAndReuseNarrowScratch) {
  const size_t length = size_t(UINT16_MAX) + 2;
  DictUnit longWord;
  longWord.word.push_back(0x7532); // Only this unique first Rune starts a word.
  for (size_t i = 1; i < length; ++i) longWord.word.push_back(0x4e59);
  longWord.weight = -1.0;
  const std::vector<DictUnit> words(1, longWord);
  DatBuildResult dat = Build(words, -2);
  ASSERT_TRUE(dat.model.get() != NULL) << dat.stats.reason;
  RuneStrArray runes;
  for (size_t i = 0; i < length; ++i)
    runes.push_back(RuneStr(longWord.word[i], static_cast<uint32_t>(i * 3), 3,
                            static_cast<uint32_t>(i), 1));
  MPCutScratch scratch;
  const size_t limits[] = {UINT16_MAX, size_t(UINT16_MAX) + 1, length};
  for (size_t variant = 0; variant < sizeof(limits) / sizeof(*limits); ++variant) {
    const size_t limit = limits[variant];
    const Reference reference = WordListDP(words, runes, limit, -2);
    RawDatWalker walker(*dat.model, runes.begin());
    std::vector<WordRange> out;
    CutRangeFused(runes.begin(), runes.size(), limit, -2, walker, scratch, out);
    ASSERT_EQ(limit == length ? size_t(1) : length, out.size());
    EXPECT_EQ(runes.begin(), out.front().left);
    EXPECT_EQ(runes.end() - 1, out.back().right);
    ASSERT_EQ(length, limit <= UINT16_MAX ? scratch.bestLen.size() : scratch.bestLenWide.size());
    EXPECT_TRUE(limit <= UINT16_MAX ? scratch.bestLenWide.empty() : scratch.bestLen.empty());
    EXPECT_EQ(limit == length ? length : size_t(1), BestLength(scratch, 0, limit));
    for (size_t i = 0; i < length; ++i)
      EXPECT_EQ(reference.lengths[i], BestLength(scratch, i, limit));
    for (size_t i = 0; i < std::min(length, limit + 1); ++i)
      EXPECT_EQ(WeightBits(reference.scores[i]), WeightBits(scratch.dpRing[i]));
  }
  // The long fixture stays linear: all suffix starts miss on their first Rune.
  // A short follow-up checks switching from wide back to compact scratch.
  runes.resize(3);
  Compare(words, *dat.model, runes, 3, -2, scratch);
  EXPECT_TRUE(scratch.bestLenWide.empty());
}
