#ifndef CPPJIEBA_BITMAP_DOUBLE_ARRAY_TRIE_H
#define CPPJIEBA_BITMAP_DOUBLE_ARRAY_TRIE_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "Unicode.hpp"

namespace cppjieba {

// Immutable raw-Rune DAT. Construction uses the lowest available bitmap anchor
// for each row, ordered by degree, label span, then logical trie node number.
// Runtime units contain signed base:22, check:21, weight code:13, reserved:8.
class BitmapDoubleArrayTrie {
 public:
  BitmapDoubleArrayTrie()
      : units_(1, Pack(0, 0, NoWeight())), value_ids_(1, NoState()),
        node_count_(1), max_word_length_(0) {}

  static uint32_t NoState() { return std::numeric_limits<uint32_t>::max(); }

  // Empty keys are ignored. Duplicate keys use the last input weight/value ID.
  // Building in a temporary provides a strong exception guarantee for callers
  // that fall back to the legacy trie when a packed field is too small.
  void Build(const std::vector<Unicode>& keys, const std::vector<double>& weights) {
    BitmapDoubleArrayTrie next;
    next.BuildImpl(keys, weights);
    units_.swap(next.units_);
    weights_.swap(next.weights_);
    value_ids_.swap(next.value_ids_);
    std::swap(node_count_, next.node_count_);
    std::swap(max_word_length_, next.max_word_length_);
  }

  uint32_t Transition(uint32_t state, Rune rune) const {
    if (state >= units_.size() || !ValidRune(rune)) return NoState();
    const uint64_t unit = units_[state];
    if (Check(unit) == NoCheck()) return NoState();
    const int64_t target = static_cast<int64_t>(Base(unit)) + rune;
    // check[0] == 0 is root metadata, never a root-to-root transition.
    if (target <= 0 || static_cast<uint64_t>(target) >= units_.size()) return NoState();
    return Check(units_[static_cast<size_t>(target)]) == state
               ? static_cast<uint32_t>(target) : NoState();
  }

  bool IsTerminal(uint32_t state) const {
    return state < units_.size() && WeightCode(units_[state]) != NoWeight();
  }

  double Weight(uint32_t state) const {
    return IsTerminal(state) ? weights_[WeightCode(units_[state])] : 0.0;
  }

  uint32_t ValueId(uint32_t state) const {
    return state < value_ids_.size() ? value_ids_[state] : NoState();
  }

  size_t SlotCount() const { return units_.size(); }
  size_t NodeCount() const { return node_count_; }
  size_t MaxWordLength() const { return max_word_length_; }
  // Serialized model payload, excluding the caller's unknown-word weight.
  size_t ModelBytes() const { return units_.size() * sizeof(uint64_t) + weights_.size() * sizeof(double); }
  // Compatibility metadata, kept separate from the packed segmentation model.
  size_t AuxiliaryBytes() const { return value_ids_.size() * sizeof(uint32_t); }
  const std::vector<uint64_t>& Units() const { return units_; }
  const std::vector<double>& Weights() const { return weights_; }

 private:
  static uint32_t NoCheck() { return (uint32_t(1) << 21) - 1; }
  static uint32_t NoWeight() { return (uint32_t(1) << 13) - 1; }
  static uint64_t BaseMask() { return (UINT64_C(1) << 22) - 1; }
  static bool ValidRune(Rune rune) {
    return rune <= 0x10ffff && !(rune >= 0xd800 && rune <= 0xdfff);
  }
  static int32_t Base(uint64_t unit) {
    const int32_t bits = static_cast<int32_t>(unit & BaseMask());
    return (bits & (int32_t(1) << 21)) ? bits - (int32_t(1) << 22) : bits;
  }
  static uint32_t Check(uint64_t unit) { return static_cast<uint32_t>((unit >> 22) & NoCheck()); }
  static uint32_t WeightCode(uint64_t unit) { return static_cast<uint32_t>((unit >> 43) & NoWeight()); }
  static uint64_t Pack(int32_t base, uint32_t check, uint32_t weight) {
    return (static_cast<uint64_t>(base) & BaseMask()) |
           (static_cast<uint64_t>(check) << 22) | (static_cast<uint64_t>(weight) << 43);
  }
  static uint64_t WeightBits(double weight) {
    static_assert(sizeof(double) == sizeof(uint64_t), "DAT weights require FP64 doubles");
    static_assert(std::numeric_limits<double>::is_iec559, "DAT weights require IEEE 754 doubles");
    uint64_t bits;
    std::memcpy(&bits, &weight, sizeof(bits));
    return bits;
  }

