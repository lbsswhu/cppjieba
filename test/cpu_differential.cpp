// DAT-only contract, HMM-equivalence and concurrent-read checks.
// Independent previous-version output comparison lives in compare_dat_only.py.
// Standalone: c++ -std=c++11 -O2 -pthread -Iinclude test/cpu_differential.cpp src/DatBuilder.cpp -o cpu_differential
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "cppjieba/Jieba.hpp"

namespace {
using namespace cppjieba;

void Require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

bool SameDouble(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

bool Same(const Word& a, const Word& b) {
  return a.word == b.word && a.offset == b.offset &&
      a.unicode_offset == b.unicode_offset && a.unicode_length == b.unicode_length;
}
bool Same(const KeywordExtractor::Word& a, const KeywordExtractor::Word& b) {
  return a.word == b.word && a.offsets == b.offsets && SameDouble(a.weight, b.weight);
}
bool Same(const std::pair<std::string, double>& a, const std::pair<std::string, double>& b) {
  return a.first == b.first && SameDouble(a.second, b.second);
}
template <typename T> bool Same(const T& a, const T& b) { return a == b; }

template <typename T>
void Compare(const std::vector<T>& a, const std::vector<T>& b,
             const char* api, const std::string& input) {
  std::ostringstream why;
  why << api << " mismatch: input bytes=" << input.size() << " prefix=" << input.substr(0, 160);
  Require(a.size() == b.size(), why.str() + " (output size)");
  for (size_t i = 0; i < a.size(); ++i) {
    if (!Same(a[i], b[i])) {
      why << " (element " << i << ")";
      throw std::runtime_error(why.str());
    }
  }
}

std::unique_ptr<Jieba> MakeJieba(const std::string& repo, const CpuCutOptions& options,
                               const std::string& users = "") {
  const std::string dict = repo + "/dict/";
  return std::unique_ptr<Jieba>(new Jieba(dict + "jieba.dict.utf8", dict + "hmm_model.utf8",
      users.empty() ? dict + "user.dict.utf8" : users,
      dict + "idf.utf8", dict + "stop_words.utf8", options));
}

void CheckAll(const Jieba& old, const Jieba& candidate, const std::string& input) {
  // Nonempty outputs deliberately exercise each overload's existing clear/append contract.
  for (unsigned hmm = 0; hmm < 2; ++hmm) {
    std::vector<std::string> a(1, "sentinel"), b(a);
    std::vector<Word> wa(1, Word("sentinel", 7, 8, 9)), wb(wa);
    old.Cut(input, a, hmm); candidate.Cut(input, b, hmm);
    Compare(a, b, "Cut(strings)", input);
    old.Cut(input, wa, hmm); candidate.Cut(input, wb, hmm);
    Compare(wa, wb, "Cut(coordinates)", input);
    old.CutForSearch(input, a, hmm); candidate.CutForSearch(input, b, hmm);
    Compare(a, b, "Search(strings)", input);
    old.CutForSearch(input, wa, hmm); candidate.CutForSearch(input, wb, hmm);
    Compare(wa, wb, "Search(coordinates)", input);
  }
  std::vector<std::string> a(1, "sentinel"), b(a);
  std::vector<Word> wa(1, Word("sentinel", 7, 8, 9)), wb(wa);
  old.CutAll(input, a); candidate.CutAll(input, b);
  Compare(a, b, "CutAll(strings)", input);
  old.CutAll(input, wa); candidate.CutAll(input, wb);
  Compare(wa, wb, "CutAll(coordinates)", input);
  old.CutHMM(input, a); candidate.CutHMM(input, b);
  Compare(a, b, "CutHMM(strings)", input);
  old.CutHMM(input, wa); candidate.CutHMM(input, wb);
  Compare(wa, wb, "CutHMM(coordinates)", input);
  const size_t lengths[] = {0, 1, 2, 4, 17, 512, 513, 65535, 65536,
                           std::numeric_limits<size_t>::max()};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    old.CutSmall(input, a, lengths[i]); candidate.CutSmall(input, b, lengths[i]);
    Compare(a, b, "CutSmall(strings)", input);
    old.CutSmall(input, wa, lengths[i]); candidate.CutSmall(input, wb, lengths[i]);
    Compare(wa, wb, "CutSmall(coordinates)", input);
  }
  std::vector<std::pair<std::string, std::string> > ta(1, std::make_pair("sentinel", "tag")), tb(ta);
  old.Tag(input, ta); candidate.Tag(input, tb);
  Compare(ta, tb, "Tag", input);
  for (size_t i = 0; i < ta.size(); ++i) {
    Require(old.LookupTag(ta[i].first) == candidate.LookupTag(ta[i].first), "LookupTag mismatch");
  }
  const size_t limits[] = {0, 1, 5, 100};
  for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
    a.assign(1, "sentinel"); b = a;
    old.extractor.Extract(input, a, limits[i]); candidate.extractor.Extract(input, b, limits[i]);
    Compare(a, b, "keywords(strings)", input);
    std::vector<std::pair<std::string, double> > pa(1, std::make_pair("sentinel", -0.0)), pb(pa);
    old.extractor.Extract(input, pa, limits[i]); candidate.extractor.Extract(input, pb, limits[i]);
    Compare(pa, pb, "keywords(weight bits)", input);
    KeywordExtractor::Word seed;
    seed.word = "sentinel"; seed.offsets.push_back(99); seed.weight = -0.0;
    std::vector<KeywordExtractor::Word> ka(1, seed), kb(ka);
    old.extractor.Extract(input, ka, limits[i]); candidate.extractor.Extract(input, kb, limits[i]);
    Compare(ka, kb, "keywords(offsets and weight bits)", input);
  }
}

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& contents) {
    static std::atomic<unsigned> sequence(0);
    const char* tmp = std::getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp) tmp = std::getenv("TEMP");
