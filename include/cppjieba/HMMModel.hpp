#ifndef CPPJIEBA_HMMMODEL_H
#define CPPJIEBA_HMMMODEL_H

#include "UnicodeFile.hpp"
#include "Utils.hpp"
#include "Trie.hpp"
#include <limits>
#include <new>
#include <stdexcept>

namespace cppjieba {

typedef unordered_map<Rune, double> EmitProbMap;

struct HMMModel {
  /*
   * STATUS:
   * 0: HMMModel::B, 1: HMMModel::E, 2: HMMModel::M, 3:HMMModel::S
   * */
  enum {B = 0, E = 1, M = 2, S = 3, STATUS_SUM = 4};

  // optimize opts into an immutable model for the lifetime of all readers.
  // The legacy public probability fields remain source-compatible, but callers
  // must not modify them (or emitProbVec pointers) in this mode. Mutation through
  // LoadModel/LoadEmitProb is rejected so a derived dense table cannot go stale.
  HMMModel(const string& modelPath, bool optimize = false,
           size_t dense_budget_bytes = 8 * 1024 * 1024)
      : optimized_(false), first_rune_(0), emission_span_(0) {
    memset(startProb, 0, sizeof(startProb));
    memset(transProb, 0, sizeof(transProb));
    statMap[0] = 'B';
    statMap[1] = 'E';
    statMap[2] = 'M';
    statMap[3] = 'S';
    emitProbVec.push_back(&emitProbB);
    emitProbVec.push_back(&emitProbE);
    emitProbVec.push_back(&emitProbM);
    emitProbVec.push_back(&emitProbS);
    LoadModel(modelPath);
    optimized_ = optimize;
    if (optimized_) BuildDenseEmissions(dense_budget_bytes);
  }
  ~HMMModel() {
  }
  void LoadModel(const string& filePath) {
    CheckMutable();
    ifstream ifile;
    OpenInputFile(ifile, filePath);
    XCHECK(ifile.is_open()) << "open " << filePath << " failed";
    string line;
    vector<string> tmp;
    vector<string> tmp2;
    //Load startProb
    XCHECK(GetLine(ifile, line));
    Split(line, tmp, " ");
    XCHECK(tmp.size() == STATUS_SUM);
    for (size_t j = 0; j< tmp.size(); j++) {
      startProb[j] = atof(tmp[j].c_str());
    }

    //Load transProb
    for (size_t i = 0; i < STATUS_SUM; i++) {
      XCHECK(GetLine(ifile, line));
      Split(line, tmp, " ");
      XCHECK(tmp.size() == STATUS_SUM);
      for (size_t j =0; j < STATUS_SUM; j++) {
        transProb[i][j] = atof(tmp[j].c_str());
      }
    }

    //Load emitProbB
    XCHECK(GetLine(ifile, line));
    XCHECK(LoadEmitProb(line, emitProbB));

    //Load emitProbE
    XCHECK(GetLine(ifile, line));
    XCHECK(LoadEmitProb(line, emitProbE));

    //Load emitProbM
    XCHECK(GetLine(ifile, line));
    XCHECK(LoadEmitProb(line, emitProbM));

    //Load emitProbS
    XCHECK(GetLine(ifile, line));
    XCHECK(LoadEmitProb(line, emitProbS));
  }
  double GetEmitProb(const EmitProbMap* ptMp, Rune key, 
        double defVal)const {
    EmitProbMap::const_iterator cit = ptMp->find(key);
    if (cit == ptMp->end()) {
      return defVal;
    }
    return cit->second;
  }
  bool IsOptimizationEnabled() const { return optimized_; }
  bool IsFrozen() const { return optimized_; }
  bool HasDenseEmissions() const { return !dense_emissions_.empty(); }
  size_t GetDenseEmissionBytes() const {
    return dense_emissions_.size() * sizeof(double);
  }

