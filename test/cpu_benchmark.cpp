// DAT-only C/D benchmark. Compile this same source with CPPJIEBA_CPU_PREVIOUS
// against 8b8a3f7 for A/B/C/D, or CPPJIEBA_CPU_BASELINE against original headers.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include "cppjieba/Jieba.hpp"

// Allocation tracking is absent from the performance binary. A separate
// diagnostic build records requested bytes and simultaneous live allocations.
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
namespace allocation_probe {
struct Counters {
  bool enabled;
  size_t generation, calls, bytes, live, peak;
  Counters() : enabled(false), generation(0), calls(0), bytes(0), live(0), peak(0) {}
};
thread_local Counters counters;
union Header {
  std::max_align_t alignment;
  struct { size_t bytes, generation; Counters* owner; } data;
};
void* Allocate(size_t bytes) {
  if (bytes > static_cast<size_t>(-1) - sizeof(Header)) throw std::bad_alloc();
  Header* p = static_cast<Header*>(std::malloc(sizeof(Header) + (bytes ? bytes : 1)));
  if (!p) throw std::bad_alloc();
  p->data.bytes = bytes;
  p->data.owner = counters.enabled ? &counters : NULL;
  p->data.generation = counters.generation;
  if (counters.enabled) {
    ++counters.calls; counters.bytes += bytes; counters.live += bytes;
    counters.peak = std::max(counters.peak, counters.live);
  }
  return p + 1;
}
void Free(void* value) noexcept {
  if (!value) return;
  Header* p = static_cast<Header*>(value) - 1;
  if (p->data.owner == &counters && p->data.generation == counters.generation)
    counters.live -= p->data.bytes;
  std::free(p);
}
void Begin() {
  ++counters.generation;
  counters.calls = counters.bytes = counters.live = counters.peak = 0;
  counters.enabled = true;
}
}  // namespace allocation_probe
void* operator new(size_t n) { return allocation_probe::Allocate(n); }
void* operator new[](size_t n) { return allocation_probe::Allocate(n); }
void operator delete(void* p) noexcept { allocation_probe::Free(p); }
void operator delete[](void* p) noexcept { allocation_probe::Free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, size_t) noexcept { allocation_probe::Free(p); }
void operator delete[](void* p, size_t) noexcept { allocation_probe::Free(p); }
#endif
#endif

namespace {
using namespace cppjieba;
typedef std::chrono::steady_clock Clock;
double Millis(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}
std::string Quote(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
    else out << c;
  }
  out << '"';
  return out.str();
}
long long CurrentRSS() {
#ifdef __linux__
  std::ifstream in("/proc/self/statm");
  long long total = 0, rss = 0;
  return in >> total >> rss ? rss * sysconf(_SC_PAGESIZE) : -1;
#elif defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
      ? static_cast<long long>(info.resident_size) : -1;
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS info;
  return GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)) ? static_cast<long long>(info.WorkingSetSize) : -1;
#else
  return -1;
#endif
}
long long PeakRSS() {
#if defined(__linux__) || defined(__APPLE__)
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage)) return -1;
#ifdef __APPLE__
  return usage.ru_maxrss;
#else
  return usage.ru_maxrss * 1024LL;
#endif
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS info;
  return GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)) ? static_cast<long long>(info.PeakWorkingSetSize) : -1;
#else
  return -1;
#endif
}
std::string Affinity() {
  std::ostringstream out;
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    bool first = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &set)) {
      if (!first) out << ',';
      out << cpu; first = false;
    }
    return out.str();
  }
#endif
  return "unavailable";
}
std::string Compiler() {
#ifdef _MSC_VER
  return std::string("MSVC ") + std::to_string(_MSC_FULL_VER);
#elif defined(__VERSION__)
  return __VERSION__;
#else
  return "unknown";
#endif
}