#endif
    std::ostringstream name;
    name << (tmp ? tmp : "/tmp") << "/cppjieba-cpu-"
         << std::chrono::high_resolution_clock::now().time_since_epoch().count()
         << '-' << sequence.fetch_add(1) << ".dict";
    path = name.str();
    std::ofstream out(path.c_str(), std::ios::binary);
    out << contents;
    Require(static_cast<bool>(out), "Cannot write temporary dictionary: " + path);
  }
  ~TempFile() { std::remove(path.c_str()); }
};

template <typename F> void MustReject(F f, const char* name) {
  bool rejected = false;
  try { f(); } catch (const std::logic_error&) { rejected = true; }
  Require(rejected, std::string("Frozen writer did not throw logic_error: ") + name);
}

void CheckFrozen(DictTrie& dict, const std::string& user_path) {
  Require(dict.IsDictionaryFrozen(), "Requested optimized model must remain frozen");
  RuneStrArray runes;
  Require(DecodeUTF8RunesInString("南京市", runes), "fixture decoding");
  const DictUnit* before = dict.Find(runes.begin(), runes.end());
  Require(before != NULL, "fixture dictionary word missing");
  const double weight = before->weight;
  const bool protected_before = dict.IsUserDictSingleChineseWord(0x5357);
  std::vector<Word> expected, actual;
  MPSegment mp(&dict);
  mp.Cut("南京市长江大桥测试新词", expected);
  Require(!dict.InsertUserWord("测试新词", "n"), "Frozen InsertUserWord(tag) accepted");
  Require(!dict.InsertUserWord("测试新词", 999999, "n"), "Frozen InsertUserWord(freq) accepted");
  Require(!dict.DeleteUserWord("南京市"), "Frozen DeleteUserWord accepted");
  MustReject([&]() { dict.InserUserDictNode("南 123 n"); }, "InserUserDictNode");
  MustReject([&]() { dict.LoadUserDict(std::vector<std::string>(1, "南 123 n")); }, "LoadUserDict(vector)");
  MustReject([&]() { dict.LoadUserDict(std::set<std::string>{"南 123 n"}); }, "LoadUserDict(set)");
  MustReject([&]() { dict.LoadUserDict(user_path); }, "LoadUserDict(path)");
  MustReject([&]() { dict.LoadUserDict(std::vector<std::string>()); }, "LoadUserDict(empty)");
  Require(dict.Find(runes.begin(), runes.end()) == before && SameDouble(before->weight, weight),
          "Frozen mutation changed dictionary payload");
  Require(dict.IsUserDictSingleChineseWord(0x5357) == protected_before,
          "Frozen mutation changed protected single Rune set");
  Require(!dict.Find("测试新词"), "Rejected insertion changed lookup");
  mp.Cut("南京市长江大桥测试新词", actual);
  Compare(expected, actual, "frozen mutation results", "");
}

