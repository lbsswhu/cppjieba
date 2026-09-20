#include "cppjieba/DatBuilder.hpp"
#include "gtest/gtest.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace cppjieba;

namespace {
DictUnit Unit(std::initializer_list<Rune> runes, double weight) {
  DictUnit unit;
  for (Rune rune : runes) unit.word.push_back(rune);
  unit.weight = weight;
  return unit;
}

uint64_t Bits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

DatBuildResult Build(const std::vector<DictUnit>& units,
                     const DatBuildOptions& options = DatBuildOptions(),
                     double unknown = -20.0) {
  std::vector<const DictUnit*> input;
  for (const DictUnit& unit : units) input.push_back(&unit);
  return DatBuilder::Build(input, unknown, options);
}

DatCursor Lookup(const DatModel& model, const Unicode& word) {
  DatCursor cursor = model.Root();
  for (size_t i = 0; i < word.size(); ++i) {
    EXPECT_TRUE(model.StepRaw(word[i], cursor));
  }
  return cursor;
}
}  // namespace

TEST(DatCpuTest, RootNegativeBaseAndFailedTransitionAreSafe) {
  std::vector<DictUnit> units{Unit({100, 103}, -1.0), Unit({108}, -2.0)};
  DatBuildResult result = Build(units);
  ASSERT_TRUE(result.model);
  EXPECT_EQ(DatBuildStatus::Success, result.stats.status);
  const DatModel& model = *result.model;
  DatCursor root = model.Root();
  EXPECT_EQ(0u, root.state);
  EXPECT_LT(dat_detail::DecodeBase(root.unit), 0);
  EXPECT_FALSE(model.IsTerminal(root));
  EXPECT_FALSE(model.StepRaw(99, root));  // Addresses reserved root slot.
  EXPECT_EQ(0u, root.state);
  EXPECT_FALSE(model.StepRaw(101, root));  // Occupied by a different parent.
  EXPECT_FALSE(model.StepRaw(102, root));  // Empty slot must use NO_CHECK.
  EXPECT_EQ(model.Root().unit, root.unit);
  EXPECT_FALSE(model.StepRaw(std::numeric_limits<Rune>::max(), root));
  ASSERT_TRUE(model.StepRaw(100, root));
  EXPECT_FALSE(model.IsTerminal(root));
  ASSERT_TRUE(model.StepRaw(103, root));
  EXPECT_TRUE(model.IsTerminal(root));
  EXPECT_EQ(-1.0, model.TerminalWeight(root));
  EXPECT_EQ(0, dat_detail::DecodeBase(root.unit));
  EXPECT_EQ(0u, root.unit >> 56);
  EXPECT_EQ(2u, model.ActualMaxWordLen());
}

TEST(DatCpuTest, LastDuplicateWinsAndWeightsPreserveBits) {
  std::vector<DictUnit> units{Unit({}, -100.0), Unit({1}, -7.0),
                            Unit({2}, +0.0), Unit({1}, -0.0),
                            Unit({3}, std::nextafter(-1.0, 0.0))};
  DatBuildResult result = Build(units, DatBuildOptions(), -123.125);
  ASSERT_TRUE(result.model);
  EXPECT_EQ(3u, result.stats.unique_weights);
  EXPECT_FALSE(result.model->IsTerminal(result.model->Root()));
  EXPECT_EQ(Bits(-123.125), Bits(result.model->UnknownWeight()));
  for (size_t i = 2; i < units.size(); ++i) {
    DatCursor cursor = Lookup(*result.model, units[i].word);
    ASSERT_TRUE(result.model->IsTerminal(cursor));
    EXPECT_EQ(Bits(units[i].weight), Bits(result.model->TerminalWeight(cursor)));
  }
}

TEST(DatCpuTest, EmptyDictionaryAndLongWord) {
  DatBuildResult empty = Build({Unit({}, std::numeric_limits<double>::infinity())});
  ASSERT_TRUE(empty.model);
  EXPECT_EQ(1u, empty.stats.slot_count);
  EXPECT_EQ(0u, empty.model->ActualMaxWordLen());
  DatCursor cursor = empty.model->Root();
  EXPECT_FALSE(empty.model->StepRaw(0, cursor));
  DictUnit long_word;
  for (size_t i = 0; i < 1024; ++i) long_word.word.push_back(42);
  long_word.weight = -3.0;
  DatBuildResult long_result = Build({long_word});
  ASSERT_TRUE(long_result.model);
  EXPECT_EQ(1024u, long_result.model->ActualMaxWordLen());
  EXPECT_TRUE(long_result.model->IsTerminal(Lookup(*long_result.model, long_word.word)));
}

TEST(DatCpuTest, NonfiniteEffectiveWeightsAreUnsupported) {
  const double bad[] = {std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()};
  for (double weight : bad) {
    DatBuildResult result = Build({Unit({1}, weight)});
    EXPECT_FALSE(result.model);
    EXPECT_EQ(DatBuildStatus::Unsupported, result.stats.status);
    result = Build({Unit({1}, -1)}, DatBuildOptions(), weight);
    EXPECT_FALSE(result.model);
    EXPECT_EQ(DatBuildStatus::Unsupported, result.stats.status);
  }
  EXPECT_TRUE(Build({Unit({1}, bad[0]), Unit({1}, -1)}).model);
}

