#ifndef CPPJIEBA_BITMAP_TRIE_HPP
#define CPPJIEBA_BITMAP_TRIE_HPP

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "BitmapDoubleArrayTrie.hpp"
#include "Trie.hpp"

namespace cppjieba {

// The packed dictionary owns weights. The original DictUnit objects remain owned
// by DictTrie so existing pointer and POS APIs keep their lifetime guarantees.
class BitmapTrie {
 public:
  BitmapTrie(const std::vector<Unicode>& keys,
             const std::vector<const DictUnit*>& values)
      : values_(values), max_word_length_(0) {
    std::vector<double> weights;
    weights.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      weights.push_back(values[i]->weight);
      max_word_length_ = std::max(max_word_length_, keys[i].size());
    }
    try {
      dat_.Build(keys, weights);
    } catch (const std::length_error&) {
      // Custom dictionaries can exceed the fixed archive field widths.
      // Retain their behavior instead of truncating state/weight identifiers.
      fallback_.reset(new Trie(keys, values));
    }
  }

  void InsertNode(const Unicode& key, const DictUnit* value) {
    if (key.empty()) return;
    OverlayNode* node = &overlay_;
    for (Unicode::const_iterator it = key.begin(); it != key.end(); ++it) {
      std::unique_ptr<OverlayNode>& child = node->children[*it];
      if (!child) child.reset(new OverlayNode);
      node = child.get();
    }
    node->overridden = true;
    node->value = value;
    max_word_length_ = std::max(max_word_length_, key.size());
  }

  void DeleteNode(const Unicode& key, const DictUnit*) {
    // A tombstone masks this terminal, leaving longer words and siblings intact.
    InsertNode(key, NULL);
  }

  const DictUnit* Find(RuneStrArray::const_iterator begin,
                       RuneStrArray::const_iterator end) const {
    if (begin == end) return NULL;
    const OverlayNode* node = overlay_.children.empty() ? NULL : &overlay_;
    uint32_t state = 0;
    for (RuneStrArray::const_iterator it = begin; it != end; ++it) {
      node = NextOverlay(node, it->rune);
      if (!fallback_) state = dat_.Transition(state, it->rune);
      if (!node && !fallback_ && state == BitmapDoubleArrayTrie::NoState()) return NULL;
    }
    if (node && node->overridden) return node->value;
    if (fallback_) return fallback_->Find(begin, end);
    return Value(state);
  }

  void Find(RuneStrArray::const_iterator begin,
            RuneStrArray::const_iterator end, std::vector<Dag>& result,
            size_t max_word_len = MAX_WORD_LENGTH) const {
    result.clear();
    if (fallback_) {
      fallback_->Find(begin, end, result, max_word_len);
      MergeOverlay(begin, end, result, max_word_len);
      return;
    }
    result.resize(end - begin);
    for (size_t i = 0; i < result.size(); ++i) {
      result[i].runestr = begin[i];
      Enumerate(begin + i, end, max_word_len, [&](size_t length, const Candidate& candidate) {
        result[i].nexts.push_back(std::make_pair(i + length - 1,
            candidate.overridden ? candidate.value : Value(candidate.state)));
      });
    }
  }

  // Shortest-first enumeration and strict greater-than preserve the old DP's
  // tie breaking. Only scores for the next maximum-word-length positions live
  // in the ring; output reconstruction retains one length per input Rune.
  void FindBestPath(RuneStrArray::const_iterator begin,
                    RuneStrArray::const_iterator end, double unknown_weight,
                    size_t max_word_len, std::vector<size_t>& lengths) const {
    const size_t count = end - begin;
    lengths.assign(count, 1);
    if (!count) return;
    const size_t limit = std::max<size_t>(1, std::min(max_word_len, max_word_length_));
    const size_t ring_size = std::min(count, limit) + 1;
    std::vector<double> scores(ring_size, 0.0);
    size_t ring_pos = count % ring_size;
    std::vector<Dag> fallback_dags;
    if (fallback_) Find(begin, end, fallback_dags, max_word_len);
    for (size_t i = count; i-- > 0;) {
      ring_pos = ring_pos ? ring_pos - 1 : ring_size - 1;
      double best = MIN_DOUBLE;
      const auto consider = [&](size_t length, double weight) {
        size_t next = ring_pos + length;
        if (next >= ring_size) next -= ring_size;
        const double score = scores[next] + weight;
        if (score > best) {
          best = score;
          lengths[i] = length;
        }
      };
      if (fallback_) {
        for (size_t j = 0; j < fallback_dags[i].nexts.size(); ++j) {
          const std::pair<size_t, const DictUnit*>& edge = fallback_dags[i].nexts[j];
          consider(edge.first - i + 1, edge.second ? edge.second->weight : unknown_weight);
        }
      } else {
        Enumerate(begin + i, end, limit, [&](size_t length, const Candidate& candidate) {
          consider(length, candidate.terminal ? candidate.weight : unknown_weight);
        });
      }
      scores[ring_pos] = best;
    }
  }

