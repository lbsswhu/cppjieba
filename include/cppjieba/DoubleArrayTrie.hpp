#ifndef CPPJIEBA_DOUBLE_ARRAY_TRIE_HPP
#define CPPJIEBA_DOUBLE_ARRAY_TRIE_HPP

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Unicode.hpp"

namespace cppjieba {

inline bool RuneLexicographicLess(const Unicode& lhs, const Unicode& rhs) {
  return std::lexicographical_compare(lhs.begin(), lhs.end(),
                                      rhs.begin(), rhs.end());
}

class DoubleArrayTrie {
 public:
  struct BuildEntry {
    Unicode word;
    double weight;
    uint16_t tag_id;
  };

  struct Match {
    size_t length;
    double weight;
    uint16_t tag_id;
  };

  struct BuildOptions {
    BuildOptions() : max_address(INT32_MAX) {}
    explicit BuildOptions(int64_t limit) : max_address(limit) {}

    int64_t max_address;
  };

  struct Stats {
    Stats()
        : slot_count(0),
          occupied_state_count(0),
          terminal_count(0),
          load_factor(0.0),
          array_bytes(0) {}

    size_t slot_count;
    size_t occupied_state_count;
    size_t terminal_count;
    double load_factor;
    size_t array_bytes;
  };

  DoubleArrayTrie()
      : max_address_(INT32_MAX), next_check_pos_(1) {
    ResetToRoot();
  }

  bool Build(const std::vector<BuildEntry>& entries,
             std::string* error,
             const BuildOptions& options = BuildOptions()) {
    if (error != NULL) {
      error->clear();
    }

    DoubleArrayTrie candidate;
    if (!candidate.Validate(entries, options, error)) {
      return false;
    }
    candidate.max_address_ = options.max_address;

    if (!entries.empty() &&
        !candidate.BuildState(entries, 0, entries.size(), 0, 0, error)) {
      return false;
    }

    candidate.TrimTrailingSlots();
    candidate.RecomputeStats();
    Swap(candidate);
    return true;
  }

  bool ExactMatch(RuneStrArray::const_iterator begin,
                  RuneStrArray::const_iterator end,
                  double* weight,
                  uint16_t* tag_id) const {
    size_t state = 0;
    for (RuneStrArray::const_iterator iter = begin; iter != end; ++iter) {
      size_t child = 0;
      if (!Transition(state, iter->rune, &child)) {
        return false;
      }
      state = child;
    }

    if (terminal_[state] == 0) {
      return false;
    }
    if (weight != NULL) {
      *weight = weight_[state];
    }
    if (tag_id != NULL) {
      *tag_id = tag_id_[state];
    }
    return true;
  }

  void CommonPrefixSearch(RuneStrArray::const_iterator begin,
                          RuneStrArray::const_iterator end,
                          size_t max_word_len,
                          std::vector<Match>* matches) const {
    if (matches == NULL) {
      return;
    }
    matches->clear();

    size_t state = 0;
    size_t length = 0;
    for (RuneStrArray::const_iterator iter = begin;
         iter != end && length < max_word_len; ++iter) {
      size_t child = 0;
      if (!Transition(state, iter->rune, &child)) {
        return;
      }
      state = child;
      ++length;
      if (terminal_[state] != 0) {
        Match match;
        match.length = length;
        match.weight = weight_[state];
        match.tag_id = tag_id_[state];
        matches->push_back(match);
      }
    }
  }

  const Stats& stats() const {
    return stats_;
  }

  int32_t base_at(size_t state) const {
    return state < base_.size() ? base_[state] : 0;
  }

  bool last_slot_occupied() const {
    return !check_.empty() && check_.back() != -1;
  }

 private:
  struct Sibling {
    Rune label;
    size_t begin;
    size_t end;
  };

  void ResetToRoot() {
    base_.assign(1, 0);
    check_.assign(1, 0);
    terminal_.assign(1, 0);
    weight_.assign(1, 0.0);
    tag_id_.assign(1, 0);
    next_check_pos_ = 1;
    RecomputeStats();
  }

