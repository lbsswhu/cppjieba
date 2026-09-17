#include "cppjieba/BitmapDoubleArrayTrie.hpp"
#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

using cppjieba::BitmapDoubleArrayTrie;
using cppjieba::Rune;
using cppjieba::Unicode;

namespace {

Unicode Runes(std::initializer_list<Rune> values) {
  return Unicode(values.begin(), values.end());
}

uint32_t Lookup(const BitmapDoubleArrayTrie& trie, const Unicode& word) {
  uint32_t state = 0;
  for (size_t i = 0; i < word.size(); ++i) {
    state = trie.Transition(state, word[i]);
  }
  return state;
}

uint64_t Bits(double weight) {
  uint64_t bits;
  std::memcpy(&bits, &weight, sizeof(bits));
  return bits;
}

// Deliberately simple scalar placement oracle, independent of bitmap shifts.
// The fixtures are small; production construction uses word intersections.
std::vector<uint64_t> ReferenceUnits(const std::vector<Unicode>& keys) {
  struct Node {
    std::map<Rune, uint32_t> children;
    uint32_t state;
    int64_t base;
    bool terminal;
    Node() : state(0), base(0), terminal(false) {}
  };
  std::vector<Unicode> sorted(keys);
  std::sort(sorted.begin(), sorted.end(), [](const Unicode& a, const Unicode& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
  });
  std::vector<Node> nodes(1);
  for (size_t i = 0; i < sorted.size(); ++i) {
    uint32_t parent = 0;
    for (size_t j = 0; j < sorted[i].size(); ++j) {
      Rune rune = sorted[i][j];
      if (!nodes[parent].children.count(rune)) {
        uint32_t child = static_cast<uint32_t>(nodes.size());
        nodes[parent].children[rune] = child;
        nodes.push_back(Node());
      }
      parent = nodes[parent].children[rune];
    }
    if (!sorted[i].empty()) nodes[parent].terminal = true;
  }
  std::vector<uint32_t> rows;
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (!nodes[i].children.empty()) rows.push_back(static_cast<uint32_t>(i));
  }
  std::sort(rows.begin(), rows.end(), [&nodes](uint32_t a, uint32_t b) {
    if (nodes[a].children.size() != nodes[b].children.size()) {
      return nodes[a].children.size() > nodes[b].children.size();
    }
    Rune sa = nodes[a].children.rbegin()->first - nodes[a].children.begin()->first;
    Rune sb = nodes[b].children.rbegin()->first - nodes[b].children.begin()->first;
    return sa != sb ? sa > sb : a < b;
  });
  std::vector<bool> occupied(65536, false);
  occupied[0] = true;
  size_t high_water = 1;
  for (size_t i = 0; i < rows.size(); ++i) {
    Node& row = nodes[rows[i]];
    Rune first = row.children.begin()->first;
    Rune span = row.children.rbegin()->first - first;
    if (static_cast<size_t>(span) + 2 > occupied.size()) {
      occupied.resize(((static_cast<size_t>(span) + 2 + 65535) / 65536) * 65536, false);
    }
    size_t anchor = 0;
    for (;;) {
      bool found = false;
      for (anchor = 1; anchor + span < occupied.size(); ++anchor) {
        bool fits = true;
        for (std::map<Rune, uint32_t>::const_iterator it = row.children.begin();
             it != row.children.end(); ++it) {
          if (occupied[anchor + it->first - first]) { fits = false; break; }
        }
        if (fits) { found = true; break; }
      }
      if (found) break;
      occupied.resize(occupied.size() + 65536, false);
    }
    row.base = static_cast<int64_t>(anchor) - first;
    for (std::map<Rune, uint32_t>::const_iterator it = row.children.begin();
         it != row.children.end(); ++it) {
      uint32_t state = static_cast<uint32_t>(anchor + it->first - first);
      nodes[it->second].state = state;
      occupied[state] = true;
      high_water = std::max(high_water, static_cast<size_t>(state) + 1);
    }
  }
  const uint64_t no_check = (UINT64_C(1) << 21) - 1;
  const uint64_t no_weight = (UINT64_C(1) << 13) - 1;
  const uint64_t base_mask = (UINT64_C(1) << 22) - 1;
  std::vector<uint64_t> result(high_water, (no_check << 22) | (no_weight << 43));
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Node& node = nodes[i];
    result[node.state] = (static_cast<uint64_t>(node.base) & base_mask) |
                        ((node.terminal ? UINT64_C(0) : no_weight) << 43);
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (std::map<Rune, uint32_t>::const_iterator it = nodes[i].children.begin();
         it != nodes[i].children.end(); ++it) {
      result[nodes[it->second].state] |= static_cast<uint64_t>(nodes[i].state) << 22;
    }
  }
  return result;
}

}  // namespace