struct Config {
  std::string repo, mode, corpus, api;
  size_t threads, iterations;
  size_t dat_max_slots, dat_max_temporary_bytes, hmm_dense_budget_bytes;
  double dat_max_build_ms;
  Config() : repo("."),
#if defined(CPPJIEBA_CPU_BASELINE) || defined(CPPJIEBA_CPU_PREVIOUS)
      mode("A"),
#else
      mode("C"),
#endif
      corpus("all"), api("all"), threads(1), iterations(200),
      dat_max_slots(std::numeric_limits<size_t>::max()),
      dat_max_temporary_bytes(std::numeric_limits<size_t>::max()),
      hmm_dense_budget_bytes(std::numeric_limits<size_t>::max()), dat_max_build_ms(-1) {}
};
Config Parse(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (i == 1 && arg.compare(0, 2, "--") != 0) c.repo = arg;
    else if (i + 1 < argc) {
      const std::string value(argv[++i]);
      if (arg == "--mode") c.mode = value;
      else if (arg == "--threads") c.threads = std::strtoul(value.c_str(), NULL, 10);
      else if (arg == "--iterations") c.iterations = std::strtoul(value.c_str(), NULL, 10);
      else if (arg == "--corpus") c.corpus = value;
      else if (arg == "--api") c.api = value;
      else if (arg == "--dat-max-slots") c.dat_max_slots = std::stoull(value);
      else if (arg == "--dat-max-temporary-bytes") c.dat_max_temporary_bytes = std::stoull(value);
      else if (arg == "--dat-max-build-ms") c.dat_max_build_ms = std::stod(value);
      else if (arg == "--hmm-dense-budget-bytes") c.hmm_dense_budget_bytes = std::stoull(value);
      else throw std::runtime_error("Unknown argument: " + arg);
    } else throw std::runtime_error("Missing argument value: " + arg);
  }
  if (!c.threads || !c.iterations || c.mode.size() != 1 || c.mode[0] < 'A' || c.mode[0] > 'D')
    throw std::runtime_error("Use --mode A/B/C/D, positive --threads and --iterations");
#ifdef CPPJIEBA_CPU_BASELINE
  if (c.mode != "A") throw std::runtime_error("Original baseline supports only mode A");
  if (c.dat_max_slots != std::numeric_limits<size_t>::max() ||
      c.dat_max_temporary_bytes != std::numeric_limits<size_t>::max() ||
      c.hmm_dense_budget_bytes != std::numeric_limits<size_t>::max() || c.dat_max_build_ms >= 0)
    throw std::runtime_error("Original baseline does not support optimization budgets");
#endif
#if !defined(CPPJIEBA_CPU_BASELINE) && !defined(CPPJIEBA_CPU_PREVIOUS)
  if (c.mode != "C" && c.mode != "D")
    throw std::runtime_error("DAT-only build supports modes C and D; use a previous-build binary for A/B");
#endif
  return c;
}

struct Input {
  std::string text;
  RuneStrArray runes;
  explicit Input(const std::string& s) : text(s) {
    if (!DecodeUTF8RunesInString(s, runes)) throw std::runtime_error("Invalid benchmark UTF-8");
  }
};
struct Corpus {
  std::string name;
  std::vector<Input> inputs;
};
std::vector<Corpus> Corpora(const std::string& repo) {
  std::vector<Corpus> result(5);
  result[0].name = "short";
  const char* short_texts[] = {"小明在南京市长江大桥", "北京大学生物系主任", "自然语言处理和搜索引擎",
    "今天下午三点开会", "研究生命起源", "OpenAI与中文123.45测试", "龘靐齉𠀀未知词", "中华人民共和国万岁"};
  for (size_t i = 0; i < sizeof(short_texts) / sizeof(short_texts[0]); ++i)
    result[0].inputs.push_back(Input(short_texts[i]));
  result[1].name = "document";
  std::ifstream in((repo + "/test/testdata/synthetic_doc.utf8").c_str(), std::ios::binary);
  if (!in) throw std::runtime_error("Cannot open synthetic_doc.utf8");
  const std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  RuneStrArray decoded;
  if (!DecodeUTF8RunesInString(doc, decoded) || decoded.empty()) throw std::runtime_error("Invalid document corpus");
  const size_t count = std::min(size_t(2048), decoded.size());
  result[1].inputs.push_back(Input(doc.substr(0, decoded[count - 1].offset + decoded[count - 1].len)));
  result[2].name = "long";
  std::string long_range;
  for (size_t i = 0; i < 512; ++i) long_range += "南京市长江大桥研究生命起源自然语言处理";
  result[2].inputs.push_back(Input(long_range));
  result[3].name = "full_document";
  result[3].inputs.push_back(Input(doc));
  result[4].name = "lookup";
  std::ifstream dictionary((repo + "/dict/jieba.dict.utf8").c_str());
  std::string line;
  size_t line_number = 0;
  while (result[4].inputs.size() < 128 && std::getline(dictionary, line)) {
    if (line_number++ % 997 != 0) continue;
    const std::string word = line.substr(0, line.find(' '));
    result[4].inputs.push_back(Input(word));
    if (line_number % 4 == 1) result[4].inputs.push_back(Input(word + "𠀀"));
  }
  if (result[4].inputs.empty()) throw std::runtime_error("Empty lookup corpus");
  return result;
}