  bool Validate(const std::vector<BuildEntry>& entries,
                const BuildOptions& options,
                std::string* error) const {
    if (options.max_address < 1 || options.max_address > INT32_MAX) {
      SetError(error, "max_address must be in [1, INT32_MAX]");
      return false;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].word.empty()) {
        SetError(error, "build word must be nonempty");
        return false;
      }
      if (!std::isfinite(entries[i].weight)) {
        SetError(error, "build payload must have finite weight");
        return false;
      }
      if (i != 0 &&
          !RuneLexicographicLess(entries[i - 1].word, entries[i].word)) {
        SetError(error, "build keys must be in strict Rune order");
        return false;
      }
    }
    return true;
  }

  bool BuildState(const std::vector<BuildEntry>& entries,
                  size_t begin,
                  size_t end,
                  size_t depth,
                  size_t state,
                  std::string* error) {
    if (begin < end && entries[begin].word.size() == depth) {
      terminal_[state] = 1;
      weight_[state] = entries[begin].weight;
      tag_id_[state] = entries[begin].tag_id;
      ++begin;
    }
    if (begin == end) {
      return true;
    }

    std::vector<Sibling> siblings;
    size_t group_begin = begin;
    while (group_begin < end) {
      if (entries[group_begin].word.size() <= depth) {
        SetError(error, "invalid ordered key interval while building trie");
        return false;
      }
      const Rune label = entries[group_begin].word[depth];
      size_t group_end = group_begin + 1;
      while (group_end < end &&
             entries[group_end].word.size() > depth &&
             entries[group_end].word[depth] == label) {
        ++group_end;
      }

      Sibling sibling;
      sibling.label = label;
      sibling.begin = group_begin;
      sibling.end = group_end;
      siblings.push_back(sibling);
      group_begin = group_end;
    }

    int32_t state_base = 0;
    std::vector<size_t> child_states;
    if (!FindBase(siblings, &state_base, &child_states, error)) {
      return false;
    }
    base_[state] = state_base;

    for (size_t i = 0; i < child_states.size(); ++i) {
      check_[child_states[i]] = static_cast<int32_t>(state);
    }
    AdvanceNextCheckPosition();

    for (size_t i = 0; i < siblings.size(); ++i) {
      if (!BuildState(entries, siblings[i].begin, siblings[i].end,
                      depth + 1, child_states[i], error)) {
        return false;
      }
    }
    return true;
  }

  bool FindBase(const std::vector<Sibling>& siblings,
                int32_t* state_base,
                std::vector<size_t>* child_states,
                std::string* error) {
    const int64_t first_label = static_cast<int64_t>(siblings.front().label);
    const int64_t last_label = static_cast<int64_t>(siblings.back().label);
    const int64_t span = last_label - first_label;
    int64_t first_slot = std::max<int64_t>(1, next_check_pos_);

    const int64_t first_slot_for_int32_base =
        first_label + static_cast<int64_t>(INT32_MIN);
    if (first_slot < first_slot_for_int32_base) {
      first_slot = first_slot_for_int32_base;
    }

    const int64_t last_first_slot = max_address_ - span;
    for (; first_slot <= last_first_slot; ++first_slot) {
      if (first_slot < static_cast<int64_t>(check_.size()) &&
          check_[static_cast<size_t>(first_slot)] != -1) {
        continue;
      }

      const int64_t candidate_base = first_slot - first_label;
      if (candidate_base < INT32_MIN || candidate_base > INT32_MAX) {
        continue;
      }

      bool available = true;
      int64_t largest_address = 0;
      child_states->clear();
      child_states->reserve(siblings.size());
      for (size_t i = 0; i < siblings.size(); ++i) {
        const int64_t address =
            candidate_base + static_cast<int64_t>(siblings[i].label);
        if (address <= 0 || address > max_address_ ||
            address > INT32_MAX) {
          available = false;
          break;
        }
        if (address < static_cast<int64_t>(check_.size()) &&
            check_[static_cast<size_t>(address)] != -1) {
          available = false;
          break;
        }
        child_states->push_back(static_cast<size_t>(address));
        largest_address = address;
      }

      if (!available) {
        continue;
      }

      EnsureSize(static_cast<size_t>(largest_address) + 1);
      *state_base = static_cast<int32_t>(candidate_base);
      return true;
    }

    child_states->clear();
    SetError(error, "double-array address limit exhausted");
    return false;
  }

  void EnsureSize(size_t size) {
    if (size <= base_.size()) {
      return;
    }
    base_.resize(size, 0);
    check_.resize(size, -1);
    terminal_.resize(size, 0);
    weight_.resize(size, 0.0);
    tag_id_.resize(size, 0);
  }

  void AdvanceNextCheckPosition() {
    if (next_check_pos_ < 1) {
      next_check_pos_ = 1;
    }
    while (next_check_pos_ < static_cast<int64_t>(check_.size()) &&
           check_[static_cast<size_t>(next_check_pos_)] != -1) {
      ++next_check_pos_;
    }
  }

  bool Transition(size_t state, Rune rune, size_t* child) const {
    if (state >= base_.size()) {
      return false;
    }
    const int64_t address = static_cast<int64_t>(base_[state]) +
                            static_cast<int64_t>(rune);
    if (address <= 0 || address > max_address_ ||
        address > INT32_MAX ||
        address >= static_cast<int64_t>(check_.size())) {
      return false;
    }

    const size_t target = static_cast<size_t>(address);
    if (check_[target] != static_cast<int32_t>(state)) {
      return false;
    }
    *child = target;
    return true;
  }

  void TrimTrailingSlots() {
    size_t size = check_.size();
    while (size > 1 && check_[size - 1] == -1) {
      --size;
    }
    base_.resize(size);
    check_.resize(size);
    terminal_.resize(size);
    weight_.resize(size);
    tag_id_.resize(size);
    assert(base_.size() == check_.size());
    assert(base_.size() == terminal_.size());
    assert(base_.size() == weight_.size());
    assert(base_.size() == tag_id_.size());
    assert(check_.size() == 1 || check_.back() != -1);
  }

  void RecomputeStats() {
    stats_ = Stats();
    stats_.slot_count = check_.size();
    for (size_t i = 0; i < check_.size(); ++i) {
      if (check_[i] != -1) {
        ++stats_.occupied_state_count;
      }
      if (terminal_[i] != 0) {
        ++stats_.terminal_count;
      }
    }
    stats_.load_factor =
        stats_.slot_count == 0
            ? 0.0
            : static_cast<double>(stats_.occupied_state_count) /
                  static_cast<double>(stats_.slot_count);
    stats_.array_bytes = stats_.slot_count *
        (2 * sizeof(int32_t) + sizeof(uint8_t) + sizeof(double) +
         sizeof(uint16_t));
  }

  void Swap(DoubleArrayTrie& other) {
    base_.swap(other.base_);
    check_.swap(other.check_);
    terminal_.swap(other.terminal_);
    weight_.swap(other.weight_);
    tag_id_.swap(other.tag_id_);
    std::swap(stats_, other.stats_);
    std::swap(max_address_, other.max_address_);
    std::swap(next_check_pos_, other.next_check_pos_);
  }

  static void SetError(std::string* error, const char* message) {
    if (error != NULL) {
      *error = message;
    }
  }

  std::vector<int32_t> base_;
  std::vector<int32_t> check_;
  std::vector<uint8_t> terminal_;
  std::vector<double> weight_;
  std::vector<uint16_t> tag_id_;
  Stats stats_;
  int64_t max_address_;
  int64_t next_check_pos_;
};

}  // namespace cppjieba

#endif  // CPPJIEBA_DOUBLE_ARRAY_TRIE_HPP