TEST(BitmapDoubleArrayTrieTest, EmptyRootAndInvalidStates) {
  BitmapDoubleArrayTrie trie;
  EXPECT_EQ(1u, trie.SlotCount());
  EXPECT_EQ(1u, trie.NodeCount());
  EXPECT_EQ(0u, trie.MaxWordLength());
  EXPECT_FALSE(trie.IsTerminal(0));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(0, 0));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(BitmapDoubleArrayTrie::NoState(), 1));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.ValueId(0));
  EXPECT_EQ(0.0, trie.Weight(BitmapDoubleArrayTrie::NoState()));
  trie.Build(std::vector<Unicode>(1), std::vector<double>(1, -1));
  EXPECT_FALSE(trie.IsTerminal(0));
  EXPECT_TRUE(trie.Weights().empty());
}

TEST(BitmapDoubleArrayTrieTest, PrefixTerminalDuplicatesAndNegativeBase) {
  std::vector<Unicode> keys;
  keys.push_back(Runes({0x4e2d, 0x56fd}));
  keys.push_back(Runes({0x4e2d}));
  keys.push_back(Runes({0x4e2d, 0x56fd}));
  keys.push_back(Runes({0x4e2d, 0x56fd, 0x4eba}));
  std::vector<double> weights = {-2, -3, -4, -5};
  BitmapDoubleArrayTrie trie;
  trie.Build(keys, weights);
  EXPECT_EQ(4u, trie.NodeCount());
  EXPECT_EQ(3u, trie.MaxWordLength());
  EXPECT_TRUE((trie.Units()[0] & (UINT64_C(1) << 21)) != 0);
  uint32_t prefix = Lookup(trie, keys[1]);
  EXPECT_TRUE(trie.IsTerminal(prefix));
  EXPECT_EQ(-3, trie.Weight(prefix));
  uint32_t duplicate = Lookup(trie, keys[0]);
  EXPECT_EQ(2u, trie.ValueId(duplicate));
  EXPECT_EQ(-4, trie.Weight(duplicate));
  EXPECT_EQ(3u, trie.Weights().size());
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(prefix, 0x4e2d));
  // A negative base must not wrap, and root metadata must never be a child.
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(0, 0));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(0, 0x4e2d - 1));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(duplicate, UINT32_MAX));
}

TEST(BitmapDoubleArrayTrieTest, ShiftedWordBoundariesMatchScalarLayout) {
  std::vector<Unicode> keys;
  const Rune offsets[] = {0, 1, 2, 3, 62, 63, 64, 65, 126, 127, 128, 129};
  for (Rune row = 0; row < 14; ++row) {
    for (size_t i = row % 4; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
      keys.push_back(Runes({1000 + row, 3000 + offsets[i]}));
    }
    keys.push_back(Runes({1000 + row}));
  }
  std::reverse(keys.begin(), keys.end());
  BitmapDoubleArrayTrie trie;
  trie.Build(keys, std::vector<double>(keys.size(), -2));
  EXPECT_EQ(ReferenceUnits(keys), trie.Units());
  for (size_t i = 0; i < keys.size(); ++i) {
    uint32_t state = Lookup(trie, keys[i]);
    ASSERT_TRUE(trie.IsTerminal(state));
    EXPECT_EQ(i, trie.ValueId(state));
  }
  EXPECT_EQ(trie.SlotCount() * sizeof(uint64_t) + sizeof(double), trie.ModelBytes());
  EXPECT_EQ(trie.SlotCount() * sizeof(uint32_t), trie.AuxiliaryBytes());
}

