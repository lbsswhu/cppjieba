#ifndef CPPJIEBA_DICT_TRIE_HPP
#define CPPJIEBA_DICT_TRIE_HPP

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DoubleArrayTrie.hpp"
#include "Trie.hpp"
#include "Unicode.hpp"
#include "UnicodeFile.hpp"
#include "Utils.hpp"

namespace cppjieba {

const size_t DICT_COLUMN_NUM = 3;
const char* const UNKNOWN_TAG = "";

struct DictionaryStats {
  DictionaryStats()
      : slot_count(0),
        occupied_state_count(0),
        terminal_count(0),
        load_factor(0.0),
        array_bytes(0),
        tag_bytes(0) {
  }

  size_t slot_count;
  size_t occupied_state_count;
  size_t terminal_count;
  double load_factor;
  size_t array_bytes;
  size_t tag_bytes;
};

struct ParsedWord {
  Unicode word;
  double weight;
  std::string tag;
  bool from_user;
  size_t source_ordinal;
};

struct DictionaryData {
  DictionaryData()
      : trie(),
        tags(),
        user_single_runes(),
        min_weight(0.0),
        stats() {
  }

  DoubleArrayTrie trie;
  std::vector<std::string> tags;
  std::vector<Rune> user_single_runes;
  double min_weight;
  DictionaryStats stats;
};

class DictTrie {
 public:
  enum UserWordWeightOption {
    WordWeightMin,
    WordWeightMedian,
    WordWeightMax,
  }; // enum UserWordWeightOption

  DictTrie(const std::string& dict_path,
           const std::string& user_dict_paths = "",
           UserWordWeightOption user_word_weight_opt = WordWeightMedian) {
    Init(dict_path, user_dict_paths, user_word_weight_opt);
  }

  bool InsertUserWord(const std::string& word,
                      const std::string& tag = UNKNOWN_TAG) {
    (void)word;
    (void)tag;
    XLOG(ERROR)
        << "static dictionary is immutable; pass user words to the constructor";
    return false;
  }

  bool InsertUserWord(const std::string& word, int freq,
                      const std::string& tag = UNKNOWN_TAG) {
    (void)word;
    (void)freq;
    (void)tag;
    XLOG(ERROR)
        << "static dictionary is immutable; pass user words to the constructor";
    return false;
  }

  bool DeleteUserWord(const std::string& word,
                      const std::string& tag = UNKNOWN_TAG) {
    (void)word;
    (void)tag;
    XLOG(ERROR)
        << "static dictionary is immutable; pass user words to the constructor";
    return false;
  }

  bool Contains(RuneStrArray::const_iterator begin,
                RuneStrArray::const_iterator end) const {
    return data_->trie.ExactMatch(begin, end, NULL, NULL);
  }

  bool Find(const std::string& word) const {
    RuneStrArray runes;
    if (!DecodeUTF8RunesInString(word, runes)) {
      XLOG(ERROR) << "Decode failed.";
      return false;
    }
    return Contains(runes.begin(), runes.end());
  }

  bool FindTag(RuneStrArray::const_iterator begin,
               RuneStrArray::const_iterator end,
               std::string* tag) const {
    uint16_t tag_id = 0;
    if (!data_->trie.ExactMatch(begin, end, NULL, &tag_id)) {
      return false;
    }
    XCHECK(static_cast<size_t>(tag_id) < data_->tags.size())
        << "dictionary tag id is out of range";
    if (tag != NULL) {
      *tag = data_->tags[tag_id];
    }
    return true;
  }

  void BuildDag(RuneStrArray::const_iterator begin,
                RuneStrArray::const_iterator end,
                std::vector<Dag>& res,
                size_t max_word_len = MAX_WORD_LENGTH) const {
    const size_t rune_count = static_cast<size_t>(end - begin);
    res.clear();
    res.resize(rune_count);

    std::vector<DoubleArrayTrie::Match> matches;
    for (size_t i = 0; i < rune_count; ++i) {
      Dag& dag = res[i];
      dag.runestr = *(begin + i);
      dag.edges.push_back(DagEdge(i, data_->min_weight, false));

      data_->trie.CommonPrefixSearch(begin + i, end, max_word_len, &matches);
      for (size_t j = 0; j < matches.size(); ++j) {
        const DagEdge edge(i + matches[j].length - 1,
                           matches[j].weight, true);
        if (matches[j].length == 1) {
          dag.edges[0] = edge;
        } else {
          dag.edges.push_back(edge);
        }
      }
    }
  }