TEST(DatCpuTest, ExplicitBudgetsDiscardTheEntireModel) {
  std::vector<DictUnit> units{Unit({0, 64}, -1), Unit({1, 65}, -2)};
  DatBuildOptions options;
  options.max_slots = 2;
  DatBuildResult result = Build(units, options);
  EXPECT_FALSE(result.model);
  EXPECT_EQ(DatBuildStatus::ResourceLimit, result.stats.status);
  options = DatBuildOptions();
  options.max_unique_weights = 1;
  result = Build(units, options);
  EXPECT_FALSE(result.model);
  EXPECT_EQ(DatBuildStatus::FieldLimit, result.stats.status);
  options = DatBuildOptions();
  options.max_temporary_bytes = 1;
  result = Build(units, options);
  EXPECT_FALSE(result.model);
  EXPECT_EQ(DatBuildStatus::ResourceLimit, result.stats.status);
  options = DatBuildOptions();
  options.max_build_ms = 0;
  result = Build(units, options);
  EXPECT_FALSE(result.model);
  EXPECT_EQ(DatBuildStatus::ResourceLimit, result.stats.status);
  EXPECT_FALSE(result.stats.reason.empty());
}

TEST(DatCpuTest, PackedBaseAndSpanLimitsReturnStructuredFailure) {
  DatBuildResult result = Build({Unit({std::numeric_limits<Rune>::max()}, -1)});
  EXPECT_FALSE(result.model);
  EXPECT_EQ(DatBuildStatus::FieldLimit, result.stats.status);
  result = Build({Unit({0}, -1), Unit({dat_detail::NO_CHECK}, -1)});
  EXPECT_FALSE(result.model);
  EXPECT_EQ(DatBuildStatus::FieldLimit, result.stats.status);
}

TEST(DatCpuTest, BitmapCrossWordReadsGrowthAndLowestPlacement) {
  // Root is the largest row. The next row needs free q, q+63, q+64.
  std::vector<DictUnit> units{Unit({0}, -1), Unit({3}, -1), Unit({8}, -1),
                            Unit({64}, -1), Unit({65535}, -1),
                            Unit({3, 0}, -2), Unit({3, 63}, -2),
                            Unit({3, 64}, -2)};
  DatBuildResult result = Build(units);
  DatBuildResult repeated = Build(units);
  ASSERT_TRUE(result.model);
  ASSERT_TRUE(repeated.model);
  EXPECT_GT(result.stats.slot_count, 65536u);
  EXPECT_EQ(1, dat_detail::DecodeBase(result.model->Root().unit));
  DatCursor branch = result.model->Root();
  ASSERT_TRUE(result.model->StepRaw(3, branch));
  // Anchors 1 and 4 are occupied. Anchor 2 collides at 65; 3 is lowest fit.
  EXPECT_EQ(3, dat_detail::DecodeBase(branch.unit));
  for (const DictUnit& unit : units) {
    DatCursor a = Lookup(*result.model, unit.word);
    DatCursor b = Lookup(*repeated.model, unit.word);
    EXPECT_TRUE(result.model->IsTerminal(a));
    EXPECT_EQ(a.state, b.state);
    EXPECT_EQ(a.unit, b.unit);
  }
}

TEST(DatCpuTest, PartialBitmapTailCannotSupplyOutOfCapacityBits) {
  const std::vector<DictUnit> units{Unit({0, 0}, -1), Unit({0, 64}, -1),
                                  Unit({65}, -1)};
  DatBuildOptions options;
  options.max_slots = 67;
  DatBuildResult limited = Build(units, options);
  EXPECT_FALSE(limited.model);
  EXPECT_EQ(DatBuildStatus::ResourceLimit, limited.stats.status);
  options.max_slots = 68;
  DatBuildResult fits = Build(units, options);
  ASSERT_TRUE(fits.model);
  EXPECT_EQ(68u, fits.stats.slot_count);
  EXPECT_EQ(67u, Lookup(*fits.model, units[1].word).state);
}

TEST(DatCpuTest, LastLegalWeightAndNegativeBaseValuesRemainUsable) {
  std::vector<DictUnit> units;
  for (uint32_t i = 0; i < dat_detail::NO_WEIGHT; ++i)
    units.push_back(Unit({i}, -static_cast<double>(i)));
  DatBuildResult valid = Build(units);
  ASSERT_TRUE(valid.model);
  EXPECT_EQ(dat_detail::NO_WEIGHT, valid.stats.unique_weights);
  const DatCursor last = Lookup(*valid.model, units.back().word);
  EXPECT_TRUE(valid.model->IsTerminal(last));
  EXPECT_EQ(dat_detail::NO_WEIGHT - 1, dat_detail::DecodeWeightCode(last.unit));
  units.push_back(Unit({dat_detail::NO_WEIGHT}, -static_cast<double>(dat_detail::NO_WEIGHT)));
  DatBuildResult overflow = Build(units);
  EXPECT_FALSE(overflow.model);
  EXPECT_EQ(DatBuildStatus::FieldLimit, overflow.stats.status);
  valid = Build({Unit({(1U << 21) + 1}, -1)});
  ASSERT_TRUE(valid.model);
  EXPECT_EQ(-(1 << 21), dat_detail::DecodeBase(valid.model->Root().unit));
  overflow = Build({Unit({(1U << 21) + 2}, -1)});
  EXPECT_FALSE(overflow.model);
  EXPECT_EQ(DatBuildStatus::FieldLimit, overflow.stats.status);
}
