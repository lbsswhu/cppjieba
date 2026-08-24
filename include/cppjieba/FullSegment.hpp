#ifndef CPPJIEBA_FULLSEGMENT_H
#define CPPJIEBA_FULLSEGMENT_H

#include <algorithm>
#include <set>
#include <cassert>
#include "Utils.hpp"
#include "DictTrie.hpp"
#include "SegmentBase.hpp"
#include "Unicode.hpp"

namespace cppjieba {
class FullSegment: public SegmentBase {
 public:
  FullSegment(const string& dictPath) {
    dictTrie_ = new DictTrie(dictPath);
    isNeedDestroy_ = true;
  }
  FullSegment(const DictTrie* dictTrie)
    : dictTrie_(dictTrie), isNeedDestroy_(false) {
    assert(dictTrie_);
  }
  ~FullSegment() {
    if (isNeedDestroy_) {
      delete dictTrie_;
    }
  }
  void Cut(const string& sentence, 
        vector<string>& words) const {
    vector<Word> tmp;
    Cut(sentence, tmp);
    GetStringsFromWords(tmp, words);
  }
  void Cut(const string& sentence, 
        vector<Word>& words) const {
    PreFilter pre_filter(symbols_, sentence);
    PreFilter::Range range;
    vector<WordRange> wrs;
    wrs.reserve(sentence.size()/2);
    while (pre_filter.HasNext()) {
      range = pre_filter.Next();
      Cut(range.begin, range.end, wrs);
    }
    words.clear();
    words.reserve(wrs.size());
    GetWordsFromWordRanges(sentence, wrs, words);
  }
  void Cut(RuneStrArray::const_iterator begin, 
        RuneStrArray::const_iterator end, 
        vector<WordRange>& res) const {
    assert(dictTrie_);
    vector<Dag> dags;
    dictTrie_->BuildDag(begin, end, dags);
    size_t max_end_exclusive = 0;
    for (size_t start = 0; start < dags.size(); ++start) {
      const bool only_edge = dags[start].edges.size() == 1;
      for (LocalVector<DagEdge>::const_iterator edge =
               dags[start].edges.begin();
           edge != dags[start].edges.end(); ++edge) {
        assert(edge->end >= start && edge->end < dags.size());
        const size_t word_len = edge->end - start + 1;
        const bool uncovered = max_end_exclusive <= start;
        const bool include = edge->in_dict
            ? word_len >= 2 || (only_edge && uncovered)
            : only_edge && uncovered;
        if (include) {
          res.push_back(WordRange(begin + start, begin + edge->end));
        }
        max_end_exclusive = std::max(max_end_exclusive, start + word_len);
      }
    }
  }
 private:
  const DictTrie* dictTrie_;
  bool isNeedDestroy_;
};
}

#endif