  bool IsUserDictSingleRune(Rune word) const {
    return std::binary_search(data_->user_single_runes.begin(),
                              data_->user_single_runes.end(), word);
  }

  double GetMinWeight() const {
    return data_->min_weight;
  }

  const DictionaryStats& GetStats() const {
    return data_->stats;
  }

  const void* GetDictionaryDataIdentity() const {
    return data_.get();
  }

  void LoadUserDict(const std::vector<std::string>& buf) {
    (void)buf;
    XLOG(ERROR)
        << "static dictionary is immutable; pass user words to the constructor";
  }

  void LoadUserDict(const std::set<std::string>& buf) {
    (void)buf;
    XLOG(ERROR)
        << "static dictionary is immutable; pass user words to the constructor";
  }

  void LoadUserDict(const std::string& file_paths) {
    (void)file_paths;
    XLOG(ERROR)
        << "static dictionary is immutable; pass user words to the constructor";
  }

 private:
  struct CacheKey {
    std::string main_path;
    std::vector<std::string> user_paths;
    UserWordWeightOption weight_option;

    bool operator==(const CacheKey& rhs) const {
      return main_path == rhs.main_path &&
             user_paths == rhs.user_paths &&
             weight_option == rhs.weight_option;
    }
  };

  struct CacheKeyHash {
    size_t operator()(const CacheKey& key) const {
      size_t result = std::hash<std::string>()(key.main_path);
      for (size_t i = 0; i < key.user_paths.size(); ++i) {
        HashCombine(&result, std::hash<std::string>()(key.user_paths[i]));
      }
      HashCombine(&result, std::hash<int>()(
          static_cast<int>(key.weight_option)));
      return result;
    }

    static void HashCombine(size_t* seed, size_t value) {
      *seed ^= value + static_cast<size_t>(0x9e3779b9U) +
               (*seed << 6) + (*seed >> 2);
    }
  };

