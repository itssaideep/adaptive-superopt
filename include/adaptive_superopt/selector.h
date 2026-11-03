#pragma once

#include "adaptive_superopt/expression.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace adaptive_superopt {

// The result returned by one logical invocation of the target backend.
// objective is deliberately generic: a target-backend adapter can encode a
// pre-registered cycle/size/spill objective here while retaining the raw
// measurements for reporting.
struct BackendResult {
  bool success = false;
  double objective = 0.0;
  double executionCycles = 0.0;
  double codeSizeBytes = 0.0;
  double backendTimeMs = 0.0;
  std::string diagnostic;
};

struct Candidate {
  std::string id;
  std::string family;
  double staticCost = 0.0;
  std::vector<double> features;
  bool semanticallyVerified = false;
  double estimatedBackendTimeMs = 1.0;
  ExpressionPtr program;
};

using BackendEvaluator = std::function<BackendResult(const Candidate&)>;

struct SelectorConfig {
  std::size_t backendBudget = 0;
  std::size_t seedCandidates = 1;
  double exploration = 1.0;
  double timePenalty = 0.0;
  double ridge = 1.0;
  double failurePenalty = 1.0;
};

struct EvaluationRecord {
  std::string candidateId;
  double staticGain = 0.0;
  double predictedGain = 0.0;
  double uncertainty = 0.0;
  double acquisition = 0.0;
  BackendResult backend;
  std::size_t backendCall = 0;
};

struct SelectionResult {
  std::optional<Candidate> best;
  std::optional<BackendResult> bestBackendResult;
  std::size_t backendInvocations = 0;
  std::vector<EvaluationRecord> evaluations;
};

class AdaptiveSelector {
 public:
  AdaptiveSelector(double baselineStaticCost,
                   double baselineBackendObjective,
                   SelectorConfig config = {});

  SelectionResult select(const std::vector<Candidate>& candidates,
                         const BackendEvaluator& evaluate) const;

 private:
  double baselineStaticCost_;
  double baselineBackendObjective_;
  SelectorConfig config_;
};

}  // namespace adaptive_superopt