  struct Node {
    Rune rune;
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t next_sibling;
    uint32_t degree;
    uint32_t state;
    int32_t base;
    uint32_t weight;
    uint32_t value;
    Node(Rune r, uint32_t p)
        : rune(r), parent(p), first_child(NoState()), last_child(NoState()),
          next_sibling(NoState()), degree(0), state(NoState()), base(0),
          weight(NoWeight()), value(NoState()) {}
  };

  class FreeBitmap {
   public:
    FreeBitmap() : words_(1024, ~UINT64_C(0)), capacity_(65536), first_word_(0) {
      words_[0] &= ~UINT64_C(1);  // Reserve the root.
    }

    uint32_t FindAnchor(const std::vector<uint32_t>& offsets) {
      const uint32_t span = offsets.back();
      if (static_cast<uint64_t>(span) + 2 > capacity_) Expand(static_cast<size_t>(span) + 2);
      for (;;) {
        const size_t last_word = (capacity_ - span - 1) / 64;
        // Every feasible anchor is free itself. All earlier words are occupied.
        for (size_t word = first_word_; word <= last_word; ++word) {
          uint64_t candidates = words_[word] & ShiftedWord(word, span);
          for (size_t i = 1; candidates && i + 1 < offsets.size(); ++i) {
            candidates &= ShiftedWord(word, offsets[i]);
          }
          if (candidates) return static_cast<uint32_t>(word * 64 + LowestBit(candidates));
        }
        Expand(capacity_ + 1);
      }
    }

    void Occupy(uint32_t state) { words_[state / 64] &= ~(UINT64_C(1) << (state % 64)); }

    void AdvanceFirstWord() {
      while (first_word_ < words_.size() && words_[first_word_] == 0) ++first_word_;
    }

   private:
    static unsigned LowestBit(uint64_t bits) {
#if defined(__GNUC__) || defined(__clang__)
      return static_cast<unsigned>(__builtin_ctzll(bits));
#else
      unsigned result = 0;
      while (!(bits & 1)) { bits >>= 1; ++result; }
      return result;
#endif
    }

    // One 64-bit window of F >> offset; out-of-capacity bits are zero.
    uint64_t ShiftedWord(size_t word, uint32_t offset) const {
      const size_t source = word + offset / 64;
      if (source >= words_.size()) return 0;
      const unsigned shift = offset % 64;
      uint64_t result = words_[source] >> shift;
      if (shift && source + 1 < words_.size()) result |= words_[source + 1] << (64 - shift);
      return result;
    }

    void Expand(size_t required) {
      if (capacity_ >= NoCheck() || required > NoCheck()) {
        throw std::length_error("Bitmap DAT exceeds the 21-bit state limit");
      }
      size_t capacity = std::max(capacity_ + 65536, ((required + 65535) / 65536) * 65536);
      // The final allocation block is clipped before the reserved check value.
      capacity = std::min(capacity, static_cast<size_t>(NoCheck()));
      words_.resize((capacity + 63) / 64, ~UINT64_C(0));
      if (capacity % 64) words_.back() &= (UINT64_C(1) << (capacity % 64)) - 1;
      capacity_ = capacity;
    }

    std::vector<uint64_t> words_;
    size_t capacity_;
    size_t first_word_;
  };

  void BuildImpl(const std::vector<Unicode>& keys, const std::vector<double>& weights) {
    if (keys.size() != weights.size()) throw std::invalid_argument("Bitmap DAT key/weight counts differ");
    if (keys.size() > NoState()) throw std::length_error("Bitmap DAT input value IDs exceed uint32");
    std::vector<uint32_t> order;
    order.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
      if (!std::isfinite(weights[i])) throw std::invalid_argument("Bitmap DAT weight is not finite");
      if (keys[i].size() >= NoCheck()) throw std::length_error("Bitmap DAT word exceeds the state limit");
      for (size_t j = 0; j < keys[i].size(); ++j) {
        if (!ValidRune(keys[i][j])) throw std::invalid_argument("Bitmap DAT contains an invalid Unicode scalar");
      }
      if (!keys[i].empty()) order.push_back(static_cast<uint32_t>(i));
    }
    std::sort(order.begin(), order.end(), [&keys](uint32_t a, uint32_t b) {
      const Unicode& left = keys[a];
      const Unicode& right = keys[b];
      const size_t common = std::min(left.size(), right.size());
      for (size_t i = 0; i < common; ++i) {
        if (left[i] != right[i]) return left[i] < right[i];
      }
      return left.size() != right.size() ? left.size() < right.size() : a < b;
    });
    size_t effective = 0;
    for (size_t i = 0; i < order.size(); ++i) {
      if (i + 1 == order.size() || keys[order[i]] != keys[order[i + 1]]) {
        order[effective++] = order[i];
      }
    }
    order.resize(effective);