TEST(BitmapDoubleArrayTrieTest, UnicodeExtremesAndCapacityGrowth) {
  std::vector<Unicode> keys;
  keys.push_back(Runes({0}));
  keys.push_back(Runes({0x10ffff}));
  keys.push_back(Runes({0x10000, 0}));
  keys.push_back(Runes({0x10000, 0x10ffff}));
  keys.push_back(Runes({0x10001, 0xd7ff}));
  keys.push_back(Runes({0x10001, 0xe000}));
  BitmapDoubleArrayTrie trie;
  trie.Build(keys, std::vector<double>(keys.size(), -1));
  EXPECT_EQ(ReferenceUnits(keys), trie.Units());
  for (size_t i = 0; i < keys.size(); ++i) {
    EXPECT_TRUE(trie.IsTerminal(Lookup(trie, keys[i])));
  }
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(0, 0x110000));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(0, 0xd800));
  EXPECT_EQ(BitmapDoubleArrayTrie::NoState(), trie.Transition(0, 1));
}

TEST(BitmapDoubleArrayTrieTest, WeightBitsPreserveSignedZeroAndSortUnsigned) {
  std::vector<Unicode> keys = {Runes({1}), Runes({2}), Runes({3}), Runes({4})};
  std::vector<double> weights = {-0.0, +0.0, -2.0, +1.0};
  BitmapDoubleArrayTrie trie;
  trie.Build(keys, weights);
  ASSERT_EQ(4u, trie.Weights().size());
  EXPECT_EQ(UINT64_C(0), Bits(trie.Weights()[0]));
  for (size_t i = 1; i < trie.Weights().size(); ++i) {
    EXPECT_LT(Bits(trie.Weights()[i - 1]), Bits(trie.Weights()[i]));
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    EXPECT_EQ(Bits(weights[i]), Bits(trie.Weight(Lookup(trie, keys[i]))));
  }
}

TEST(BitmapDoubleArrayTrieTest, FailedRebuildPreservesPreviousTree) {
  BitmapDoubleArrayTrie trie;
  std::vector<Unicode> keys(1, Runes({0x4e2d}));
  trie.Build(keys, std::vector<double>(1, -3));
  const std::vector<uint64_t> before = trie.Units();
  EXPECT_THROW(trie.Build(keys, std::vector<double>()), std::invalid_argument);
  EXPECT_THROW(trie.Build(keys, std::vector<double>(1, std::numeric_limits<double>::infinity())), std::invalid_argument);
  EXPECT_THROW(trie.Build(keys, std::vector<double>(1, std::numeric_limits<double>::quiet_NaN())), std::invalid_argument);
  EXPECT_THROW(trie.Build(std::vector<Unicode>(1, Runes({0xd800})), std::vector<double>(1, -1)), std::invalid_argument);
  EXPECT_THROW(trie.Build(std::vector<Unicode>(1, Runes({0x110000})), std::vector<double>(1, -1)), std::invalid_argument);
  EXPECT_EQ(before, trie.Units());
  EXPECT_EQ(-3, trie.Weight(Lookup(trie, keys[0])));
  EXPECT_EQ(0u, trie.ValueId(Lookup(trie, keys[0])));
}

TEST(BitmapDoubleArrayTrieTest, WeightCodeLimitPreservesPreviousTree) {
  BitmapDoubleArrayTrie trie;
  trie.Build(std::vector<Unicode>(1, Runes({1})), std::vector<double>(1, -1));
  const std::vector<uint64_t> before = trie.Units();
  std::vector<Unicode> keys;
  std::vector<double> weights;
  for (Rune i = 1; i <= 8192; ++i) {
    keys.push_back(Runes({i}));
    weights.push_back(-static_cast<double>(i));
  }
  EXPECT_THROW(trie.Build(keys, weights), std::length_error);
  EXPECT_EQ(before, trie.Units());
  keys.pop_back();
  weights.pop_back();
  EXPECT_NO_THROW(trie.Build(keys, weights));
  EXPECT_EQ(8191u, trie.Weights().size());
}
