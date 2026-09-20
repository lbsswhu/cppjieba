#ifndef CPPJIEBA_DICT_TRIE_HPP
#define CPPJIEBA_DICT_TRIE_HPP

#include <algorithm>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "Utils.hpp"
#include "UnicodeFile.hpp"
#include "Unicode.hpp"
#include "DictTypes.hpp"
#include "CpuCutOptions.hpp"

namespace cppjieba {
const size_t DICT_COLUMN_NUM = 3;
const char* const UNKNOWN_TAG = "";

class DictTrie {
 public:
  enum UserWordWeightOption {
    WordWeightMin,
    WordWeightMedian,
    WordWeightMax,
  }; // enum UserWordWeightOption

  DictTrie(const std::string& dict_path, const std::string& user_dict_paths = "", UserWordWeightOption user_word_weight_opt = WordWeightMedian,
           const CpuCutOptions& options = CpuCutOptions())
      : options_(options), initialized_(false), actual_max_word_len_(0),
        load_ms_(0.0) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Init(dict_path, user_dict_paths, user_word_weight_opt);
    load_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() - dat_stats_.build_ms;
    initialized_ = true;
  }

  // Runtime dictionary edits are unsupported. Load all user words at startup.
  bool InsertUserWord(const std::string&, const std::string& = UNKNOWN_TAG) {
    return false;
  }
  bool InsertUserWord(const std::string&, int, const std::string& = UNKNOWN_TAG) {
    return false;
  }
  bool DeleteUserWord(const std::string&, const std::string& = UNKNOWN_TAG) {
    return false;
  }

  const DictUnit* Find(RuneStrArray::const_iterator begin,
                       RuneStrArray::const_iterator end) const {
    if (begin == end) return NULL;
    DatCursor cursor = dat_model_->Root();
    for (; begin != end; ++begin) {
      if (!dat_model_->StepRaw(begin->rune, cursor)) return NULL;
    }
    return dat_model_->IsTerminal(cursor) ? TerminalValue(cursor) : NULL;
  }

  bool Contains(RuneStrArray::const_iterator begin,
                RuneStrArray::const_iterator end) const {
    if (begin == end) return false;
    DatCursor cursor = dat_model_->Root();
    for (; begin != end; ++begin) {
      if (!dat_model_->StepRaw(begin->rune, cursor)) return false;
    }
    return dat_model_->IsTerminal(cursor);
  }

  bool Contains(const std::string& word) const {
    RuneStrArray runes;
    if (!DecodeUTF8RunesInString(word, runes)) {
      XLOG(ERROR) << "Decode failed.";
      return false;
    }
    return Contains(runes.begin(), runes.end());
  }
  bool Find(const std::string& word) const { return Contains(word); }

  // Compatibility enumeration for FullSegment and external DAG readers.
  // Always produce the length-one fallback, including requested max length 0.
  void Find(RuneStrArray::const_iterator begin,
            RuneStrArray::const_iterator end, std::vector<Dag>& res,
            size_t max_word_len = MAX_WORD_LENGTH) const {
    assert(end >= begin);
    const size_t n = static_cast<size_t>(end - begin);
    res.clear();
    res.resize(n);
    const size_t limit = std::max(size_t(1),
        std::min(actual_max_word_len_, max_word_len));
    for (size_t i = 0; i < n; ++i) {
      res[i].runestr = begin[i];
      DatCursor cursor = dat_model_->Root();
      const size_t stop = std::min(limit, n - i);
      for (size_t len = 1; len <= stop; ++len) {
        const bool alive = dat_model_->StepRaw(begin[i + len - 1].rune, cursor);
        const bool terminal = alive && dat_model_->IsTerminal(cursor);
        if (len == 1 || terminal) {
          res[i].nexts.push_back(std::make_pair(i + len - 1,
              terminal ? TerminalValue(cursor) : static_cast<const DictUnit*>(NULL)));
        }
        if (!alive) break;
      }
    }
  }

  bool IsUserDictSingleChineseWord(const Rune& word) const {
    return IsIn(user_dict_single_chinese_word_, word);
  }

  double GetMinWeight() const {
    return min_weight_;
  }

