#ifndef CPPJIEBA_HMM_SCRATCH_HPP
#define CPPJIEBA_HMM_SCRATCH_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cppjieba {

// Owned by one segmentation request and reusable across that request's ranges.
// Never share a scratch object between concurrent calls.
struct HmmScratch {
  double prev[4];
  double cur[4];
  std::vector<uint8_t> path;
  std::vector<uint8_t> status;

  void Prepare(size_t rune_count) {
    if (rune_count > std::numeric_limits<size_t>::max() / 4) {
      throw std::length_error("HMM path size overflow");
    }
    path.resize(rune_count * 4);
    status.resize(rune_count);
  }
};

}  // namespace cppjieba

#endif