struct Counts {
  size_t transitions, candidates, fused_ranges, legacy_ranges, wide_ranges;
  Counts() : transitions(0), candidates(0), fused_ranges(0), legacy_ranges(0), wide_ranges(0) {}
};
size_t Request(const Jieba& jieba, const MPSegment& mp, const std::string& api,
               const Input& input, Counts* counts) {
  if (api == "mp") {
    std::vector<WordRange> output;
#if defined(CPPJIEBA_CPU_DIAGNOSTICS) && !defined(CPPJIEBA_CPU_BASELINE)
    MPCutScratch scratch;
    mp.CutWithScratch(input.runes.begin(), input.runes.end(), output, scratch);
    if (counts) {
      counts->transitions += scratch.transitions; counts->candidates += scratch.candidates;
      counts->fused_ranges += scratch.fused_ranges;
#ifdef CPPJIEBA_CPU_PREVIOUS
      counts->legacy_ranges += scratch.legacy_ranges;
#else
      counts->wide_ranges += scratch.wide_ranges;
#endif
    }
#else
    (void)counts;
    mp.Cut(input.runes.begin(), input.runes.end(), output);
#endif
    return output.size();
  }
  if (api == "find")
    return jieba.GetDictTrie()->Find(input.runes.begin(), input.runes.end()) != NULL;
  if (api == "keywords") {
    std::vector<KeywordExtractor::Word> output;
    jieba.extractor.Extract(input.text, output, 20);
    return output.size();
  }
  std::vector<Word> output;
  if (api == "cut") jieba.Cut(input.text, output);
  else if (api == "search") jieba.CutForSearch(input.text, output);
  else if (api == "small") jieba.CutSmall(input.text, output, 3);
  else if (api == "hmm") jieba.CutHMM(input.text, output);
  else if (api == "full") jieba.CutAll(input.text, output);
  else throw std::runtime_error("Unknown API: " + api);
  return output.size();
}

double Percentile(const std::vector<double>& sorted, double percentile) {
  if (sorted.empty()) return 0;
  return sorted[static_cast<size_t>(std::ceil(percentile * sorted.size())) - 1];
}
struct Worker {
  std::vector<double> latency;
  size_t bytes, runes, tokens, allocations, allocated_bytes, peak_live_bytes;
  Counts counts;
  Worker() : bytes(0), runes(0), tokens(0), allocations(0), allocated_bytes(0), peak_live_bytes(0) {}
};

void Measure(const Config& c, const Jieba& jieba, const Corpus& corpus, const std::string& api) {
  const MPSegment mp(jieba.GetDictTrie());
  const size_t requests = corpus.name == "full_document" ? std::max(size_t(1), c.iterations / 20) :
      c.iterations * ((corpus.name == "short" || corpus.name == "lookup") ? 20 : 1);
  for (size_t i = 0; i < corpus.inputs.size(); ++i) Request(jieba, mp, api, corpus.inputs[i], NULL);
  std::vector<Worker> results(c.threads);
  std::vector<std::thread> threads;
  std::atomic<size_t> ready(0);
  std::atomic<bool> start(false);
  for (size_t thread = 0; thread < c.threads; ++thread) {
    results[thread].latency.reserve((requests + c.threads - 1) / c.threads);
    threads.push_back(std::thread([&, thread]() {
      Worker& result = results[thread];
      ready.fetch_add(1);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t i = thread; i < requests; i += c.threads) {
        const Input& input = corpus.inputs[i % corpus.inputs.size()];
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
        allocation_probe::Begin();
#endif
        const Clock::time_point begin = Clock::now();
        result.tokens += Request(jieba, mp, api, input, &result.counts);
        const Clock::time_point end = Clock::now();
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
        allocation_probe::counters.enabled = false;
        result.allocations += allocation_probe::counters.calls;
        result.allocated_bytes += allocation_probe::counters.bytes;
        result.peak_live_bytes = std::max(result.peak_live_bytes, allocation_probe::counters.peak);
#endif
        result.latency.push_back(Millis(begin, end) * 1000.0);
        result.bytes += input.text.size(); result.runes += input.runes.size();
      }
    }));
  }
  while (ready.load() != c.threads) std::this_thread::yield();
  const Clock::time_point begin = Clock::now();
  start.store(true, std::memory_order_release);
  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
  const double seconds = Millis(begin, Clock::now()) / 1000.0;
  Worker total;
  for (size_t i = 0; i < results.size(); ++i) {
    const Worker& w = results[i];
    total.bytes += w.bytes; total.runes += w.runes; total.tokens += w.tokens;
    total.allocations += w.allocations; total.allocated_bytes += w.allocated_bytes;
    total.peak_live_bytes = std::max(total.peak_live_bytes, w.peak_live_bytes);
    total.counts.transitions += w.counts.transitions; total.counts.candidates += w.counts.candidates;
    total.counts.fused_ranges += w.counts.fused_ranges; total.counts.legacy_ranges += w.counts.legacy_ranges;
    total.counts.wide_ranges += w.counts.wide_ranges;
    total.latency.insert(total.latency.end(), w.latency.begin(), w.latency.end());
  }
  std::sort(total.latency.begin(), total.latency.end());
  std::cout << "{\"kind\":\"warm\",\"mode\":" << Quote(c.mode)
            << ",\"corpus\":" << Quote(corpus.name) << ",\"api\":" << Quote(api)
            << ",\"threads\":" << c.threads << ",\"requests\":" << requests
            << ",\"input_bytes\":" << total.bytes << ",\"input_runes\":" << total.runes
            << ",\"output_tokens\":" << total.tokens;
  if (api == "find") std::cout << ",\"query_hits\":" << total.tokens;
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
  std::cout << ",\"diagnostic\":true,\"allocations_per_request\":" << double(total.allocations) / requests
            << ",\"allocated_bytes_per_request\":" << double(total.allocated_bytes) / requests
            << ",\"max_request_live_allocation_bytes\":" << total.peak_live_bytes;
  if (api == "mp")
    std::cout << ",\"transitions\":" << total.counts.transitions << ",\"candidates\":" << total.counts.candidates
              << ",\"fused_ranges\":" << total.counts.fused_ranges << ",\"legacy_ranges\":" << total.counts.legacy_ranges
              << ",\"wide_ranges\":" << total.counts.wide_ranges;
