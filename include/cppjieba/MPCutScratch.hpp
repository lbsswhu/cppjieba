#ifndef CPPJIEBA_MP_CUT_SCRATCH_HPP
#define CPPJIEBA_MP_CUT_SCRATCH_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cppjieba {
struct MPCutScratch {
  std::vector<double> dpRing;
  std::vector<uint16_t> bestLen;
  std::vector<size_t> bestLenWide;
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
  size_t transitions = 0;
  size_t candidates = 0;
  size_t fused_ranges = 0;
  size_t wide_ranges = 0;
#endif
};
} // namespace cppjieba
#endif