void CheckFixtures() {
  std::ostringstream contents;
  contents << "南京 8 n\n南京市 12 ns\n长江 9 n\n大桥 6 n\n长江大桥 12 n\n甲 1 n\n甲甲 2 n\n";
  std::string prefix;
  for (size_t n = 1; n <= 24; ++n) {
    prefix += "甲";
    contents << prefix << ' ' << n + 1 << " n\n";
  }
  TempFile main_dict(contents.str());
  TempFile users1("南京市 123 first\n南 nz\n𠀀 5 x\n启动用户词\n");
  TempFile users2("南京市 456 last\n");
  const std::string users = users1.path + "|" + users2.path;
  DictTrie candidate(main_dict.path, users);
  Require(candidate.IsDictionaryFrozen(), "Default DAT-only model must be frozen");
  Require(candidate.GetCpuCutMode() == CpuCutMode::DatRawFused, "Default DAT-only mode changed");
  RuneStrArray r;
  DecodeUTF8RunesInString("南京市", r);
  Require(candidate.Find(r.begin(), r.end())->tag == "last", "Startup override order changed");
  Require(candidate.IsUserDictSingleChineseWord(0x5357), "Startup single Rune not protected");
  CheckFrozen(candidate, users1.path);
  // A failed DAT construction must fail clearly; no Pointer Trie remains to
  // support the former implicit fallback.
  for (int budget = 0; budget < 4; ++budget) {
    CpuCutOptions options;
    if (budget == 0) options.dat_build_options.max_slots = 1;
    if (budget == 1) options.dat_build_options.max_temporary_bytes = 1;
    if (budget == 2) options.dat_build_options.max_unique_weights = 1;
    if (budget == 3) options.dat_build_options.max_build_ms = 0.000001;
    bool rejected = false;
    try { DictTrie failure(main_dict.path, users, DictTrie::WordWeightMedian, options); }
    catch (const DatBuildError& error) {
      rejected = true;
      Require(!error.GetStats().reason.empty(), "DAT construction error needs a reason");
      Require(error.GetStats().status != DatBuildStatus::Success, "Failed build reported success");
    }
    Require(rejected, "Insufficient DAT budget did not reject construction");
  }
  TempFile nonfinite_users("南京市 0 nz\n");
  bool rejected = false;
  try { DictTrie invalid(main_dict.path, nonfinite_users.path); }
  catch (const DatBuildError& error) {
    rejected = true;
    Require(error.GetStats().status == DatBuildStatus::Unsupported,
            "Non-finite model did not report Unsupported");
  }
  Require(rejected, "Non-finite startup weight did not reject construction");
}

