#ifndef CPPJIEBA_CPU_CUT_OPTIONS_HPP
#define CPPJIEBA_CPU_CUT_OPTIONS_HPP

#include "DatBuilder.hpp"

namespace cppjieba {

enum class CpuCutMode { LegacyDag, PointerFused, DatRawFused };

// Non-legacy dictionaries are frozen after startup, even after DAT fallback.
struct CpuCutOptions {
  CpuCutMode mode;
  bool optimize_hmm;
  size_t hmm_dense_budget_bytes;
  DatBuildOptions dat_build_options;
  CpuCutOptions() : mode(CpuCutMode::LegacyDag), optimize_hmm(false),
      hmm_dense_budget_bytes(8 * 1024 * 1024) {}
};

} // namespace cppjieba
#endif