#else
  std::cout << ",\"diagnostic\":false,\"elapsed_seconds\":" << seconds
            << ",\"bytes_per_second\":" << total.bytes / seconds << ",\"runes_per_second\":" << total.runes / seconds
            << ",\"requests_per_second\":" << requests / seconds
            << ",\"latency_p50_us\":" << Percentile(total.latency, .50)
            << ",\"latency_p95_us\":" << Percentile(total.latency, .95)
            << ",\"latency_p99_us\":" << Percentile(total.latency, .99);
#endif
  std::cout << "}\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const Config c = Parse(argc, argv);
    std::cout << std::setprecision(12);
    const std::string dict = c.repo + "/dict/";
    const long long rss_before = CurrentRSS();
    const Clock::time_point begin = Clock::now();
#ifdef CPPJIEBA_CPU_BASELINE
    Jieba jieba(dict + "jieba.dict.utf8", dict + "hmm_model.utf8", dict + "user.dict.utf8",
                dict + "idf.utf8", dict + "stop_words.utf8");
#else
    CpuCutOptions options;
#ifdef CPPJIEBA_CPU_PREVIOUS
    options.mode = c.mode == "A" ? CpuCutMode::LegacyDag :
        c.mode == "B" ? CpuCutMode::PointerFused : CpuCutMode::DatRawFused;
#else
    options.mode = CpuCutMode::DatRawFused;
#endif
    options.optimize_hmm = c.mode == "D";
    if (c.dat_max_slots != std::numeric_limits<size_t>::max())
      options.dat_build_options.max_slots = c.dat_max_slots;
    if (c.dat_max_temporary_bytes != std::numeric_limits<size_t>::max())
      options.dat_build_options.max_temporary_bytes = c.dat_max_temporary_bytes;
    if (c.dat_max_build_ms >= 0) options.dat_build_options.max_build_ms = c.dat_max_build_ms;
    if (c.hmm_dense_budget_bytes != std::numeric_limits<size_t>::max())
      options.hmm_dense_budget_bytes = c.hmm_dense_budget_bytes;
    Jieba jieba(dict + "jieba.dict.utf8", dict + "hmm_model.utf8", dict + "user.dict.utf8",
                dict + "idf.utf8", dict + "stop_words.utf8", options);
