#ifndef CPPJIEBA_TRIE_HPP
#define CPPJIEBA_TRIE_HPP

#include <cstddef>

#include "Utils.hpp"
#include "Unicode.hpp"

namespace cppjieba {

const std::size_t MAX_WORD_LENGTH = 512;

struct DagEdge {
  std::size_t end;
  double weight;
  bool in_dict;

  DagEdge(): end(0), weight(0.0), in_dict(false) {
  }
  DagEdge(std::size_t edge_end, double edge_weight, bool edge_in_dict)
      : end(edge_end), weight(edge_weight), in_dict(edge_in_dict) {
  }
}; // struct DagEdge

struct Dag {
  RuneStr runestr;
  LocalVector<DagEdge> edges;
  double weight;
  std::size_t next_pos;

  Dag(): runestr(), edges(), weight(0.0), next_pos(0) {
  }
}; // struct Dag
} // namespace cppjieba

#endif // CPPJIEBA_TRIE_HPP
