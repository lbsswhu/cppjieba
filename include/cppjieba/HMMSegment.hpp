#ifndef CPPJIBEA_HMMSEGMENT_H
#define CPPJIBEA_HMMSEGMENT_H

#include <iostream>
#include <fstream>
#include <memory.h>
#include <cassert>
#include "HMMModel.hpp"
#include "HmmScratch.hpp"
#include "SegmentBase.hpp"

namespace cppjieba {
class HMMSegment: public SegmentBase {
 public:
  HMMSegment(const string& filePath)
  : model_(new HMMModel(filePath)), isNeedDestroy_(true) {
  }
  HMMSegment(const HMMModel* model) 
  : model_(model), isNeedDestroy_(false) {
  }
  ~HMMSegment() {
    if (isNeedDestroy_) {
      delete model_;
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
    HmmScratch scratch;
    wrs.reserve(sentence.size()/2);
    while (pre_filter.HasNext()) {
      range = pre_filter.Next();
      CutWithScratch(range.begin, range.end, wrs, scratch);
    }
    words.clear();
    words.reserve(wrs.size());
    GetWordsFromWordRanges(sentence, wrs, words);
  }
  void Cut(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end, vector<WordRange>& res) const {
    HmmScratch scratch;
    CutWithScratch(begin, end, res, scratch);
  }
  void CutWithScratch(RuneStrArray::const_iterator begin,
                      RuneStrArray::const_iterator end,
                      vector<WordRange>& res, HmmScratch& scratch) const {
    RuneStrArray::const_iterator left = begin;
    RuneStrArray::const_iterator right = begin;
    while (right != end) {
      if (right->rune < 0x80) {
        if (left != right) {
          InternalCut(left, right, res, scratch);
        }
        left = right;
        do {
          right = SequentialLetterRule(left, end);
          if (right != left) {
            break;
          }
          right = NumbersRule(left, end);
          if (right != left) {
            break;
          }
          right ++;
        } while (false);
        WordRange wr(left, right - 1);
        res.push_back(wr);
        left = right;
      } else {
        right++;
      }
    }
    if (left != right) {
      InternalCut(left, right, res, scratch);
    }
  }
 private:
  // sequential letters rule
  RuneStrArray::const_iterator SequentialLetterRule(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end) const {
    Rune x = begin->rune;
    if (('a' <= x && x <= 'z') || ('A' <= x && x <= 'Z')) {
      begin ++;
    } else {
      return begin;
    }
    while (begin != end) {
      x = begin->rune;
      if (('a' <= x && x <= 'z') || ('A' <= x && x <= 'Z') || ('0' <= x && x <= '9')) {
        begin ++;
      } else {
        break;
      }
    }
    // Optionally consume a decimal suffix (e.g. "abc1.2" -> keep together)
    if (begin != end && begin->rune == '.') {
      RuneStrArray::const_iterator after_dot = begin + 1;
      if (after_dot != end && '0' <= after_dot->rune && after_dot->rune <= '9') {
        begin++; // consume '.'
        while (begin != end && '0' <= begin->rune && begin->rune <= '9') {
          begin++;
        }
      }
    }
    return begin;
  }
  // numbers rule: digits optionally followed by letters, then an optional decimal suffix
  // Handles the digit-starting subset of Python jieba finalseg re_skip = "[a-zA-Z0-9]+(?:\.\d+)?"
  RuneStrArray::const_iterator NumbersRule(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end) const {
    Rune x = begin->rune;
    if ('0' <= x && x <= '9') {
      begin ++;
    } else {
      return begin;
    }
    while (begin != end) {
      x = begin->rune;
      if (('0' <= x && x <= '9') || ('a' <= x && x <= 'z') || ('A' <= x && x <= 'Z')) {
        begin++;
      } else {
        break;
      }
    }
    // Optionally consume a decimal suffix (e.g. "3.14" or "5G3.14")
    if (begin != end && begin->rune == '.') {
      RuneStrArray::const_iterator after_dot = begin + 1;
      if (after_dot != end && '0' <= after_dot->rune && after_dot->rune <= '9') {
        begin++; // consume '.'
        while (begin != end && '0' <= begin->rune && begin->rune <= '9') {
          begin++;
        }
      }
    }
    return begin;
  }
  void InternalCut(RuneStrArray::const_iterator begin, RuneStrArray::const_iterator end,
                   vector<WordRange>& res, HmmScratch& scratch) const {
    if (model_->IsOptimizationEnabled()) {
      ViterbiRolling(begin, end, scratch);
      AppendWords(begin, scratch.status, res);
      return;
    }
    vector<size_t> status;
    Viterbi(begin, end, status);
    AppendWords(begin, status, res);
  }

  template <typename States>
  void AppendWords(RuneStrArray::const_iterator begin, const States& status,
                   vector<WordRange>& res) const {
    RuneStrArray::const_iterator left = begin;
    RuneStrArray::const_iterator right;
    for (size_t i = 0; i < status.size(); i++) {
      if (status[i] % 2) { //if (HMMModel::E == status[i] || HMMModel::S == status[i])
        right = begin + i + 1;
        WordRange wr(left, right - 1);
        res.push_back(wr);
        left = right;
      }
    }
  }

  void ViterbiRolling(RuneStrArray::const_iterator begin,
                      RuneStrArray::const_iterator end,
                      HmmScratch& scratch) const {
    const size_t count = end - begin;
    scratch.Prepare(count);
    if (!count) return;
    double emissions[HMMModel::STATUS_SUM];
    model_->GetEmitProbs(begin->rune, emissions);
    for (size_t y = 0; y < HMMModel::STATUS_SUM; ++y) {
      scratch.prev[y] = model_->startProb[y] + emissions[y];
      scratch.path[y] = 0xff;
    }
    for (size_t x = 1; x < count; ++x) {
      model_->GetEmitProbs((begin + x)->rune, emissions);
      for (size_t y = 0; y < HMMModel::STATUS_SUM; ++y) {
        scratch.cur[y] = MIN_DOUBLE;
        uint8_t& predecessor = scratch.path[x * HMMModel::STATUS_SUM + y];
        predecessor = HMMModel::E;
        for (size_t previous = 0; previous < HMMModel::STATUS_SUM; ++previous) {
          const double score = (scratch.prev[previous] + model_->transProb[previous][y])
              + emissions[y];
          if (score > scratch.cur[y]) {
            scratch.cur[y] = score;
            predecessor = static_cast<uint8_t>(previous);
          }
        }
      }
      for (size_t y = 0; y < HMMModel::STATUS_SUM; ++y)
        scratch.prev[y] = scratch.cur[y];
    }
    uint8_t state = scratch.prev[HMMModel::E] >= scratch.prev[HMMModel::S]
        ? HMMModel::E : HMMModel::S;
    for (size_t x = count; x-- > 0;) {
      scratch.status[x] = state;
      if (x) state = scratch.path[x * HMMModel::STATUS_SUM + state];
    }
  }

  void Viterbi(RuneStrArray::const_iterator begin, 
        RuneStrArray::const_iterator end, 
        vector<size_t>& status) const {
    size_t Y = HMMModel::STATUS_SUM;
    size_t X = end - begin;

    size_t XYSize = X * Y;
    size_t now, old, stat;
    double tmp, endE, endS;

    vector<int> path(XYSize);
    vector<double> weight(XYSize);

    //start
    for (size_t y = 0; y < Y; y++) {
      weight[0 + y * X] = model_->startProb[y] + model_->GetEmitProb(model_->emitProbVec[y], begin->rune, MIN_DOUBLE);
      path[0 + y * X] = -1;
    }

    double emitProb;

    for (size_t x = 1; x < X; x++) {
      for (size_t y = 0; y < Y; y++) {
        now = x + y*X;
        weight[now] = MIN_DOUBLE;
        path[now] = HMMModel::E; // warning
        emitProb = model_->GetEmitProb(model_->emitProbVec[y], (begin+x)->rune, MIN_DOUBLE);
        for (size_t preY = 0; preY < Y; preY++) {
          old = x - 1 + preY * X;
          tmp = weight[old] + model_->transProb[preY][y] + emitProb;
          if (tmp > weight[now]) {
            weight[now] = tmp;
            path[now] = preY;
          }
        }
      }
    }

    endE = weight[X-1+HMMModel::E*X];
    endS = weight[X-1+HMMModel::S*X];
    stat = 0;
    if (endE >= endS) {
      stat = HMMModel::E;
    } else {
      stat = HMMModel::S;
    }

    status.resize(X);
    for (int x = X -1 ; x >= 0; x--) {
      status[x] = stat;
      stat = path[x + stat*X];
    }
  }

  const HMMModel* model_;
  bool isNeedDestroy_;
}; // class HMMSegment

} // namespace cppjieba

#endif