#endif
    const double cold_ms = Millis(begin, Clock::now());
    const long long rss_after = CurrentRSS(), peak_after = PeakRSS();
    std::cout << "{\"kind\":\"cold\",\"mode\":" << Quote(c.mode) << ",\"threads\":" << c.threads
              << ",\"compiler\":" << Quote(Compiler()) << ",\"cplusplus\":" << __cplusplus
              << ",\"affinity\":" << Quote(Affinity()) << ",\"cold_ms\":" << cold_ms
              << ",\"rss_before_bytes\":" << rss_before << ",\"model_rss_bytes\":" << rss_after
              << ",\"model_rss_delta_bytes\":" << rss_after - rss_before
              << ",\"initialization_peak_rss_bytes\":" << peak_after;
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
    std::cout << ",\"diagnostic\":true";
#else
    std::cout << ",\"diagnostic\":false";
#endif
#ifdef CPPJIEBA_CPU_BASELINE
    std::cout << ",\"original_baseline\":true,\"actual_backend\":\"LegacyDag\""
              << ",\"hmm_optimization_enabled\":false,\"hmm_dense_emissions\":false,\"hmm_dense_emission_bytes\":0";
#else
    const DictTrie* trie = jieba.GetDictTrie();
    const DatBuildStats& stats = trie->GetDatBuildStats();
#ifdef CPPJIEBA_CPU_PREVIOUS
    const char* backend = trie->GetCpuCutMode() == CpuCutMode::LegacyDag ? "LegacyDag" :
        trie->GetCpuCutMode() == CpuCutMode::PointerFused ? "PointerFused" : "DatRawFused";
#else
    const char* backend = "DatRawFused";
#endif
    std::cout << ",\"original_baseline\":false,\"actual_backend\":" << Quote(backend)
              << ",\"dictionary_load_ms\":" << trie->GetLoadMilliseconds()
              << ",\"dat_build_ms\":" << stats.build_ms << ",\"dat_layout_ms\":" << stats.layout_ms
              << ",\"dat_validation_ms\":" << stats.validation_ms << ",\"dat_slots\":" << stats.slot_count
              << ",\"dat_logical_nodes\":" << stats.logical_nodes << ",\"dat_unique_weights\":" << stats.unique_weights
              << ",\"dat_model_bytes\":" << stats.model_bytes << ",\"dat_status\":" << static_cast<int>(stats.status)
              << ",\"dat_reason\":" << Quote(stats.reason)
              << ",\"dat_max_slots\":" << options.dat_build_options.max_slots
              << ",\"dat_max_temporary_bytes\":" << options.dat_build_options.max_temporary_bytes
              << ",\"dat_max_build_ms\":" << options.dat_build_options.max_build_ms
              << ",\"hmm_dense_budget_bytes\":" << options.hmm_dense_budget_bytes
              << ",\"hmm_optimization_enabled\":" << (jieba.GetHMMModel()->IsOptimizationEnabled() ? "true" : "false")
              << ",\"hmm_dense_emissions\":" << (jieba.GetHMMModel()->HasDenseEmissions() ? "true" : "false")
              << ",\"hmm_dense_emission_bytes\":" << jieba.GetHMMModel()->GetDenseEmissionBytes();
#ifndef CPPJIEBA_CPU_PREVIOUS
    std::cout << ",\"dat_topology_bytes\":" << stats.topology_bytes
              << ",\"dat_source_index_bytes\":" << stats.source_index_bytes
              << ",\"dat_weight_bytes\":" << stats.weight_bytes;
#endif
#endif
#if defined(CPPJIEBA_CPU_BASELINE) || defined(CPPJIEBA_CPU_PREVIOUS)
    std::cout << ",\"pointer_trie_present\":true";
#else
    std::cout << ",\"pointer_trie_present\":false";
#endif
    std::cout << "}\n";
    const std::vector<Corpus> corpora = Corpora(c.repo);
    const char* apis[] = {"mp", "cut", "search", "small", "keywords", "hmm", "full", "find"};
    bool matched = false;
    for (size_t i = 0; i < corpora.size(); ++i) {
      if (c.corpus == "all" && corpora[i].name == "full_document") continue;
      if (c.corpus != "all" && c.corpus != corpora[i].name) continue;
      for (size_t a = 0; a < sizeof(apis) / sizeof(apis[0]); ++a) {
        if (c.api != "all" && c.api != apis[a]) continue;
        if (corpora[i].name == "lookup" && std::string(apis[a]) != "find") continue;
        if (c.corpus == "all" && std::string(apis[a]) == "find" && corpora[i].name != "lookup") continue;
        Measure(c, jieba, corpora[i], apis[a]); matched = true;
      }
    }
    if (!matched) throw std::runtime_error("Unknown corpus or API selection");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "cpu benchmark: " << e.what() << '\n';
    return 1;
  }
}