  void InserUserDictNode(const std::string& line) {
    CheckMutable();
    std::vector<std::string> buf;
    DictUnit node_info;
    Split(line, buf, " ");
    if(buf.size() == 1){
          MakeNodeInfo(node_info,
                buf[0],
                user_word_default_weight_,
                UNKNOWN_TAG);
        } else if (buf.size() == 2) {
          MakeNodeInfo(node_info,
                buf[0],
                user_word_default_weight_,
                buf[1]);
        } else if (buf.size() == 3) {
          int freq = atoi(buf[1].c_str());
          assert(freq_sum_ > 0.0);
          double weight = log(1.0 * freq / freq_sum_);
          MakeNodeInfo(node_info, buf[0], weight, buf[2]);
        }
        static_node_infos_.push_back(node_info);
        if (node_info.word.size() == 1) {
          user_dict_single_chinese_word_.insert(node_info.word[0]);
        }
  }

  void LoadUserDict(const std::vector<std::string>& buf) {
    CheckMutable();
    for (size_t i = 0; i < buf.size(); i++) {
      InserUserDictNode(buf[i]);
    }
  }

   void LoadUserDict(const std::set<std::string>& buf) {
    CheckMutable();
    std::set<std::string>::const_iterator iter;
    for (iter = buf.begin(); iter != buf.end(); iter++){
      InserUserDictNode(*iter);
    }
  }

  void LoadUserDict(const std::string& filePaths) {
    CheckMutable();
    std::vector<std::string> files = Split(filePaths, "|;");
    for (size_t i = 0; i < files.size(); i++) {
      std::ifstream ifs;
      OpenInputFile(ifs, files[i]);
      XCHECK(ifs.is_open()) << "open " << files[i] << " failed";
      std::string line;

      while(getline(ifs, line)) {
        if (line.size() == 0) {
          continue;
        }
        InserUserDictNode(line);
      }
    }
  }


  bool IsDictionaryFrozen() const {
    return initialized_;
  }
  CpuCutMode GetRequestedCpuCutMode() const { return options_.mode; }
  CpuCutMode GetCpuCutMode() const { return CpuCutMode::DatRawFused; }
  const DatModel* GetDatModel() const { return dat_model_.get(); }
  const DatBuildStats& GetDatBuildStats() const { return dat_stats_; }
  size_t GetActualMaxWordLen() const { return actual_max_word_len_; }
  double GetLoadMilliseconds() const { return load_ms_; }

 private:
  const DictUnit* TerminalValue(const DatCursor& cursor) const {
    const size_t index = dat_model_->TerminalSourceIndex(cursor);
    const size_t base_size = base_static_node_infos_->size();
    if (index < base_size) return &(*base_static_node_infos_)[index];
    assert(index - base_size < static_node_infos_.size());
    return &static_node_infos_[index - base_size];
  }

  void CheckMutable() const {
    if (IsDictionaryFrozen()) throw std::logic_error("cppjieba dictionary is frozen");
  }
  struct DictCacheEntry {
    std::shared_ptr<const std::vector<DictUnit> > node_infos;
    double freq_sum;
    double min_weight;
    double max_weight;
    double median_weight;
  };

  void Init(const std::string& dict_path, const std::string& user_dict_paths, UserWordWeightOption user_word_weight_opt) {
    const DictCacheEntry& cache = GetDictCache(dict_path);
    base_static_node_infos_ = cache.node_infos;
    freq_sum_ = cache.freq_sum;
    min_weight_ = cache.min_weight;
    max_weight_ = cache.max_weight;
    median_weight_ = cache.median_weight;
    switch (user_word_weight_opt) {
      case WordWeightMin:
        user_word_default_weight_ = min_weight_;
        break;
      case WordWeightMedian:
        user_word_default_weight_ = median_weight_;
        break;
      default:
        user_word_default_weight_ = max_weight_;
        break;
    }

    if (user_dict_paths.size()) {
      LoadUserDict(user_dict_paths);
    }
    Shrink(static_node_infos_);
    CreateDat();
  }

  void CreateDat() {
    const size_t total_size = base_static_node_infos_->size() + static_node_infos_.size();
    assert(total_size);
    std::vector<const DictUnit*> values;
    values.reserve(total_size);
    for (size_t i = 0; i < base_static_node_infos_->size(); ++i)
      values.push_back(&(*base_static_node_infos_)[i]);
    for (size_t i = 0; i < static_node_infos_.size(); ++i)
      values.push_back(&static_node_infos_[i]);
    DatBuildResult result = DatBuilder::Build(values, min_weight_, options_.dat_build_options);
    if (!result.model) throw DatBuildError(result.stats);
    actual_max_word_len_ = result.model->ActualMaxWordLen();
    dat_model_ = std::move(result.model);
    dat_stats_ = std::move(result.stats);
  }