void CheckLongWords() {
  // A distinct first Rune makes suffix starts miss at the root, avoiding O(n^2) fixtures.
  const size_t sizes[] = {513, 65535, 65536};
  const char* starts[] = {"甲", "丙", "丁"};
  std::vector<std::string> words;
  std::ostringstream data;
  data << "普通 10 n\n";
  for (size_t i = 0; i < 3; ++i) {
    std::string word(starts[i]);
    for (size_t n = 1; n < sizes[i]; ++n) word += "乙";
    data << word << " 100 n\n";
    words.push_back(word);
  }
  TempFile file(data.str());
  DictTrie dict(file.path);
  MPSegment mp(&dict);
  Require(dict.GetActualMaxWordLen() == 65536, "Actual maximum word length truncated");
  const size_t limits[] = {0, 1, 2, 512, 513, 65535, 65536, std::numeric_limits<size_t>::max()};
  for (size_t i = 0; i < words.size(); ++i) {
    for (size_t l = 0; l < sizeof(limits) / sizeof(limits[0]); ++l) {
      std::vector<Word> output;
      mp.Cut(words[i], output, limits[l]);
      const size_t expected_count = limits[l] >= sizes[i] ? 1 : sizes[i];
      Require(output.size() == expected_count, "Long word boundary/count mismatch");
      size_t bytes = 0, runes = 0;
      for (size_t n = 0; n < output.size(); ++n) {
        Require(output[n].offset == bytes && output[n].unicode_offset == runes,
                "Long word coordinates changed");
        Require(output[n].word == words[i].substr(bytes, output[n].word.size()),
                "Long word output does not recover the input");
        bytes += output[n].word.size(); runes += output[n].unicode_length;
      }
      Require(bytes == words[i].size() && runes == sizes[i], "Long output does not cover input");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string repo = argc > 1 ? argv[1] : ".";
    const size_t random_count = argc > 2 ? std::strtoul(argv[2], NULL, 10) : 300;
    CheckFixtures();
    CheckLongWords();
    CpuCutOptions defaults;
    std::unique_ptr<Jieba> reference = MakeJieba(repo, defaults);
    std::vector<std::unique_ptr<Jieba> > variants;
    for (int i = 0; i < 2; ++i) {
      CpuCutOptions options;
      options.mode = CpuCutMode::DatRawFused;
      options.optimize_hmm = true;
      if (i == 1) options.hmm_dense_budget_bytes = 1;
      variants.push_back(MakeJieba(repo, options));
      Require(variants.back()->GetDictTrie()->GetCpuCutMode() == options.mode,
              "Default dictionary unexpectedly fell back");
    }
    std::vector<std::string> corpus = {"", "小明在南京市长江大桥", "南京市长江大桥研究生命起源",
      "甲", "龘", "𠀀𠀁😀中国𠀀", "abc123.45% 中国C++ test@example.com\t\r\n",
      "北京大学生物系主任在研究生命起源", "据新华社消息，人工智能和自然语言处理技术正在持续发展。",
      std::string("a\0中国", 8), "中国中国中国中国中国中国中国中国中国中国"};
    std::ifstream real_corpus((repo + "/test/testdata/review.100").c_str());
    Require(static_cast<bool>(real_corpus), "Cannot open repository review corpus");
    std::string review;
    size_t review_count = 0;
    while (review_count < 100 && std::getline(real_corpus, review)) {
      if (!review.empty()) { corpus.push_back(review); ++review_count; }
    }
    Require(review_count > 0, "Repository review corpus is empty");
    std::mt19937 random(0xc0ffee);
    const std::string atoms[] = {"南", "京", "市", "长", "江", "大", "桥", "研究", "生命", "起源",
      "中国", "自然语言", "处理", "甲", "龘", "𠀀", "😀", "abc", "3.14", " ", ",", "。", "\n"};
    for (size_t i = 0; i < random_count; ++i) {
      std::string text;
      const size_t n = random() % 90;
      for (size_t j = 0; j < n; ++j) text += atoms[random() % (sizeof(atoms) / sizeof(atoms[0]))];
      corpus.push_back(text);
    }
    for (size_t v = 0; v < variants.size(); ++v) {
      for (size_t i = 0; i < corpus.size(); ++i) CheckAll(*reference, *variants[v], corpus[i]);
      // Each thread writes only its own outputs while sharing the same model and reference.
      std::vector<std::thread> readers;
      std::vector<std::string> errors(4);
      for (size_t thread = 0; thread < errors.size(); ++thread) {
        readers.push_back(std::thread([&, thread]() {
          try {
            for (size_t i = thread; i < corpus.size(); i += errors.size())
              CheckAll(*reference, *variants[v], corpus[i]);
          } catch (const std::exception& e) { errors[thread] = e.what(); }
        }));
      }
      for (size_t t = 0; t < readers.size(); ++t) readers[t].join();
      for (size_t t = 0; t < errors.size(); ++t) Require(errors[t].empty(), "Concurrent read: " + errors[t]);
    }
    // Invalid trailing UTF-8 must keep the existing decode-failure behavior too.
    const std::string invalid[] = {std::string("\xff", 1), std::string("中\xe4\xb8", 5)};
    for (size_t v = 0; v < variants.size(); ++v)
      for (size_t i = 0; i < 2; ++i) CheckAll(*reference, *variants[v], invalid[i]);
    std::cout << "cpu differential passed: " << corpus.size()
              << " texts, DAT-only HMM variants, all APIs, 4 shared readers, long words and construction failures\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "cpu differential FAILED: " << e.what() << '\n';
    return 1;
  }
}
