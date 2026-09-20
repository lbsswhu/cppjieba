#ifndef CPPJIEBA_DICT_TYPES_HPP
#define CPPJIEBA_DICT_TYPES_HPP

#include "Utils.hpp"
#include "Unicode.hpp"

namespace cppjieba {
const size_t MAX_WORD_LENGTH = 512;

struct DictUnit {
  Unicode word;
  double weight;
  std::string tag;
};

// Compatibility data for callers that enumerate all dictionary candidates.
// Dictionary traversal itself is implemented exclusively by DatModel.
struct Dag {
  RuneStr runestr;
  LocalVector<std::pair<size_t, const DictUnit*> > nexts;
  const DictUnit* pInfo;
  double weight;
  size_t nextPos;
  Dag() : runestr(), pInfo(NULL), weight(0.0), nextPos(0) {}
};
} // namespace cppjieba
#endif