  bool MakeNodeInfo(DictUnit& node_info,
        const std::string& word,
        double weight,
        const std::string& tag) {
    if (!DecodeUTF8RunesInString(word, node_info.word)) {
      XLOG(ERROR) << "UTF-8 decode failed for dict word: " << word;
      return false;
    }
    node_info.weight = weight;
    node_info.tag = tag;
    return true;
  }

  static DictCacheEntry BuildDictCacheEntry(const std::string& filePath) {
    DictCacheEntry entry;
    std::vector<DictUnit> node_infos;
    std::ifstream ifs;
    OpenInputFile(ifs, filePath);
    XCHECK(ifs.is_open()) << "open " << filePath << " failed.";
    std::string line;
    std::vector<std::string> buf;
    while (getline(ifs, line)) {
      Split(line, buf, " ");
      XCHECK(buf.size() == DICT_COLUMN_NUM) << "split result illegal, line:" << line;
      DictUnit node_info;
      XCHECK(DecodeUTF8RunesInString(buf[0], node_info.word)) << "UTF-8 decode failed for dict word: " << buf[0];
      node_info.weight = atof(buf[1].c_str());
      node_info.tag = buf[2];
      node_infos.push_back(node_info);
    }
    XCHECK(!node_infos.empty()) << "dict file is empty: " << filePath;

    entry.freq_sum = CalcFreqSum(node_infos);
    CalculateWeight(node_infos, entry.freq_sum);
    std::vector<DictUnit> sorted = node_infos;
    std::sort(sorted.begin(), sorted.end(), WeightCompare);
    entry.min_weight = sorted.front().weight;
    entry.max_weight = sorted.back().weight;
    entry.median_weight = sorted[sorted.size() / 2].weight;

    entry.node_infos = std::shared_ptr<const std::vector<DictUnit> >(new std::vector<DictUnit>(node_infos));
    return entry;
  }

  static const DictCacheEntry& GetDictCache(const std::string& filePath) {
    static std::unordered_map<std::string, DictCacheEntry> cache;
    static std::mutex cache_mutex;
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::unordered_map<std::string, DictCacheEntry>::const_iterator it = cache.find(filePath);
    if (it != cache.end()) {
      return it->second;
    }
    DictCacheEntry entry = BuildDictCacheEntry(filePath);
    std::pair<std::unordered_map<std::string, DictCacheEntry>::iterator, bool> result =
      cache.insert(std::make_pair(filePath, entry));
    return result.first->second;
  }

  static bool WeightCompare(const DictUnit& lhs, const DictUnit& rhs) {
    return lhs.weight < rhs.weight;
  }

  static double CalcFreqSum(const std::vector<DictUnit>& node_infos) {
    double sum = 0.0;
    for (size_t i = 0; i < node_infos.size(); i++) {
      sum += node_infos[i].weight;
    }
    return sum;
  }

  static void CalculateWeight(std::vector<DictUnit>& node_infos, double sum) {
    assert(sum > 0.0);
    for (size_t i = 0; i < node_infos.size(); i++) {
      DictUnit& node_info = node_infos[i];
      assert(node_info.weight > 0.0);
      node_info.weight = log(double(node_info.weight)/sum);
    }
  }

  void Shrink(std::vector<DictUnit>& units) const {
    std::vector<DictUnit>(units.begin(), units.end()).swap(units);
  }

  std::shared_ptr<const std::vector<DictUnit> > base_static_node_infos_;
  std::vector<DictUnit> static_node_infos_;
  CpuCutOptions options_;
  bool initialized_;
  size_t actual_max_word_len_;
  double load_ms_;
  std::unique_ptr<const DatModel> dat_model_;
  DatBuildStats dat_stats_;

  double freq_sum_;
  double min_weight_;
  double max_weight_;
  double median_weight_;
  double user_word_default_weight_;
  std::unordered_set<Rune> user_dict_single_chinese_word_;
};
}

#endif
