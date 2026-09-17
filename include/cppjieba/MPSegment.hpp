#ifndef CPPJIEBA_MPSEGMENT_H
#define CPPJIEBA_MPSEGMENT_H

#include <algorithm>
#include <set>
#include <cassert>
#include "Utils.hpp"
#include "DictTrie.hpp"
#include "SegmentTagged.hpp"
#include "PosTagger.hpp"

namespace cppjieba {

class MPSegment: public SegmentTagged {
 public:
  MPSegment(const string& dictPath, const string& userDictPath = "")
    : dictTrie_(new DictTrie(dictPath, userDictPath)), isNeedDestroy_(true) {
  }
  MPSegment(const DictTrie* dictTrie)
    : dictTrie_(dictTrie), isNeedDestroy_(false) {
    assert(dictTrie_);
  }
  ~MPSegment() {
    if (isNeedDestroy_) {
      delete dictTrie_;
    }
  }

  void Cut(const string& sentence, vector<string>& words) const {
    Cut(sentence, words, MAX_WORD_LENGTH);
  }

  void Cut(const string& sentence,
        vector<string>& words,
        size_t max_word_len) const {
    vector<Word> tmp;
    Cut(sentence, tmp, max_word_len);
    GetStringsFromWords(tmp, words);
  }
  void Cut(const string& sentence, 
        vector<Word>& words, 
        size_t max_word_len = MAX_WORD_LENGTH) const {
    PreFilter pre_filter(symbols_, sentence);
    PreFilter::Range range;
    vector<WordRange> wrs;
    wrs.reserve(sentence.size()/2);
    while (pre_filter.HasNext()) {
      range = pre_filter.Next();
      Cut(range.begin, range.end, wrs, max_word_len);
    }
    words.clear();
    words.reserve(wrs.size());
    GetWordsFromWordRanges(sentence, wrs, words);
  }
  void Cut(RuneStrArray::const_iterator begin,
           RuneStrArray::const_iterator end,
           vector<WordRange>& words,
           size_t max_word_len = MAX_WORD_LENGTH) const {
    vector<size_t> lengths;
    dictTrie_->FindBestPath(begin, end, max_word_len, lengths);
    for (size_t i = 0; i < lengths.size(); i += lengths[i]) {
      words.push_back(WordRange(begin + i, begin + i + lengths[i] - 1));
    }
  }

  const DictTrie* GetDictTrie() const {
    return dictTrie_;
  }

  bool Tag(const string& src, vector<pair<string, string> >& res) const {
    return tagger_.Tag(src, res, *this);
  }

  bool IsUserDictSingleChineseWord(const Rune& value) const {
    return dictTrie_->IsUserDictSingleChineseWord(value);
  }
 private:
  const DictTrie* dictTrie_;
  bool isNeedDestroy_;
  PosTagger tagger_;

}; // class MPSegment

} // namespace cppjieba

#endif
