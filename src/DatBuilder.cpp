#include "cppjieba/DatBuilder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace cppjieba {
namespace {

using Clock = std::chrono::steady_clock;

struct BuildFailure {
  DatBuildStatus status;
  const char* reason;
};

void Fail(DatBuildStatus status, const char* reason) {
  throw BuildFailure{status, reason};
}

size_t CheckedAdd(size_t lhs, size_t rhs) {
  if (rhs > std::numeric_limits<size_t>::max() - lhs)
    Fail(DatBuildStatus::ResourceLimit, "temporary byte count overflow");
  return lhs + rhs;
}

size_t CheckedMultiply(size_t lhs, size_t rhs) {
  if (rhs && lhs > std::numeric_limits<size_t>::max() / rhs)
    Fail(DatBuildStatus::ResourceLimit, "temporary byte count overflow");
  return lhs * rhs;
}

double Elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class Budget {
 public:
  Budget(const DatBuildOptions& options, Clock::time_point start)
      : options_(options), start_(start), fixed_bytes_(4096) {}

  void CheckTime() const {
    if (!(options_.max_build_ms > 0) || Elapsed(start_) >= options_.max_build_ms)
      Fail(DatBuildStatus::ResourceLimit, "build time budget exceeded");
  }

  void Add(size_t count, size_t bytes) {
    fixed_bytes_ = CheckedAdd(fixed_bytes_, CheckedMultiply(count, bytes));
    CheckBytes(fixed_bytes_);
  }

  void CheckSlots(size_t count) const {
    // Include the final units, source indices, validation marks, and both bitmap
    // allocations alive during growth. Other arrays have precomputed lengths.
    const size_t bitmap = CheckedMultiply(CheckedAdd(count, 63) / 64, 16);
    const size_t slot_bytes = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(unsigned char);
    CheckBytes(CheckedAdd(fixed_bytes_, CheckedAdd(CheckedMultiply(count, slot_bytes), bitmap)));
  }

 private:
  void CheckBytes(size_t bytes) const {
    if (bytes > options_.max_temporary_bytes)
      Fail(DatBuildStatus::ResourceLimit, "temporary memory budget exceeded");
  }
  const DatBuildOptions& options_;
  Clock::time_point start_;
  size_t fixed_bytes_;
};

uint64_t WeightBits(double weight) {
  uint64_t bits;
  std::memcpy(&bits, &weight, sizeof(bits));
  return bits;
}

uint64_t Pack(int32_t base, uint32_t check, uint32_t weight) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(base)) & dat_detail::BASE_MASK)
      | (static_cast<uint64_t>(check) << 22)
      | (static_cast<uint64_t>(weight) << 43);
}

unsigned TrailingZeros(uint64_t bits) {
  assert(bits);
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctzll(bits));
#else
  unsigned count = 0;
  while ((bits & 1) == 0) { bits >>= 1; ++count; }
  return count;
#endif
}

struct Record {
  const DictUnit* unit;
  size_t source_index;
};
struct LogicalNode {
  uint32_t first_edge;
  uint32_t edge_count;
  uint32_t weight;
  uint32_t source_index;
  LogicalNode() : first_edge(0), edge_count(0), weight(dat_detail::NO_WEIGHT),
                  source_index(dat_detail::NO_SOURCE_INDEX) {}
};
struct PendingEdge { uint32_t parent, rune, child; };
struct LogicalEdge { uint32_t rune, child; };

bool WordLess(const Unicode& lhs, const Unicode& rhs) {
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}
bool WordEqual(const Unicode& lhs, const Unicode& rhs) {
  return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}
size_t CommonPrefix(const Unicode* previous, const Unicode& word) {
  if (!previous) return 0;
  const size_t limit = std::min(previous->size(), word.size());
  size_t i = 0;
  while (i < limit && (*previous)[i] == word[i]) ++i;
  return i;
}

class FreeBitmap {
 public:
  FreeBitmap(size_t limit, const Budget& budget)
      : limit_(limit), capacity_(0), first_free_word_(0), budget_(budget) {}

