#include <fstream>
#include <iostream>
#include <iomanip>
#include <malloc.h>
#include <unistd.h>
#include "cppjieba/Jieba.hpp"
struct Sample { size_t rss, allocated, free_heap; };
Sample sample() {
  size_t size = 0, rss = 0;
  { std::ifstream in("/proc/self/statm"); in >> size >> rss; }
  const struct mallinfo2 m = mallinfo2();
  return Sample{rss * static_cast<size_t>(sysconf(_SC_PAGESIZE)), m.uordblks + m.hblkhd, m.fordblks};
}
int main(int argc, char** argv) {
  cppjieba::CpuCutOptions options;
  if (argc > 1 && argv[1][0] == 'D') {
    options.mode = cppjieba::CpuCutMode::DatRawFused;
    options.optimize_hmm = true;
  }
  const std::string d = "/home/lb/cppjieba/dict/";
  cppjieba::Jieba jieba(d + "jieba.dict.utf8", d + "hmm_model.utf8", d + "user.dict.utf8", d + "idf.utf8", d + "stop_words.utf8", options);
  const Sample before = sample();
  const int trimmed = malloc_trim(0);
  const Sample after = sample();
  std::cout << std::fixed << std::setprecision(3)
            << (argc > 1 ? argv[1] : "A")
            << " rss_before_MiB=" << before.rss / 1048576.0
            << " rss_after_trim_MiB=" << after.rss / 1048576.0
            << " allocated_before_MiB=" << before.allocated / 1048576.0
            << " allocated_after_MiB=" << after.allocated / 1048576.0
            << " free_heap_before_MiB=" << before.free_heap / 1048576.0
            << " trim_return=" << trimmed << '\n';
}
