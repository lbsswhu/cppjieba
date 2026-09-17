// Compile this unchanged against either checkout to compare their observable
// dictionary metadata, token text, token offsets and POS results by checksum.
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include "cppjieba/Jieba.hpp"

class Digest {
 public:
  Digest() : value_(UINT64_C(14695981039346656037)) {}
  void Bytes(const void* data, size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { value_ ^= p[i]; value_ *= UINT64_C(1099511628211); }
  }
  void Integer(uint64_t value) {
    for (size_t i = 0; i < 8; ++i) { const unsigned char b = (value >> (8 * i)) & 255; Bytes(&b, 1); }
  }
  void Text(const std::string& text) { Integer(text.size()); Bytes(text.data(), text.size()); }
  uint64_t Value() const { return value_; }
 private:
  uint64_t value_;
};

int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: bitmap_differential REPO_ROOT\n"; return 1; }
  const std::string root = argv[1];
  cppjieba::Jieba jieba(root + "/dict/jieba.dict.utf8", root + "/dict/hmm_model.utf8",
      root + "/dict/user.dict.utf8", root + "/dict/idf.utf8", root + "/dict/stop_words.utf8");
  const cppjieba::DictTrie* dict = jieba.GetDictTrie();
  std::vector<std::string> words;
  Digest dictionary, tokens;
  const char* dict_files[] = {"/dict/jieba.dict.utf8", "/dict/user.dict.utf8"};
  for (const char* file : dict_files) {
    std::ifstream input((root + file).c_str());
    if (!input) return 2;
    std::string word, tail;
    while (input >> word) {
      std::getline(input, tail);
      words.push_back(word);
      cppjieba::RuneStrArray runes;
      if (!cppjieba::DecodeUTF8RunesInString(word, runes)) return 3;
      const cppjieba::DictUnit* unit = dict->Find(runes.begin(), runes.end());
      if (!unit) return 4;
      dictionary.Text(word);
      uint64_t bits;
      std::memcpy(&bits, &unit->weight, sizeof(bits));
      dictionary.Integer(bits);
      dictionary.Text(unit->tag);
      dictionary.Integer(unit->word.size());
      for (size_t i = 0; i < unit->word.size(); ++i) dictionary.Integer(unit->word[i]);
      cppjieba::DecodeUTF8RunesInString(word + "\xf4\x8f\xbf\xbf", runes);
      if (dict->Find(runes.begin(), runes.end())) return 5;
    }
  }
  std::vector<std::string> texts;
  const char* corpus_files[] = {"/test/testdata/synthetic_doc.utf8", "/test/testdata/testlines.utf8", "/test/testdata/review.100"};
  for (const char* file : corpus_files) {
    std::ifstream input((root + file).c_str());
    if (!input) return 6;
    std::string line;
    while (std::getline(input, line)) texts.push_back(line);
  }
  std::mt19937 rng(20260917);
  for (size_t i = 0; i < 1000; ++i) {
    std::string text;
    for (size_t j = 0; j < 5; ++j) text += words[rng() % words.size()];
    text += " ABC123.45é😀未知词\xf4\x8f\xbf\xbf";
    texts.push_back(text);
  }
  for (const std::string& text : texts) {
    for (int mode = 0; mode < 5; ++mode) {
      std::vector<cppjieba::Word> output;
      if (mode == 0) jieba.Cut(text, output, false);
      if (mode == 1) jieba.Cut(text, output, true);
      if (mode == 2) jieba.CutAll(text, output);
      if (mode == 3) jieba.CutForSearch(text, output, false);
      if (mode == 4) jieba.CutForSearch(text, output, true);
      tokens.Integer(output.size());
      for (const cppjieba::Word& word : output) {
        tokens.Text(word.word);
        tokens.Integer(word.offset);
        tokens.Integer(word.unicode_offset);
        tokens.Integer(word.unicode_length);
      }
    }
    std::vector<std::pair<std::string, std::string> > tags;
    jieba.Tag(text, tags);
    tokens.Integer(tags.size());
    for (const auto& item : tags) { tokens.Text(item.first); tokens.Text(item.second); }
  }
  std::cout << "dictionary_records=" << words.size() << " dictionary_digest=" << dictionary.Value()
            << " texts=" << texts.size() << " token_digest=" << tokens.Value() << '\n';
}