  int32_t Place(const LogicalEdge* edges, size_t count,
                std::vector<uint32_t>& states) {
    const uint32_t first = edges[0].rune;
    const uint64_t span = static_cast<uint64_t>(edges[count - 1].rune) - first;
    if (span + 2 > dat_detail::NO_CHECK)
      Fail(DatBuildStatus::FieldLimit, "Rune span exceeds packed slot range");
    if (span + 2 > limit_)
      Fail(DatBuildStatus::ResourceLimit, "slot budget exceeded");
    if (capacity_ < span + 2) Grow(static_cast<size_t>(span + 2));
    for (;;) {
      budget_.CheckTime();
      // All anchors below this word are occupied. Skipping them preserves the
      // globally lowest feasible anchor while avoiding a quadratic zero scan.
      for (size_t word = first_free_word_; word < bits_.size(); ++word) {
        if ((word & 255U) == 0) budget_.CheckTime();
        const size_t q0 = word * 64;
        uint64_t mask = bits_[word];
        for (size_t j = count; mask && j > 1; --j) {
          if ((j & 255U) == 0) budget_.CheckTime();
          const size_t offset = static_cast<size_t>(edges[j - 1].rune - first);
          mask &= Read64(q0 + offset);
        }
        if (!mask) continue;
        const size_t q = q0 + TrailingZeros(mask);
        const int64_t base = static_cast<int64_t>(q) - static_cast<int64_t>(first);
        if (base < -(int64_t(1) << 21) || base >= (int64_t(1) << 21))
          Fail(DatBuildStatus::FieldLimit, "base exceeds signed 22-bit range");
        for (size_t j = 0; j < count; ++j) {
          const size_t target = q + static_cast<size_t>(edges[j].rune - first);
          if (target == 0 || target >= capacity_ || !IsFree(target))
            Fail(DatBuildStatus::ValidationFailed, "bitmap placement collision");
          bits_[target / 64] &= ~(uint64_t(1) << (target % 64));
          states[edges[j].child] = static_cast<uint32_t>(target);
        }
        while (first_free_word_ < bits_.size() && !bits_[first_free_word_])
          ++first_free_word_;
        return static_cast<int32_t>(base);
      }
      if (capacity_ == limit_) {
        Fail(limit_ == dat_detail::NO_CHECK ? DatBuildStatus::FieldLimit
                                         : DatBuildStatus::ResourceLimit,
             "no placement within slot budget");
      }
      Grow(std::min(limit_, CheckedAdd(capacity_, 65536)));
    }
  }

 private:
  uint64_t LoadWord(size_t index) const { return index < bits_.size() ? bits_[index] : 0; }
  uint64_t Read64(size_t bit) const {
    const size_t word = bit / 64;
    const unsigned shift = static_cast<unsigned>(bit % 64);
    uint64_t result = LoadWord(word) >> shift;
    if (shift) result |= LoadWord(word + 1) << (64 - shift);
    return result;
  }
  bool IsFree(size_t bit) const { return (bits_[bit / 64] >> (bit % 64)) & 1; }

  void Grow(size_t required) {
    const size_t rounded = CheckedMultiply(CheckedAdd(required, 65535) / 65536, 65536);
    const size_t next = std::min(limit_, rounded);
    budget_.CheckSlots(next);
    budget_.CheckTime();
    std::vector<uint64_t> grown((next + 63) / 64, ~uint64_t(0));
    std::copy(bits_.begin(), bits_.end(), grown.begin());
    // Every non-final growth is a multiple of 65,536, so no partial old word
    // needs to be reopened. The final partial word cannot grow again.
    if (next % 64) grown.back() &= (uint64_t(1) << (next % 64)) - 1;
    grown[0] &= ~uint64_t(1);
    bits_.swap(grown);
    capacity_ = next;
  }

  size_t limit_;
  size_t capacity_;
  size_t first_free_word_;
  const Budget& budget_;
  std::vector<uint64_t> bits_;
};

void Require(bool condition, const char* reason) {
  if (!condition) Fail(DatBuildStatus::ValidationFailed, reason);
}

}  // namespace