    std::vector<uint64_t> bits;
    bits.reserve(order.size());
    for (size_t i = 0; i < order.size(); ++i) bits.push_back(WeightBits(weights[order[i]]));
    std::sort(bits.begin(), bits.end());
    bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    if (bits.size() > NoWeight()) throw std::length_error("Bitmap DAT exceeds the 13-bit weight limit");
    weights_.resize(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) std::memcpy(&weights_[i], &bits[i], sizeof(double));

    std::vector<Node> nodes(1, Node(0, 0));
    std::vector<uint32_t> path(1, 0);
    const Unicode* previous = NULL;
    for (size_t i = 0; i < order.size(); ++i) {
      const uint32_t value = order[i];
      const Unicode& word = keys[value];
      size_t common = 0;
      if (previous) {
        const size_t length = std::min(previous->size(), word.size());
        while (common < length && (*previous)[common] == word[common]) ++common;
      }
      path.resize(common + 1);
      for (size_t j = common; j < word.size(); ++j) {
        if (nodes.size() >= NoCheck()) throw std::length_error("Bitmap DAT exceeds the 21-bit node limit");
        const uint32_t parent = path.back();
        const uint32_t child = static_cast<uint32_t>(nodes.size());
        nodes.push_back(Node(word[j], parent));
        Node& row = nodes[parent];
        if (row.degree) nodes[row.last_child].next_sibling = child;
        else row.first_child = child;
        row.last_child = child;
        ++row.degree;
        path.push_back(child);
      }
      Node& terminal = nodes[path.back()];
      terminal.weight = static_cast<uint32_t>(std::lower_bound(bits.begin(), bits.end(), WeightBits(weights[value])) - bits.begin());
      terminal.value = value;
      max_word_length_ = std::max(max_word_length_, word.size());
      previous = &word;
    }

    std::vector<uint32_t> rows;
    rows.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].degree) rows.push_back(static_cast<uint32_t>(i));
    }
    std::sort(rows.begin(), rows.end(), [&nodes](uint32_t a, uint32_t b) {
      const Node& left = nodes[a];
      const Node& right = nodes[b];
      if (left.degree != right.degree) return left.degree > right.degree;
      const Rune left_span = nodes[left.last_child].rune - nodes[left.first_child].rune;
      const Rune right_span = nodes[right.last_child].rune - nodes[right.first_child].rune;
      return left_span != right_span ? left_span > right_span : a < b;
    });

    FreeBitmap free;
    std::vector<uint32_t> offsets;
    size_t high_water = 1;
    nodes[0].state = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      Node& row = nodes[rows[i]];
      const Rune first = nodes[row.first_child].rune;
      offsets.clear();
      offsets.reserve(row.degree);
      for (uint32_t child = row.first_child; child != NoState(); child = nodes[child].next_sibling) {
        offsets.push_back(nodes[child].rune - first);
      }
      const uint32_t anchor = free.FindAnchor(offsets);
      const int64_t base = static_cast<int64_t>(anchor) - first;
      if (base < -(int64_t(1) << 21) || base >= (int64_t(1) << 21)) {
        throw std::length_error("Bitmap DAT exceeds the signed 22-bit base limit");
      }
      row.base = static_cast<int32_t>(base);
      for (uint32_t child = row.first_child; child != NoState(); child = nodes[child].next_sibling) {
        const uint32_t state = anchor + (nodes[child].rune - first);
        nodes[child].state = state;
        free.Occupy(state);
      }
      free.AdvanceFirstWord();
      high_water = std::max(high_water, static_cast<size_t>(anchor) + offsets.back() + 1);
    }

    units_.assign(high_water, Pack(0, NoCheck(), NoWeight()));
    value_ids_.assign(high_water, NoState());
    for (size_t i = 0; i < nodes.size(); ++i) {
      const Node& node = nodes[i];
      units_[node.state] = Pack(node.base, nodes[node.parent].state, node.weight);
      value_ids_[node.state] = node.value;
    }
    node_count_ = nodes.size();
  }

  std::vector<uint64_t> units_;
  std::vector<double> weights_;
  std::vector<uint32_t> value_ids_;
  size_t node_count_;
  size_t max_word_length_;
};

}  // namespace cppjieba

#endif  // CPPJIEBA_BITMAP_DOUBLE_ARRAY_TRIE_H