  const BitmapDoubleArrayTrie& GetDAT() const { return dat_; }
  bool UsesPackedDAT() const { return !fallback_; }
  size_t MetadataBytes() const {
    return dat_.AuxiliaryBytes() + values_.capacity() * sizeof(const DictUnit*);
  }

 private:
  struct OverlayNode {
    std::unordered_map<Rune, std::unique_ptr<OverlayNode> > children;
    const DictUnit* value;
    bool overridden;
    OverlayNode() : value(NULL), overridden(false) {}
  };
  struct Candidate {
    uint32_t state;
    const DictUnit* value;
    double weight;
    bool overridden;
    bool terminal;
  };

  static const OverlayNode* NextOverlay(const OverlayNode* node, Rune rune) {
    if (!node) return NULL;
    const auto it = node->children.find(rune);
    return it == node->children.end() ? NULL : it->second.get();
  }

  const DictUnit* Value(uint32_t state) const {
    return dat_.IsTerminal(state) ? values_[dat_.ValueId(state)] : NULL;
  }

  template <class Visitor>
  void Enumerate(RuneStrArray::const_iterator begin,
                 RuneStrArray::const_iterator end, size_t max_word_len,
                 Visitor visit) const {
    uint32_t state = 0;
    const OverlayNode* node = overlay_.children.empty() ? NULL : &overlay_;
    const size_t limit = std::min<size_t>(end - begin, std::max<size_t>(1, max_word_len));
    for (size_t i = 0; i < limit; ++i) {
      state = dat_.Transition(state, begin[i].rune);
      node = NextOverlay(node, begin[i].rune);
      Candidate candidate = {state, NULL, 0.0, false, false};
      if (node && node->overridden) {
        candidate.overridden = true;
        candidate.value = node->value;
        candidate.terminal = node->value != NULL;
        if (candidate.terminal) candidate.weight = node->value->weight;
      } else {
        candidate.terminal = dat_.IsTerminal(state);
        if (candidate.terminal) candidate.weight = dat_.Weight(state);
      }
      if (i == 0 || candidate.terminal) visit(i + 1, candidate);
      if (state == BitmapDoubleArrayTrie::NoState() && !node) break;
    }
  }

  void MergeOverlay(RuneStrArray::const_iterator begin,
                    RuneStrArray::const_iterator end, std::vector<Dag>& dags,
                    size_t max_word_len) const {
    if (overlay_.children.empty()) return;
    for (size_t i = 0; i < dags.size(); ++i) {
      // Copy only on the uncommon wide-dictionary fallback path.
      std::vector<std::pair<size_t, const DictUnit*> > edges(
          dags[i].nexts.begin(), dags[i].nexts.end());
      const OverlayNode* node = &overlay_;
      const size_t limit = std::min<size_t>(end - (begin + i), std::max<size_t>(1, max_word_len));
      for (size_t length = 1; length <= limit; ++length) {
        node = NextOverlay(node, begin[i + length - 1].rune);
        if (!node) break;
        if (!node->overridden) continue;
        const size_t offset = i + length - 1;
        auto edge = std::lower_bound(edges.begin(), edges.end(), offset,
            [](const std::pair<size_t, const DictUnit*>& item, size_t pos) { return item.first < pos; });
        if (edge != edges.end() && edge->first == offset) {
          if (node->value || length == 1) edge->second = node->value;
          else edges.erase(edge);
        } else if (node->value) {
          edges.insert(edge, std::make_pair(offset, node->value));
        }
      }
      dags[i].nexts.clear();
      for (size_t j = 0; j < edges.size(); ++j) dags[i].nexts.push_back(edges[j]);
    }
  }

  BitmapDoubleArrayTrie dat_;
  std::vector<const DictUnit*> values_;
  std::unique_ptr<Trie> fallback_;
  OverlayNode overlay_;
  size_t max_word_length_;
};

}  // namespace cppjieba
#endif