DatBuildResult DatBuilder::Build(const std::vector<const DictUnit*>& input,
                                 double unknownWeight,
                                 const DatBuildOptions& options) {
  DatBuildResult result;
  const Clock::time_point start = Clock::now();
  Clock::time_point phase_start = start;
  enum Phase { Preparation, Layout, Validation } phase = Preparation;
  try {
    Budget budget(options, start);
    budget.CheckTime();
    if (sizeof(double) != sizeof(uint64_t) || !std::numeric_limits<double>::is_iec559)
      Fail(DatBuildStatus::Unsupported, "IEEE 754 binary64 weights required");
    if (!std::isfinite(unknownWeight))
      Fail(DatBuildStatus::Unsupported, "unknown weight is not finite");
    const size_t slot_limit = std::min(options.max_slots, size_t(dat_detail::NO_CHECK));
    if (!slot_limit) Fail(DatBuildStatus::ResourceLimit, "root exceeds slot budget");
    // UINT32_MAX is reserved for nonterminals, so the final usable original
    // input index is UINT32_MAX - 1. Check before allocating or narrowing it.
    if (input.size() > dat_detail::NO_SOURCE_INDEX)
      Fail(DatBuildStatus::FieldLimit, "input indices exceed 32-bit source range");

    budget.Add(input.size(), sizeof(Record));
    std::vector<Record> records(input.size());
    size_t used = 0;
    for (size_t i = 0; i < input.size(); ++i) {
      if ((i & 1023U) == 0) budget.CheckTime();
      if (!input[i]) Fail(DatBuildStatus::Unsupported, "null dictionary entry");
      if (!input[i]->word.empty()) records[used++] = Record{input[i], i};
    }
    records.resize(used);
    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
      if (WordLess(a.unit->word, b.unit->word)) return true;
      if (WordLess(b.unit->word, a.unit->word)) return false;
      return a.source_index < b.source_index;
    });
    budget.CheckTime();
    used = 0;
    for (size_t i = 0; i < records.size(); ++i) {
      if (used && WordEqual(records[used - 1].unit->word, records[i].unit->word))
        records[used - 1] = records[i];
      else
        records[used++] = records[i];
    }
    records.resize(used);

    budget.Add(records.size(), sizeof(uint64_t));
    std::vector<uint64_t> weight_bits(records.size());
    size_t node_count = 1;
    size_t max_word_len = 0;
    const Unicode* previous = NULL;
    for (size_t i = 0; i < records.size(); ++i) {
      if ((i & 1023U) == 0) budget.CheckTime();
      const DictUnit& unit = *records[i].unit;
      if (!std::isfinite(unit.weight))
        Fail(DatBuildStatus::Unsupported, "effective dictionary weight is not finite");
      weight_bits[i] = WeightBits(unit.weight);
      node_count = CheckedAdd(node_count, unit.word.size() - CommonPrefix(previous, unit.word));
      max_word_len = std::max(max_word_len, unit.word.size());
      previous = &unit.word;
    }
    result.stats.logical_nodes = node_count;
    if (node_count > dat_detail::NO_CHECK)
      Fail(DatBuildStatus::FieldLimit, "logical nodes exceed packed slot range");
    if (node_count > slot_limit)
      Fail(DatBuildStatus::ResourceLimit, "logical nodes exceed slot budget");
    std::sort(weight_bits.begin(), weight_bits.end());
    weight_bits.erase(std::unique(weight_bits.begin(), weight_bits.end()), weight_bits.end());
    result.stats.unique_weights = weight_bits.size();
    if (weight_bits.size() > std::min(options.max_unique_weights, size_t(dat_detail::NO_WEIGHT)))
      Fail(DatBuildStatus::FieldLimit, "unique weights exceed packed weight range");

    budget.Add(weight_bits.size(), sizeof(double));
    budget.Add(node_count, sizeof(LogicalNode) + sizeof(uint32_t) * 3 + sizeof(int32_t));
    budget.Add(node_count - 1, sizeof(PendingEdge) + sizeof(LogicalEdge));
    budget.Add(CheckedAdd(max_word_len, 1), sizeof(uint32_t));
    budget.CheckSlots(1);
    std::unique_ptr<DatModel> model(new DatModel);
    model->uniqueWeights_.resize(weight_bits.size());
    for (size_t i = 0; i < weight_bits.size(); ++i)
      std::memcpy(&model->uniqueWeights_[i], &weight_bits[i], sizeof(double));
    std::memcpy(&model->unknownWeight_, &unknownWeight, sizeof(double));
    model->actualMaxWordLen_ = max_word_len;

    std::vector<LogicalNode> nodes(node_count);
    std::vector<PendingEdge> pending(node_count - 1);
    std::vector<LogicalEdge> edges(node_count - 1);
    std::vector<uint32_t> offsets(node_count, 0);
    std::vector<uint32_t> stack(max_word_len + 1, 0);
    std::vector<uint32_t> rows(node_count, 0);
    std::vector<int32_t> bases(node_count, 0);
    std::vector<uint32_t> states(node_count, dat_detail::NO_CHECK);
    states[0] = 0;
    uint32_t next_node = 1;
    size_t next_edge = 0;
    previous = NULL;
    for (size_t i = 0; i < records.size(); ++i) {
      if ((i & 1023U) == 0) budget.CheckTime();
      const DictUnit& unit = *records[i].unit;
      const size_t common = CommonPrefix(previous, unit.word);
      for (size_t j = common; j < unit.word.size(); ++j) {
        if ((j & 1023U) == 0) budget.CheckTime();
        const uint32_t parent = stack[j];
        const uint32_t child = next_node++;
        stack[j + 1] = child;
        pending[next_edge++] = PendingEdge{parent, unit.word[j], child};
        ++nodes[parent].edge_count;
      }
      const uint64_t bits = WeightBits(unit.weight);
      nodes[stack[unit.word.size()]].weight = static_cast<uint32_t>(
          std::lower_bound(weight_bits.begin(), weight_bits.end(), bits) - weight_bits.begin());
      nodes[stack[unit.word.size()]].source_index = static_cast<uint32_t>(records[i].source_index);
      previous = &unit.word;
    }
    Require(next_node == node_count && next_edge == pending.size(), "logical node count mismatch");
    uint32_t offset = 0;
    size_t row_count = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
      nodes[i].first_edge = offset;
      offsets[i] = offset;
      offset += nodes[i].edge_count;
      if (nodes[i].edge_count) rows[row_count++] = static_cast<uint32_t>(i);
    }
    rows.resize(row_count);
    for (size_t i = 0; i < pending.size(); ++i) {
      const PendingEdge& edge = pending[i];
      edges[offsets[edge.parent]++] = LogicalEdge{edge.rune, edge.child};
    }
    std::sort(rows.begin(), rows.end(), [&nodes, &edges](uint32_t a, uint32_t b) {
      if (nodes[a].edge_count != nodes[b].edge_count)
        return nodes[a].edge_count > nodes[b].edge_count;
      const uint32_t span_a = edges[nodes[a].first_edge + nodes[a].edge_count - 1].rune
                            - edges[nodes[a].first_edge].rune;
      const uint32_t span_b = edges[nodes[b].first_edge + nodes[b].edge_count - 1].rune
                            - edges[nodes[b].first_edge].rune;
      return span_a != span_b ? span_a > span_b : a < b;
    });

    phase = Layout;
    phase_start = Clock::now();
    FreeBitmap bitmap(slot_limit, budget);
    for (size_t i = 0; i < rows.size(); ++i) {
      budget.CheckTime();
      const uint32_t row = rows[i];
      bases[row] = bitmap.Place(&edges[nodes[row].first_edge], nodes[row].edge_count, states);
    }
    result.stats.layout_ms = Elapsed(phase_start);
    const size_t slots = static_cast<size_t>(*std::max_element(states.begin(), states.end())) + 1;
    Require(slots <= slot_limit, "unplaced logical state");
    result.stats.slot_count = slots;
    result.stats.topology_bytes = CheckedMultiply(slots, sizeof(uint64_t));
    result.stats.source_index_bytes = CheckedMultiply(slots, sizeof(uint32_t));
    result.stats.weight_bytes = CheckedMultiply(weight_bits.size(), sizeof(double));
    result.stats.model_bytes = CheckedAdd(CheckedAdd(result.stats.topology_bytes,
        result.stats.source_index_bytes), result.stats.weight_bytes);
    budget.CheckSlots(slots);
    const uint64_t empty = Pack(0, dat_detail::NO_CHECK, dat_detail::NO_WEIGHT);
    model->units_.assign(slots, empty);
    model->terminalSourceIndices_.assign(slots, dat_detail::NO_SOURCE_INDEX);
    for (size_t i = 0; i < nodes.size(); ++i) {
      model->units_[states[i]] = Pack(bases[i], dat_detail::NO_CHECK, nodes[i].weight);
      model->terminalSourceIndices_[states[i]] = nodes[i].source_index;
    }
    for (size_t i = 0; i < pending.size(); ++i) {
      const PendingEdge& edge = pending[i];
      model->units_[states[edge.child]] = Pack(bases[edge.child], states[edge.parent], nodes[edge.child].weight);
    }
    model->units_[0] = Pack(bases[0], 0, dat_detail::NO_WEIGHT);

    phase = Validation;
    phase_start = Clock::now();
    std::vector<unsigned char> seen(slots, 0);
    for (size_t i = 0; i < nodes.size(); ++i) {
      if ((i & 1023U) == 0) budget.CheckTime();
      const uint32_t state = states[i];
      Require(state < slots && !seen[state], "duplicate physical slot");
      seen[state] = 1;
      const uint64_t unit = model->units_[state];
      Require(dat_detail::DecodeBase(unit) == bases[i], "base packing mismatch");
      Require(dat_detail::DecodeWeightCode(unit) == nodes[i].weight, "weight packing mismatch");
      Require(model->terminalSourceIndices_[state] == nodes[i].source_index,
              "source index mapping mismatch");
      Require(!nodes[i].edge_count || nodes[i].first_edge < edges.size(), "invalid edge range");
      Require(nodes[i].edge_count || bases[i] == 0, "leaf has nonzero base");
    }
    for (size_t i = 0; i < pending.size(); ++i) {
      if ((i & 1023U) == 0) budget.CheckTime();
      const PendingEdge& edge = pending[i];
      DatCursor cursor{states[edge.parent], model->units_[states[edge.parent]]};
      Require(model->StepRaw(edge.rune, cursor) && cursor.state == states[edge.child], "logical edge transition mismatch");
      Require(dat_detail::DecodeCheck(cursor.unit) == states[edge.parent], "check packing mismatch");
    }
    Require(dat_detail::DecodeCheck(model->Root().unit) == 0 && !model->IsTerminal(model->Root()), "invalid root");
    for (size_t i = 0; i < slots; ++i) {
      if ((i & 1023U) == 0) budget.CheckTime();
      const uint64_t unit = model->units_[i];
      const uint32_t source_index = model->terminalSourceIndices_[i];
      const uint32_t weight = dat_detail::DecodeWeightCode(unit);
      Require(!(unit >> 56), "nonzero reserved bits");
      Require(weight == dat_detail::NO_WEIGHT
                  ? source_index == dat_detail::NO_SOURCE_INDEX
                  : source_index < input.size(),
              "invalid terminal source index");
      if (!seen[i]) Require(unit == empty, "invalid empty slot");
      else {
        Require(dat_detail::DecodeCheck(unit) < slots, "invalid check index");
        Require(weight == dat_detail::NO_WEIGHT || weight < model->uniqueWeights_.size(), "invalid weight index");
      }
    }
    for (size_t i = 0; i < records.size(); ++i) {
      if ((i & 255U) == 0) budget.CheckTime();
      const DictUnit& word = *records[i].unit;
      DatCursor cursor = model->Root();
      for (size_t j = 0; j < word.word.size(); ++j) {
        if ((j & 1023U) == 0) budget.CheckTime();
        Require(model->StepRaw(word.word[j], cursor), "effective word transition mismatch");
      }
      Require(model->IsTerminal(cursor), "effective word is not terminal");
      Require(model->TerminalSourceIndex(cursor) == records[i].source_index,
              "effective word source index differs");
      Require(input[model->TerminalSourceIndex(cursor)] == records[i].unit,
              "effective word source payload differs");
      Require(WeightBits(model->TerminalWeight(cursor)) == WeightBits(word.weight), "effective word weight bits differ");
    }
    budget.CheckTime();
    result.stats.validation_ms = Elapsed(phase_start);
    result.stats.status = DatBuildStatus::Success;
    result.model = std::move(model);
  } catch (const BuildFailure& failure) {
    result.stats.status = failure.status;
    try {
      result.stats.reason = failure.reason;
    } catch (const std::bad_alloc&) {
      result.stats.status = DatBuildStatus::ResourceLimit;
    }
  } catch (const std::bad_alloc&) {
    result.stats.status = DatBuildStatus::ResourceLimit;
    try {
      result.stats.reason = "allocation";
    } catch (const std::bad_alloc&) {
      // A library without small-string storage may also fail to save a reason.
      // The structured status still guarantees a safe legacy-backend fallback.
    }
  }
  if (!result.model && phase == Layout) result.stats.layout_ms = Elapsed(phase_start);
  if (!result.model && phase == Validation) result.stats.validation_ms = Elapsed(phase_start);
  result.stats.build_ms = Elapsed(start);
  return result;
}

}  // namespace cppjieba
