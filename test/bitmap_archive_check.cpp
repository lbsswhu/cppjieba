#include "cppjieba/BitmapDoubleArrayTrie.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: bitmap_archive_check ARCHIVE_DIR\n"; return 2; }
  const std::string root(argv[1]);
  std::ifstream main((root + "/input/jieba.dict.utf8").c_str());
  if (!main) return 2;
  std::vector<cppjieba::Unicode> keys;
  std::vector<double> weights;
  std::string word, tag;
  double frequency, total = 0;
  while (main >> word >> frequency >> tag) {
    keys.push_back(cppjieba::DecodeUTF8RunesInString(word));
    weights.push_back(frequency);
    total += frequency;
  }
  for (size_t i = 0; i < weights.size(); ++i) weights[i] = std::log(weights[i] / total);
  std::vector<double> sorted(weights);
  std::sort(sorted.begin(), sorted.end());
  if (sorted.empty()) return 2;
  double median = sorted[sorted.size() / 2];
  std::ifstream user((root + "/input/user.dict.utf8").c_str());
  if (!user) return 2;
  std::string line;
  while (std::getline(user, line)) {
    std::istringstream fields(line);
    std::vector<std::string> parts;
    while (fields >> word) parts.push_back(word);
    if (parts.empty()) continue;
    keys.push_back(cppjieba::DecodeUTF8RunesInString(parts[0]));
    weights.push_back(parts.size() == 3 ? std::log(std::stod(parts[1]) / total) : median);
  }
  auto started = std::chrono::steady_clock::now();
  cppjieba::BitmapDoubleArrayTrie trie;
  trie.Build(keys, weights);
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "input=" << keys.size() << " nodes=" << trie.NodeCount() << " slots=" << trie.SlotCount()
            << " weights=" << trie.Weights().size() << " bytes=" << trie.ModelBytes()
            << " auxiliary=" << trie.AuxiliaryBytes() << " seconds=" << seconds << std::endl;
  for (size_t i = 0; i < keys.size(); ++i) {
    uint32_t state = 0;
    for (size_t j = 0; j < keys[i].size(); ++j) state = trie.Transition(state, keys[i][j]);
    if (!trie.IsTerminal(state) || trie.Weight(state) != weights[i]) {
      std::cerr << "lookup failed at " << i << std::endl;
      return 1;
    }
  }
  // Decode the archive's little-endian bytes explicitly, independent of host endianness.
  std::ifstream units((root + "/model/dat.units.u64le").c_str(), std::ios::binary);
  std::ifstream table((root + "/model/weights.f64le").c_str(), std::ios::binary);
  if (!units || !table) return 2;
  const auto read_u64 = [](std::istream& stream, uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      const int byte = stream.get();
      if (byte == std::char_traits<char>::eof()) return false;
      value |= static_cast<uint64_t>(static_cast<unsigned char>(byte)) << shift;
    }
    return true;
  };
  for (uint64_t expected : trie.Units()) {
    uint64_t actual;
    if (!read_u64(units, actual) || actual != expected) {
      std::cerr << "Packed Unit differs from archive\n";
      return 1;
    }
  }
  for (double expected : trie.Weights()) {
    uint64_t actual, bits;
    std::memcpy(&bits, &expected, sizeof(bits));
    if (!read_u64(table, actual) || actual != bits) {
      std::cerr << "C++ FP64 weight differs from archived Python weight\n";
      return 1;
    }
  }
  if (units.peek() != std::char_traits<char>::eof() || table.peek() != std::char_traits<char>::eof()) return 1;
  std::cout << "archive_units_and_weights=byte_identical\n";
}