  // Retrieve all four states once per Rune. A rejected dense allocation leaves
  // the original maps available, independently of rolling-score Viterbi.
  void GetEmitProbs(Rune rune, double* probabilities) const {
    if (!HasDenseEmissions()) {
      for (size_t state = 0; state < STATUS_SUM; ++state)
        probabilities[state] = GetEmitProb(emitProbVec[state], rune, MIN_DOUBLE);
      return;
    }
    const uint64_t offset = static_cast<uint64_t>(rune) - first_rune_;
    if (rune < first_rune_ || offset >= emission_span_) {
      for (size_t state = 0; state < STATUS_SUM; ++state)
        probabilities[state] = MIN_DOUBLE;
      return;
    }
    const size_t index = static_cast<size_t>(offset) * STATUS_SUM;
    for (size_t state = 0; state < STATUS_SUM; ++state)
      probabilities[state] = dense_emissions_[index + state];
  }
  bool GetLine(ifstream& ifile, string& line) {
    while (getline(ifile, line)) {
      Trim(line);
      if (line.empty()) {
        continue;
      }
      if (StartsWith(line, "#")) {
        continue;
      }
      return true;
    }
    return false;
  }
  bool LoadEmitProb(const string& line, EmitProbMap& mp) {
    CheckMutable();
    if (line.empty()) {
      return false;
    }
    vector<string> tmp, tmp2;
    Unicode unicode;
    Split(line, tmp, ",");
    for (size_t i = 0; i < tmp.size(); i++) {
      Split(tmp[i], tmp2, ":");
      if (2 != tmp2.size()) {
        XLOG(ERROR) << "emitProb illegal.";
        return false;
      }
      if (!DecodeUTF8RunesInString(tmp2[0], unicode) || unicode.size() != 1) {
        XLOG(ERROR) << "TransCode failed.";
        return false;
      }
      mp[unicode[0]] = atof(tmp2[1].c_str());
    }
    return true;
  }

  char statMap[STATUS_SUM];
  double startProb[STATUS_SUM];
  double transProb[STATUS_SUM][STATUS_SUM];
  EmitProbMap emitProbB;
  EmitProbMap emitProbE;
  EmitProbMap emitProbM;
  EmitProbMap emitProbS;
  vector<EmitProbMap* > emitProbVec;

 private:
  void CheckMutable() const {
    if (optimized_)
      throw std::logic_error("optimized HMM model is frozen; construct a new model");
  }

  void BuildDenseEmissions(size_t budget_bytes) {
    Rune first = std::numeric_limits<Rune>::max();
    Rune last = 0;
    bool found = false;
    for (size_t state = 0; state < STATUS_SUM; ++state) {
      const EmitProbMap& entries = *emitProbVec[state];
      for (EmitProbMap::const_iterator it = entries.begin(); it != entries.end(); ++it) {
        first = std::min(first, it->first);
        last = std::max(last, it->first);
        found = true;
      }
    }
    if (!found) return;
    const uint64_t span = static_cast<uint64_t>(last) - first + 1;
    const size_t row_bytes = STATUS_SUM * sizeof(double);
    if (span > std::numeric_limits<size_t>::max() / row_bytes ||
        span > budget_bytes / row_bytes)
      return;
    try {
      vector<double> dense(static_cast<size_t>(span) * STATUS_SUM, MIN_DOUBLE);
      for (size_t state = 0; state < STATUS_SUM; ++state) {
        const EmitProbMap& entries = *emitProbVec[state];
        for (EmitProbMap::const_iterator it = entries.begin(); it != entries.end(); ++it) {
          const size_t row = static_cast<size_t>(it->first - first);
          dense[row * STATUS_SUM + state] = it->second;
        }
      }
      dense_emissions_.swap(dense);
      first_rune_ = first;
      emission_span_ = span;
    } catch (const std::bad_alloc&) {
      // Dense storage is optional; the model and rolling Viterbi remain usable.
    }
  }

  bool optimized_;
  Rune first_rune_;
  uint64_t emission_span_;
  vector<double> dense_emissions_;
}; // struct HMMModel

} // namespace cppjieba

#endif