  static std::vector<std::string> SplitUserDictionaryPaths(
      const std::string& paths) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin < paths.size()) {
      const size_t separator = paths.find_first_of("|;", begin);
      const size_t length = separator == std::string::npos
          ? paths.size() - begin
          : separator - begin;
      if (length != 0) {
        result.push_back(paths.substr(begin, length));
      }
      if (separator == std::string::npos) {
        break;
      }
      begin = separator + 1;
    }
    return result;
  }

  static std::vector<std::string> TokenizeDictionaryRow(
      const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) {
      fields.push_back(field);
    }
    return fields;
  }

  static bool IsContinuationByte(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
  }

  static bool DecodeDictionaryWord(const std::string& encoded,
                                   Unicode* word) {
    word->clear();
    if (encoded.empty()) {
      return false;
    }

    size_t offset = 0;
    while (offset < encoded.size()) {
      const unsigned char first =
          static_cast<unsigned char>(encoded[offset]);
      Rune rune = 0;
      size_t length = 0;
      if (first <= 0x7fU) {
        rune = first;
        length = 1;
      } else if (first >= 0xc2U && first <= 0xdfU) {
        length = 2;
        if (offset + length > encoded.size()) {
          return false;
        }
        const unsigned char second =
            static_cast<unsigned char>(encoded[offset + 1]);
        if (!IsContinuationByte(second)) {
          return false;
        }
        rune = (static_cast<Rune>(first & 0x1fU) << 6) |
               static_cast<Rune>(second & 0x3fU);
      } else if (first >= 0xe0U && first <= 0xefU) {
        length = 3;
        if (offset + length > encoded.size()) {
          return false;
        }
        const unsigned char second =
            static_cast<unsigned char>(encoded[offset + 1]);
        const unsigned char third =
            static_cast<unsigned char>(encoded[offset + 2]);
        if (!IsContinuationByte(second) || !IsContinuationByte(third) ||
            (first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xa0U)) {
          return false;
        }
        rune = (static_cast<Rune>(first & 0x0fU) << 12) |
               (static_cast<Rune>(second & 0x3fU) << 6) |
               static_cast<Rune>(third & 0x3fU);
      } else if (first >= 0xf0U && first <= 0xf4U) {
        length = 4;
        if (offset + length > encoded.size()) {
          return false;
        }
        const unsigned char second =
            static_cast<unsigned char>(encoded[offset + 1]);
        const unsigned char third =
            static_cast<unsigned char>(encoded[offset + 2]);
        const unsigned char fourth =
            static_cast<unsigned char>(encoded[offset + 3]);
        if (!IsContinuationByte(second) || !IsContinuationByte(third) ||
            !IsContinuationByte(fourth) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second >= 0x90U)) {
          return false;
        }
        rune = (static_cast<Rune>(first & 0x07U) << 18) |
               (static_cast<Rune>(second & 0x3fU) << 12) |
               (static_cast<Rune>(third & 0x3fU) << 6) |
               static_cast<Rune>(fourth & 0x3fU);
      } else {
        return false;
      }
      word->push_back(rune);
      offset += length;
    }
    return !word->empty();
  }

  static double ParsePositiveFrequency(const std::string& text,
                                       const std::string& path,
                                       size_t line_number) {
    char* parsed_end = NULL;
    errno = 0;
    const double frequency = std::strtod(text.c_str(), &parsed_end);
    XCHECK(errno == 0 && !text.empty() &&
           parsed_end == text.c_str() + text.size() &&
           std::isfinite(frequency) && frequency > 0.0)
        << "frequency must be finite and greater than zero at "
        << path << ':' << line_number;
    return frequency;
  }

  static void ParseMainDictionary(const std::string& path,
                                  std::vector<ParsedWord>* words,
                                  std::vector<double>* main_weights,
                                  double* freq_sum,
                                  size_t* source_ordinal) {
    std::ifstream ifs;
    OpenInputFile(ifs, path);
    XCHECK(ifs.is_open()) << "open " << path << " failed.";

    std::string line;
    size_t line_number = 0;
    while (std::getline(ifs, line)) {
      ++line_number;
      const std::vector<std::string> fields = TokenizeDictionaryRow(line);
      XCHECK(fields.size() == DICT_COLUMN_NUM)
          << "main dictionary row must contain exactly word frequency tag at "
          << path << ':' << line_number;

      ParsedWord parsed;
      XCHECK(DecodeDictionaryWord(fields[0], &parsed.word))
          << "dictionary word must be nonempty valid UTF-8 at "
          << path << ':' << line_number;
      parsed.weight = ParsePositiveFrequency(fields[1], path, line_number);
      parsed.tag = fields[2];
      parsed.from_user = false;
      parsed.source_ordinal = (*source_ordinal)++;

      const double next_sum = *freq_sum + parsed.weight;
      XCHECK(std::isfinite(next_sum) && next_sum > 0.0)
          << "aggregate frequency must be finite and greater than zero at "
          << path << ':' << line_number;
      *freq_sum = next_sum;
      words->push_back(parsed);
    }

    XCHECK(!words->empty()) << "effective dictionary is empty";
    XCHECK(std::isfinite(*freq_sum) && *freq_sum > 0.0)
        << "aggregate frequency must be finite and greater than zero";

    const double log_sum = std::log(*freq_sum);
    for (size_t i = 0; i < words->size(); ++i) {
      (*words)[i].weight = std::log((*words)[i].weight) - log_sum;
      XCHECK(std::isfinite((*words)[i].weight))
          << "dictionary weight must be finite at " << path;
      main_weights->push_back((*words)[i].weight);
    }
  }

  static void ParseUserDictionaries(
      const std::vector<std::string>& paths,
      double freq_sum,
      double default_weight,
      std::vector<ParsedWord>* words,
      size_t* source_ordinal) {
    const double log_sum = std::log(freq_sum);
    for (size_t path_index = 0; path_index < paths.size(); ++path_index) {
      std::ifstream ifs;
      OpenInputFile(ifs, paths[path_index]);
      XCHECK(ifs.is_open())
          << "open " << paths[path_index] << " failed";

      std::string line;
      size_t line_number = 0;
      while (std::getline(ifs, line)) {
        ++line_number;
        const std::vector<std::string> fields = TokenizeDictionaryRow(line);
        XCHECK(fields.size() >= 1 && fields.size() <= 3)
            << "user dictionary row must contain word, word tag, or word "
               "frequency tag at "
            << paths[path_index] << ':' << line_number;

        ParsedWord parsed;
        XCHECK(DecodeDictionaryWord(fields[0], &parsed.word))
            << "dictionary word must be nonempty valid UTF-8 at "
            << paths[path_index] << ':' << line_number;
        parsed.weight = default_weight;
        parsed.tag = UNKNOWN_TAG;
        if (fields.size() == 2) {
          parsed.tag = fields[1];
        } else if (fields.size() == 3) {
          const double frequency = ParsePositiveFrequency(
              fields[1], paths[path_index], line_number);
          parsed.weight = std::log(frequency) - log_sum;
          XCHECK(std::isfinite(parsed.weight))
              << "dictionary weight must be finite at "
              << paths[path_index] << ':' << line_number;
          parsed.tag = fields[2];
        }
        parsed.from_user = true;
        parsed.source_ordinal = (*source_ordinal)++;
        words->push_back(parsed);
      }
    }
  }

  static bool ParsedWordLess(const ParsedWord& lhs,
                             const ParsedWord& rhs) {
    return RuneLexicographicLess(lhs.word, rhs.word);
  }

  static bool SameWord(const Unicode& lhs, const Unicode& rhs) {
    return !RuneLexicographicLess(lhs, rhs) &&
           !RuneLexicographicLess(rhs, lhs);
  }

  static uint16_t InternTag(
      const std::string& tag,
      std::unordered_map<std::string, uint16_t>* tag_ids,
      std::vector<std::string>* tags) {
    const std::unordered_map<std::string, uint16_t>::const_iterator found =
        tag_ids->find(tag);
    if (found != tag_ids->end()) {
      return found->second;
    }

    XCHECK(tag.empty() ||
           tags->size() <= std::numeric_limits<uint16_t>::max())
        << "more than 65535 non-empty tags";
    const uint16_t tag_id = static_cast<uint16_t>(tags->size());
    tags->push_back(tag);
    tag_ids->insert(std::make_pair(tag, tag_id));
    return tag_id;
  }

  static std::shared_ptr<const DictionaryData> BuildDictionaryData(
      const CacheKey& key) {
    std::vector<ParsedWord> parsed_words;
    std::vector<double> main_weights;
    double freq_sum = 0.0;
    size_t source_ordinal = 0;
    ParseMainDictionary(key.main_path, &parsed_words, &main_weights,
                        &freq_sum, &source_ordinal);

    std::sort(main_weights.begin(), main_weights.end());
    const double min_weight = main_weights.front();
    const double median_weight = main_weights[main_weights.size() / 2];
    const double max_weight = main_weights.back();
    double default_weight = median_weight;
    if (key.weight_option == WordWeightMin) {
      default_weight = min_weight;
    } else if (key.weight_option == WordWeightMax) {
      default_weight = max_weight;
    }

    ParseUserDictionaries(key.user_paths, freq_sum, default_weight,
                          &parsed_words, &source_ordinal);
    std::stable_sort(parsed_words.begin(), parsed_words.end(), ParsedWordLess);

    std::vector<ParsedWord> survivors;
    survivors.reserve(parsed_words.size());
    size_t group_begin = 0;
    while (group_begin < parsed_words.size()) {
      size_t group_end = group_begin + 1;
      size_t winner = group_begin;
      while (group_end < parsed_words.size() &&
             SameWord(parsed_words[group_begin].word,
                      parsed_words[group_end].word)) {
        if (parsed_words[group_end].source_ordinal >
            parsed_words[winner].source_ordinal) {
          winner = group_end;
        }
        ++group_end;
      }
      survivors.push_back(parsed_words[winner]);
      group_begin = group_end;
    }
    XCHECK(!survivors.empty()) << "effective dictionary is empty";

    std::shared_ptr<DictionaryData> data(new DictionaryData);
    data->min_weight = min_weight;
    data->tags.push_back(UNKNOWN_TAG);

    std::unordered_map<std::string, uint16_t> tag_ids;
    tag_ids.insert(std::make_pair(std::string(UNKNOWN_TAG), 0));
    std::vector<DoubleArrayTrie::BuildEntry> entries;
    entries.reserve(survivors.size());
    for (size_t i = 0; i < survivors.size(); ++i) {
      DoubleArrayTrie::BuildEntry entry;
      entry.word = survivors[i].word;
      entry.weight = survivors[i].weight;
      entry.tag_id = InternTag(survivors[i].tag, &tag_ids, &data->tags);
      entries.push_back(entry);

      if (survivors[i].from_user && survivors[i].word.size() == 1) {
        data->user_single_runes.push_back(survivors[i].word[0]);
      }
    }

    std::sort(data->user_single_runes.begin(),
              data->user_single_runes.end());
    data->user_single_runes.erase(
        std::unique(data->user_single_runes.begin(),
                    data->user_single_runes.end()),
        data->user_single_runes.end());

    std::string build_error;
    XCHECK(data->trie.Build(entries, &build_error))
        << "failed to build dictionary double-array trie: " << build_error;
    const DoubleArrayTrie::Stats& trie_stats = data->trie.stats();
    data->stats.slot_count = trie_stats.slot_count;
    data->stats.occupied_state_count = trie_stats.occupied_state_count;
    data->stats.terminal_count = trie_stats.terminal_count;
    data->stats.load_factor = trie_stats.load_factor;
    data->stats.array_bytes = trie_stats.array_bytes;
    for (size_t i = 0; i < data->tags.size(); ++i) {
      data->stats.tag_bytes += data->tags[i].size();
    }

    std::vector<ParsedWord>().swap(parsed_words);
    std::vector<ParsedWord>().swap(survivors);
    std::vector<double>().swap(main_weights);
    std::vector<DoubleArrayTrie::BuildEntry>().swap(entries);
    return std::shared_ptr<const DictionaryData>(data);
  }

  static std::shared_ptr<const DictionaryData> GetDictionaryData(
      const CacheKey& key) {
    typedef std::unordered_map<CacheKey,
        std::weak_ptr<const DictionaryData>, CacheKeyHash> Cache;
    static Cache cache;
    static std::mutex cache_mutex;

    // Build outside the mutex, then converge on the published live value.
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      typename Cache::iterator found = cache.find(key);
      if (found != cache.end()) {
        const std::shared_ptr<const DictionaryData> live =
            found->second.lock();
        if (live) {
          return live;
        }
      }
    }

    const std::shared_ptr<const DictionaryData> built =
        BuildDictionaryData(key);

    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      typename Cache::iterator found = cache.find(key);
      if (found != cache.end()) {
        const std::shared_ptr<const DictionaryData> live =
            found->second.lock();
        if (live) {
          return live;
        }
        found->second = built;
      } else {
        cache.insert(std::make_pair(key, built));
      }
    }
    return built;
  }

  void Init(const std::string& dict_path,
            const std::string& user_dict_paths,
            UserWordWeightOption user_word_weight_opt) {
    CacheKey key;
    key.main_path = dict_path;
    key.user_paths = SplitUserDictionaryPaths(user_dict_paths);
    key.weight_option = user_word_weight_opt;
    data_ = GetDictionaryData(key);
    XCHECK(data_ && data_->stats.terminal_count != 0)
        << "effective dictionary is empty";
  }

  std::shared_ptr<const DictionaryData> data_;
};

}  // namespace cppjieba

#endif  // CPPJIEBA_DICT_TRIE_HPP
