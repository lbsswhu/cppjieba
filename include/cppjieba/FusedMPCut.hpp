#ifndef CPPJIEBA_FUSED_MP_CUT_HPP
#define CPPJIEBA_FUSED_MP_CUT_HPP

#ifdef __FAST_MATH__
#error "cppjieba CPU kernels require strict floating point; disable fast-math"
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include "Trie.hpp"
#include "DatModel.hpp"
#include "MPCutScratch.hpp"

namespace cppjieba {

class PointerWalker {
 public:
  PointerWalker(const Trie& trie, RuneStrArray::const_iterator begin)
      : root_(trie.root_), node_(trie.root_), begin_(begin) {}
  void Reset() { node_ = root_; }
  bool Step(size_t offset) {
    if (!node_->next) return false;
    TrieNode::NextMap::const_iterator it = node_->next->find(begin_[offset].rune);
    if (it == node_->next->end()) return false;
    node_ = it->second;
    return true;
  }
  bool IsTerminal() const { return node_->ptValue != NULL; }
  double TerminalWeight() const { return node_->ptValue->weight; }
 private:
  const TrieNode* root_;
  const TrieNode* node_;
  RuneStrArray::const_iterator begin_;
};

class RawDatWalker {
 public:
  RawDatWalker(const DatModel& model, RuneStrArray::const_iterator begin)
      : model_(model), cursor_(model.Root()), begin_(begin) {}
  void Reset() { cursor_ = model_.Root(); }
  bool Step(size_t offset) { return model_.StepRaw(begin_[offset].rune, cursor_); }
  bool IsTerminal() const { return model_.IsTerminal(cursor_); }
  double TerminalWeight() const { return model_.TerminalWeight(cursor_); }
 private:
  const DatModel& model_;
  DatCursor cursor_;
  RuneStrArray::const_iterator begin_;
};

// Each call owns scratch; no model writes, virtual calls or candidate containers.
template<class Walker>
void CutRangeFused(RuneStrArray::const_iterator begin, size_t n, size_t limit,
                   double unknownWeight, Walker& walker, MPCutScratch& scratch,
                   std::vector<WordRange>& out) {
  if (!n) return;
  assert(limit >= 1 && limit <= n && limit <= UINT16_MAX);
  if (n > out.max_size() - out.size()) throw std::length_error("too many words");
  const size_t ringSize = limit + 1;
  scratch.dpRing.resize(ringSize);
  scratch.bestLen.resize(n);
  out.reserve(out.size() + n);
  scratch.dpRing[n % ringSize] = 0.0;
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
  ++scratch.fused_ranges;
#endif
  for (size_t i = n; i-- > 0;) {
    double best = MIN_DOUBLE;
    uint16_t chosen = 1;
    size_t futureSlot = (i + 1) % ringSize;
    const size_t stop = std::min(limit, n - i);
    walker.Reset();
    for (size_t len = 1; len <= stop; ++len) {
      const bool alive = walker.Step(i + len - 1);
      const bool terminal = alive && walker.IsTerminal();
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
      ++scratch.transitions;
#endif
      if (len == 1 || terminal) {
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
        ++scratch.candidates;
#endif
        double score = 0.0;
        if (i + len < n) score += scratch.dpRing[futureSlot];
        score += terminal ? walker.TerminalWeight() : unknownWeight;
        if (score > best) {
          best = score;
          chosen = static_cast<uint16_t>(len);
        }
      }
      if (!alive) break;
      if (++futureSlot == ringSize) futureSlot = 0;
    }
    scratch.dpRing[i % ringSize] = best;
    scratch.bestLen[i] = chosen;
  }
  for (size_t i = 0; i < n;) {
    const size_t len = scratch.bestLen[i];
    assert(len >= 1 && len <= n - i);
    out.push_back(WordRange(begin + i, begin + i + len - 1));
    i += len;
  }
}
} // namespace cppjieba
#endif
