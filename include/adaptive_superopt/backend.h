#pragma once

#include "adaptive_superopt/selector.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace adaptive_superopt {

struct BackendConfig {
  unsigned width = 8;
  unsigned lanes = 2;
  std::size_t instructionBytes = 4;
  double codeSizeWeight = 0.02;
  bool enableCse = true;
  bool enableStrengthReduction = true;
  bool enablePatternFolding = true;
  std::set<Op> supportedOperations = {
      Op::Add, Op::Sub, Op::Mul, Op::UDiv, Op::Xor, Op::And, Op::Or,
      Op::Shl};
  std::map<Op, double> latency = {
      {Op::Add, 1.0}, {Op::Sub, 1.0}, {Op::Mul, 2.0}, {Op::UDiv, 4.0},
      {Op::Xor, 1.0}, {Op::And, 1.0}, {Op::Or, 1.0}, {Op::Shl, 1.0}};
};

class TargetBackend {
 public:
  explicit TargetBackend(BackendConfig config = {});

  BackendResult evaluate(const Candidate& candidate) const;
  BackendEvaluator evaluator() const;

 private:
  BackendConfig config_;
};

class CachingBackend {
 public:
  explicit CachingBackend(BackendEvaluator evaluator);

  BackendResult evaluate(const Candidate& candidate);
  BackendEvaluator evaluator();

  std::size_t backendCalls() const { return backendCalls_; }
  std::size_t cacheHits() const { return cacheHits_; }

 private:
  BackendEvaluator evaluator_;
  std::map<std::string, BackendResult> cache_;
  std::size_t backendCalls_ = 0;
  std::size_t cacheHits_ = 0;
};

}  // namespace adaptive_superopt
