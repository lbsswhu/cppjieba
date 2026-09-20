// Compile this exact file twice, once against the previous checkout with
// CPPJIEBA_CPU_PREVIOUS, and once against the DAT-only library. The runner
// compares complete transcripts from independent processes, not local hashes.
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include "cppjieba/Jieba.hpp"

namespace {
using namespace cppjieba;

std::string Hex(const std::string& text) {
  static const char digits[] = "0123456789abcdef";
  std::string result(text.size() * 2, '0');
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    result[2 * i] = digits[c >> 4]; result[2 * i + 1] = digits[c & 15];
  }
  return result;
}
unsigned Nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  throw std::runtime_error("Invalid hexadecimal input");
}
std::string Unhex(const std::string& text) {
  if (text.size() % 2) throw std::runtime_error("Odd hexadecimal input length");
  std::string result(text.size() / 2, '\0');
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<char>((Nibble(text[2 * i]) << 4) | Nibble(text[2 * i + 1]));
  return result;
}
void Value(const std::string& value) { std::cout << 'x' << Hex(value); }
void Value(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  std::cout << std::hex << std::setw(16) << std::setfill('0') << bits << std::dec;
}
void Value(const Word& value) {
  Value(value.word);
  std::cout << ':' << value.offset << ':' << value.unicode_offset << ':' << value.unicode_length;
}
void Value(const KeywordExtractor::Word& value) {
  Value(value.word); std::cout << ':'; Value(value.weight);
  std::cout << ':' << value.offsets.size();
  for (size_t i = 0; i < value.offsets.size(); ++i) std::cout << ',' << value.offsets[i];
}
template<class T> void Value(const std::pair<std::string, T>& value) {
  Value(value.first); std::cout << ':'; Value(value.second);
}
template<class T> void Output(size_t case_id, const std::string& api, const std::vector<T>& output) {
  std::cout << case_id << ' ' << api << ' ' << output.size();
  for (size_t i = 0; i < output.size(); ++i) { std::cout << ' '; Value(output[i]); }
  std::cout << '\n';
}
void Lookup(Jieba& jieba, size_t case_id, const std::string& text) {
  RuneStrArray runes;
  DecodeUTF8RunesInString(text, runes);
  const DictUnit* unit = jieba.GetDictTrie()->Find(runes.begin(), runes.end());
  std::cout << case_id << " find " << jieba.Find(text) << ' ' << (unit != NULL) << ' ';
  Value(jieba.LookupTag(text));
  if (unit) {
    std::cout << ' '; Value(unit->weight); std::cout << ' '; Value(unit->tag);
    std::cout << ' ' << unit->word.size();
    for (size_t i = 0; i < unit->word.size(); ++i) std::cout << ':' << unit->word[i];
  }
  std::cout << '\n';
  std::cout << case_id << " user-single " << runes.size();
  for (size_t i = 0; i < runes.size(); ++i)
    std::cout << ' ' << jieba.GetDictTrie()->IsUserDictSingleChineseWord(runes[i].rune);
  std::cout << '\n';
}
void Check(Jieba& jieba, size_t case_id, const std::string& text, bool only_mp) {
  const size_t limits[] = {0, 1, 2, 4, 16, 24, 512, 513, 65535, 65536,
                           std::numeric_limits<size_t>::max()};
  for (size_t l = 0; l < sizeof(limits) / sizeof(limits[0]); ++l) {
    std::vector<std::string> words(1, "sentinel");
    std::vector<Word> located(1, Word("sentinel", 7, 8, 9));
    jieba.CutSmall(text, words, limits[l]); jieba.CutSmall(text, located, limits[l]);
    const std::string label = "small:" + std::to_string(limits[l]);
    Output(case_id, label, words); Output(case_id, label + ":coords", located);
  }
  Lookup(jieba, case_id, text);
  if (only_mp) return;
  for (int hmm = 0; hmm < 2; ++hmm) {
    std::vector<std::string> words(1, "sentinel");
    std::vector<Word> located(1, Word("sentinel", 7, 8, 9));
    const std::string flag = std::to_string(hmm);
    jieba.Cut(text, words, hmm); jieba.Cut(text, located, hmm);
    Output(case_id, "cut:" + flag, words); Output(case_id, "cut-coords:" + flag, located);
    jieba.CutForSearch(text, words, hmm); jieba.CutForSearch(text, located, hmm);
    Output(case_id, "search:" + flag, words); Output(case_id, "search-coords:" + flag, located);
  }
  std::vector<std::string> words(1, "sentinel");
  std::vector<Word> located(1, Word("sentinel", 7, 8, 9));
  jieba.CutAll(text, words); jieba.CutAll(text, located);
  Output(case_id, "all", words); Output(case_id, "all-coords", located);
  jieba.CutHMM(text, words); jieba.CutHMM(text, located);
  Output(case_id, "hmm", words); Output(case_id, "hmm-coords", located);
  std::vector<std::pair<std::string, std::string> > tags(1, std::make_pair("sentinel", "tag"));
  jieba.Tag(text, tags); Output(case_id, "tag", tags);
  for (size_t i = 0; i < tags.size(); ++i) Lookup(jieba, case_id, tags[i].first);
  const size_t tops[] = {0, 1, 5, 100};
  for (size_t i = 0; i < sizeof(tops) / sizeof(tops[0]); ++i) {
    words.assign(1, "sentinel");
    std::vector<std::pair<std::string, double> > weights(1, std::make_pair("sentinel", -0.0));
    KeywordExtractor::Word seed;
    seed.word = "sentinel"; seed.weight = -0.0; seed.offsets.push_back(99);
    std::vector<KeywordExtractor::Word> offsets(1, seed);
    jieba.extractor.Extract(text, words, tops[i]);
    jieba.extractor.Extract(text, weights, tops[i]);
    jieba.extractor.Extract(text, offsets, tops[i]);
    const std::string suffix = std::to_string(tops[i]);
    Output(case_id, "keywords:" + suffix, words);
    Output(case_id, "keyword-bits:" + suffix, weights);
    Output(case_id, "keyword-offsets:" + suffix, offsets);
  }
  RuneStrArray runes;
  if (DecodeUTF8RunesInString(text, runes)) {
    const size_t dag_limits[] = {0, 2, 512, std::numeric_limits<size_t>::max()};
    for (size_t l = 0; l < 4; ++l) {
      std::vector<Dag> dag;
      jieba.GetDictTrie()->Find(runes.begin(), runes.end(), dag, dag_limits[l]);
      for (size_t i = 0; i < dag.size(); ++i) {
        std::cout << case_id << " candidates:" << dag_limits[l] << ':' << i << ' ' << dag[i].nexts.size();
        for (size_t j = 0; j < dag[i].nexts.size(); ++j) {
          const std::pair<size_t, const DictUnit*>& edge = dag[i].nexts[j];
          std::cout << ' ' << edge.first << ':' << (edge.second != NULL) << ':';
          Value(edge.second ? edge.second->weight : jieba.GetDictTrie()->GetMinWeight());
          if (edge.second) { std::cout << ':'; Value(edge.second->tag); }
        }
        std::cout << '\n';
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) throw std::runtime_error("usage: dat_only_oracle REPO [--mode A/C/D] [--dict PATH] [--users PATH]");
    const std::string repo = argv[1], dir = repo + "/dict/";
    std::string mode = "C", dict = dir + "jieba.dict.utf8", users = dir + "user.dict.utf8", separators;
    bool only_mp = false, reset_separators = false;
    CpuCutOptions options;
    for (int i = 2; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--only-mp") { only_mp = true; continue; }
      if (i + 1 == argc) throw std::runtime_error("Missing option value");
      const std::string value = argv[++i];
      if (flag == "--mode") mode = value;
      else if (flag == "--dict") dict = value;
      else if (flag == "--users") users = value;
      else if (flag == "--hmm-budget") options.hmm_dense_budget_bytes = std::stoull(value);
      else if (flag == "--separators") { separators = value; reset_separators = true; }
      else throw std::runtime_error("Unknown option: " + flag);
    }
    if (mode != "A" && mode != "C" && mode != "D") throw std::runtime_error("Invalid mode");
#ifdef CPPJIEBA_CPU_PREVIOUS
    options.mode = mode == "A" ? CpuCutMode::LegacyDag : CpuCutMode::DatRawFused;
#else
    if (mode == "A") throw std::runtime_error("DAT-only oracle has no Legacy backend");
    options.mode = CpuCutMode::DatRawFused;
#endif
    options.optimize_hmm = mode == "D";
    Jieba jieba(dict, dir + "hmm_model.utf8", users, dir + "idf.utf8", dir + "stop_words.utf8", options);
    if (jieba.GetDictTrie()->GetCpuCutMode() != options.mode)
      throw std::runtime_error("Oracle dictionary unexpectedly changed backend");
    if (reset_separators) jieba.ResetSeparators(separators);
    std::string line;
    size_t case_id = 0;
    while (std::getline(std::cin, line)) {
      if (line.size() < 2 || line[1] != ' ') throw std::runtime_error("Expected T/F and hex text");
      const std::string text = Unhex(line.substr(2));
      if (line[0] == 'T') Check(jieba, case_id, text, only_mp);
      else if (line[0] == 'F') Lookup(jieba, case_id, text);
      else throw std::runtime_error("Unknown input record type");
      ++case_id;
    }
    if (!std::cout) throw std::runtime_error("Oracle transcript write failed");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "dat-only oracle: " << error.what() << '\n';
    return 1;
  }
}
