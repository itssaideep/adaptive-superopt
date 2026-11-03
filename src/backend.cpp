#include "adaptive_superopt/backend.h"

#include "adaptive_superopt/expression.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>

namespace adaptive_superopt {
namespace {

struct CostSummary {
  std::size_t instructions = 0;
  double criticalPath = 0.0;
  bool supported = true;
  std::string diagnostic;
};

CostSummary summarize(const ExpressionPtr& expression,
                      const BackendConfig& config) {
  if (!expression) {
    return CostSummary{0, 0.0, false, "candidate has no expression"};
  }
  if (expression->op == Op::Variable || expression->op == Op::Constant) {
    return CostSummary{};
  }

  const CostSummary lhs = summarize(expression->lhs, config);
  if (!lhs.supported) {
    return lhs;
  }
  const CostSummary rhs = summarize(expression->rhs, config);
  if (!rhs.supported) {
    return rhs;
  }
  if (config.supportedOperations.find(expression->op) ==
      config.supportedOperations.end()) {
    return CostSummary{0, 0.0, false,
                       "target does not support " + opName(expression->op)};
  }

  const auto latency = config.latency.find(expression->op);
  const double operationLatency =
      latency == config.latency.end() ? 1.0 : latency->second;
  if (!(operationLatency > 0.0) || !std::isfinite(operationLatency)) {
    throw std::invalid_argument("backend operation latencies must be finite and positive");
  }

  return CostSummary{
      lhs.instructions + rhs.instructions + 1,
      std::max(lhs.criticalPath, rhs.criticalPath) + operationLatency,
      true,
      "",
  };
}

}  // namespace

TargetBackend::TargetBackend(BackendConfig config) : config_(std::move(config)) {
  if (config_.lanes == 0) {
    throw std::invalid_argument("backend lanes must be positive");
  }
  if (config_.instructionBytes == 0) {
    throw std::invalid_argument("instructionBytes must be positive");
  }
  if (config_.codeSizeWeight < 0.0 ||
      !std::isfinite(config_.codeSizeWeight)) {
    throw std::invalid_argument("codeSizeWeight must be finite and non-negative");
  }
}

BackendResult TargetBackend::evaluate(const Candidate& candidate) const {
  if (!candidate.program) {
    return BackendResult{false, 0.0, 0.0, 0.0, 0.0,
                         "candidate has no executable expression"};
  }

  const CostSummary summary = summarize(candidate.program, config_);
  if (!summary.supported) {
    return BackendResult{false, 0.0, 0.0, 0.0, 0.01, summary.diagnostic};
  }

  const double issueCycles =
      std::ceil(static_cast<double>(summary.instructions) / config_.lanes);
  // A compiled fragment has a minimum one-cycle cost in this reference
  // backend, including a leaf expression such as a variable or constant.
  const double cycles = std::max(1.0, std::max(summary.criticalPath, issueCycles));
  const double codeSize =
      static_cast<double>(summary.instructions * config_.instructionBytes);
  const double objective = cycles + config_.codeSizeWeight * codeSize;
  const double backendTime = 0.01 + 0.005 * summary.instructions;
  return BackendResult{true, objective, cycles, codeSize, backendTime, ""};
}

BackendEvaluator TargetBackend::evaluator() const {
  return [this](const Candidate& candidate) { return evaluate(candidate); };
}

CachingBackend::CachingBackend(BackendEvaluator evaluator)
    : evaluator_(std::move(evaluator)) {
  if (!evaluator_) {
    throw std::invalid_argument("a backend evaluator is required");
  }
}

BackendResult CachingBackend::evaluate(const Candidate& candidate) {
  const auto cached = cache_.find(candidate.id);
  if (cached != cache_.end()) {
    ++cacheHits_;
    return cached->second;
  }
  BackendResult result = evaluator_(candidate);
  ++backendCalls_;
  cache_.emplace(candidate.id, result);
  return result;
}

BackendEvaluator CachingBackend::evaluator() {
  return [this](const Candidate& candidate) { return evaluate(candidate); };
}

}  // namespace adaptive_superopt
