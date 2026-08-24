#ifndef CPPJIEBA_MPSEGMENT_H
#define CPPJIEBA_MPSEGMENT_H

#include <algorithm>
#include <cassert>
#include <cmath>
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
    vector<Dag> dags;
    dictTrie_->BuildDag(begin, end, dags, max_word_len);
    CalcDP(dags);
    CutByDag(begin, end, dags, words);
  }

  const DictTrie* GetDictTrie() const {
    return dictTrie_;
  }

  bool Tag(const string& src, vector<pair<string, string> >& res) const {
    return tagger_.Tag(src, res, *this);
  }

  bool IsUserDictSingleRune(const Rune& value) const {
    return dictTrie_->IsUserDictSingleRune(value);
  }
 private:
  void CalcDP(vector<Dag>& dags) const {
    for (vector<Dag>::reverse_iterator rit = dags.rbegin(); rit != dags.rend(); rit++) {
      rit->weight = MIN_DOUBLE;
      rit->next_pos = 0;
      assert(!rit->edges.empty());
      for (LocalVector<DagEdge>::const_iterator it = rit->edges.begin();
           it != rit->edges.end(); ++it) {
        assert(it->end < dags.size());
        assert(std::isfinite(it->weight));
        double candidate = it->weight;
        if (it->end + 1 < dags.size()) {
          assert(std::isfinite(dags[it->end + 1].weight));
          candidate += dags[it->end + 1].weight;
        }
        assert(std::isfinite(candidate));
        if (candidate > rit->weight) {
          rit->weight = candidate;
          rit->next_pos = it->end;
        }
      }
    }
  }
  void CutByDag(RuneStrArray::const_iterator begin, 
        RuneStrArray::const_iterator end, 
        const vector<Dag>& dags, 
        vector<WordRange>& words) const {
    size_t i = 0;
    assert(static_cast<size_t>(end - begin) == dags.size());
    while (i < dags.size()) {
      assert(dags[i].next_pos >= i && dags[i].next_pos < dags.size());
      words.push_back(WordRange(begin + i, begin + dags[i].next_pos));
      i = dags[i].next_pos + 1;
    }
  }

  const DictTrie* dictTrie_;
  bool isNeedDestroy_;
  PosTagger tagger_;

}; // class MPSegment

} // namespace cppjieba

#endif
