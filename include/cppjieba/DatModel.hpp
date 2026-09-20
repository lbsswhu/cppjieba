#ifndef CPPJIEBA_DAT_MODEL_HPP
#define CPPJIEBA_DAT_MODEL_HPP

#include <cassert>
#include <cstdint>
#include <vector>
#include "Unicode.hpp"

namespace cppjieba {

namespace dat_detail {
constexpr uint32_t BASE_MASK = (1U << 22) - 1;
constexpr uint32_t NO_CHECK = (1U << 21) - 1;
constexpr uint32_t NO_WEIGHT = (1U << 13) - 1;

inline int32_t DecodeBase(uint64_t unit) {
  const uint32_t raw = static_cast<uint32_t>(unit & BASE_MASK);
  return (raw & (1U << 21)) ? static_cast<int32_t>(raw) - (1 << 22)
                            : static_cast<int32_t>(raw);
}
inline uint32_t DecodeCheck(uint64_t unit) {
  return static_cast<uint32_t>((unit >> 22) & NO_CHECK);
}
inline uint32_t DecodeWeightCode(uint64_t unit) {
  return static_cast<uint32_t>((unit >> 43) & NO_WEIGHT);
}
}  // namespace dat_detail

struct DatCursor {
  uint32_t state;
  uint64_t unit;
};

// Cursors must originate from this model; a failed transition preserves them.
class DatModel {
 public:
  DatCursor Root() const { return DatCursor{0, units_[0]}; }

  bool StepRaw(Rune rune, DatCursor& cursor) const {
    const int64_t target = static_cast<int64_t>(dat_detail::DecodeBase(cursor.unit))
                         + static_cast<int64_t>(rune);
    if (target <= 0 || static_cast<uint64_t>(target) >= units_.size()) return false;
    const uint64_t next = units_[static_cast<size_t>(target)];
    if (dat_detail::DecodeCheck(next) != cursor.state) return false;
    cursor.state = static_cast<uint32_t>(target);
    cursor.unit = next;
    return true;
  }

  bool IsTerminal(const DatCursor& cursor) const {
    return dat_detail::DecodeWeightCode(cursor.unit) != dat_detail::NO_WEIGHT;
  }
  double TerminalWeight(const DatCursor& cursor) const {
    assert(IsTerminal(cursor));
    return uniqueWeights_[dat_detail::DecodeWeightCode(cursor.unit)];
  }
  size_t ActualMaxWordLen() const { return actualMaxWordLen_; }
  double UnknownWeight() const { return unknownWeight_; }

 private:
  friend class DatBuilder;
  DatModel() : unknownWeight_(0), actualMaxWordLen_(0) {}
  std::vector<uint64_t> units_;
  std::vector<double> uniqueWeights_;
  double unknownWeight_;
  size_t actualMaxWordLen_;
};

}  // namespace cppjieba
#endif
