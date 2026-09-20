#ifndef CPPJIEBA_CPU_CUT_OPTIONS_HPP
#define CPPJIEBA_CPU_CUT_OPTIONS_HPP

#include "DatBuilder.hpp"

namespace cppjieba {

enum class CpuCutMode { DatRawFused = 2 };

// Dictionaries use raw DAT exclusively and are frozen after startup.
struct CpuCutOptions {
  CpuCutMode mode;
  bool optimize_hmm;
  size_t hmm_dense_budget_bytes;
  DatBuildOptions dat_build_options;
  CpuCutOptions() : mode(CpuCutMode::DatRawFused), optimize_hmm(false),
      hmm_dense_budget_bytes(8 * 1024 * 1024) {}
};

} // namespace cppjieba
#endif
