#ifndef CPPJIEBA_MPSEGMENT_H
#define CPPJIEBA_MPSEGMENT_H

#include <algorithm>
#include <set>
#include <cassert>
#include "Utils.hpp"
#include "DictTrie.hpp"
#include "SegmentTagged.hpp"
#include "PosTagger.hpp"
#include "FusedMPCut.hpp"

namespace cppjieba {

class MPSegment: public SegmentTagged {
 public:
  MPSegment(const string& dictPath, const string& userDictPath = "",
            const CpuCutOptions& options = CpuCutOptions())
    : dictTrie_(new DictTrie(dictPath, userDictPath, DictTrie::WordWeightMedian, options)), isNeedDestroy_(true) {
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
    MPCutScratch scratch;
    while (pre_filter.HasNext()) {
      range = pre_filter.Next();
      CutWithScratch(range.begin, range.end, wrs, scratch, max_word_len);
    }
    words.clear();
    words.reserve(wrs.size());
    GetWordsFromWordRanges(sentence, wrs, words);
  }
  void Cut(RuneStrArray::const_iterator begin,
           RuneStrArray::const_iterator end,
           vector<WordRange>& words,
           size_t max_word_len = MAX_WORD_LENGTH) const {
    MPCutScratch scratch;
    CutWithScratch(begin, end, words, scratch, max_word_len);
  }

  void CutWithScratch(RuneStrArray::const_iterator begin,
                      RuneStrArray::const_iterator end,
                      vector<WordRange>& words, MPCutScratch& scratch,
                      size_t max_word_len = MAX_WORD_LENGTH) const {
    assert(end >= begin);
    const size_t n = static_cast<size_t>(end - begin);
    if (!n) return;
    const CpuCutMode mode = dictTrie_->GetCpuCutMode();
    const size_t limit = std::max(size_t(1), std::min(n,
        std::min(dictTrie_->GetActualMaxWordLen(), max_word_len)));
    if (mode == CpuCutMode::LegacyDag || limit > UINT16_MAX) {
#ifdef CPPJIEBA_CPU_DIAGNOSTICS
      ++scratch.legacy_ranges;
#endif
      CutRangeLegacy(begin, end, words, max_word_len);
      return;
    }
    if (mode == CpuCutMode::PointerFused) {
      PointerWalker walker(dictTrie_->GetTrie(), begin);
      CutRangeFused(begin, n, limit, dictTrie_->GetMinWeight(), walker, scratch, words);
    } else {
      const DatModel& model = *dictTrie_->GetDatModel();
      RawDatWalker walker(model, begin);
      CutRangeFused(begin, n, limit, model.UnknownWeight(), walker, scratch, words);
    }
  }

 private:
  void CutRangeLegacy(RuneStrArray::const_iterator begin,
                      RuneStrArray::const_iterator end,
                      vector<WordRange>& words, size_t max_word_len) const {
    vector<Dag> dags;
    dictTrie_->Find(begin, 
          end, 
          dags,
          max_word_len);
    CalcDP(dags);
    CutByDag(begin, end, dags, words);
  }

 public:
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
  void CalcDP(vector<Dag>& dags) const {
    size_t nextPos;
    const DictUnit* p;
    double val;

    for (vector<Dag>::reverse_iterator rit = dags.rbegin(); rit != dags.rend(); rit++) {
      rit->pInfo = NULL;
      rit->weight = MIN_DOUBLE;
      assert(!rit->nexts.empty());
      for (LocalVector<pair<size_t, const DictUnit*> >::const_iterator it = rit->nexts.begin(); it != rit->nexts.end(); it++) {
        nextPos = it->first;
        p = it->second;
        val = 0.0;
        if (nextPos + 1 < dags.size()) {
          val += dags[nextPos + 1].weight;
        }

        if (p) {
          val += p->weight;
        } else {
          val += dictTrie_->GetMinWeight();
        }
        if (val > rit->weight) {
          rit->pInfo = p;
          rit->weight = val;
        }
      }
    }
  }
  void CutByDag(RuneStrArray::const_iterator begin, 
        RuneStrArray::const_iterator end, 
        const vector<Dag>& dags, 
        vector<WordRange>& words) const {
    size_t i = 0;
    while (i < dags.size()) {
      const DictUnit* p = dags[i].pInfo;
      if (p) {
        assert(p->word.size() >= 1);
        WordRange wr(begin + i, begin + i + p->word.size() - 1);
        words.push_back(wr);
        i += p->word.size();
      } else { //single chinese word
        WordRange wr(begin + i, begin + i);
        words.push_back(wr);
        i++;
      }
    }
  }

  const DictTrie* dictTrie_;
  bool isNeedDestroy_;
  PosTagger tagger_;

}; // class MPSegment

} // namespace cppjieba

#endif
