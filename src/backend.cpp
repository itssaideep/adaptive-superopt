#include "adaptive_superopt/backend.h"

#include "adaptive_superopt/expression.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
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

std::uint32_t widthMask(unsigned width) {
  if (width == 0 || width > 32) {
    throw std::invalid_argument("backend bit-vector width must be between 1 and 32");
  }
  if (width == 32) {
    return 0xffffffffu;
  }
  return (std::uint32_t{1} << width) - 1u;
}

bool isConstant(const ExpressionPtr& expression, std::uint32_t* value) {
  if (!expression || expression->op != Op::Constant) {
    return false;
  }
  *value = expression->value;
  return true;
}

double operationLatency(Op operation, const BackendConfig& config) {
  const auto latency = config.latency.find(operation);
  const double result = latency == config.latency.end() ? 1.0 : latency->second;
  if (!(result > 0.0) || !std::isfinite(result)) {
    throw std::invalid_argument(
        "backend operation latencies must be finite and positive");
  }
  return result;
}

CostSummary summarize(
    const ExpressionPtr& expression, const BackendConfig& config,
    std::map<std::string, CostSummary>& memo) {
  if (!expression) {
    return CostSummary{0, 0.0, false, "candidate has no expression"};
  }
  if (expression->op == Op::Variable || expression->op == Op::Constant) {
    return CostSummary{};
  }

  const std::string key = toString(expression);
  if (config.enableCse) {
    const auto cached = memo.find(key);
    if (cached != memo.end()) {
      return cached->second;
    }
  }

  const CostSummary lhs = summarize(expression->lhs, config, memo);
  if (!lhs.supported) {
    return lhs;
  }
  const CostSummary rhs = summarize(expression->rhs, config, memo);
  if (!rhs.supported) {
    return rhs;
  }
  if (config.supportedOperations.find(expression->op) ==
      config.supportedOperations.end()) {
    return CostSummary{0, 0.0, false,
                       "target does not support " + opName(expression->op)};
  }
  if (expression->op == Op::UDiv &&
      (expression->rhs->op != Op::Constant ||
       (expression->rhs->value & widthMask(config.width)) == 0)) {
    return CostSummary{0, 0.0, false,
                       "unsigned division requires a nonzero constant divisor"};
  }

  std::uint32_t rhsConstant = 0;
  const bool rhsIsConstant = isConstant(expression->rhs, &rhsConstant);
  const std::uint32_t mask = widthMask(config.width);
  const bool rhsIsZero = rhsIsConstant && (rhsConstant & mask) == 0;
  const bool rhsIsOne = rhsIsConstant && (rhsConstant & mask) == 1;

  if (config.enablePatternFolding &&
      ((expression->op == Op::Add || expression->op == Op::Sub ||
        expression->op == Op::Or || expression->op == Op::Xor) &&
       rhsIsZero)) {
    if (config.enableCse) {
      memo.emplace(key, lhs);
    }
    return lhs;
  }
  if (config.enablePatternFolding &&
      ((expression->op == Op::Mul || expression->op == Op::UDiv) &&
       rhsIsOne)) {
    if (config.enableCse) {
      memo.emplace(key, lhs);
    }
    return lhs;
  }
  if (config.enablePatternFolding && expression->op == Op::Shl && rhsIsZero) {
    if (config.enableCse) {
      memo.emplace(key, lhs);
    }
    return lhs;
  }

  std::uint32_t lhsConstant = 0;
  const bool lhsIsConstant = isConstant(expression->lhs, &lhsConstant);
  if (config.enablePatternFolding && expression->op == Op::And &&
      ((rhsIsConstant && (rhsConstant & mask) == mask) ||
       (lhsIsConstant && (lhsConstant & mask) == mask))) {
    const CostSummary result = lhsIsConstant ? rhs : lhs;
    if (config.enableCse) {
      memo.emplace(key, result);
    }
    return result;
  }

  const std::size_t sharedInstructions =
      config.enableCse && toString(expression->lhs) == toString(expression->rhs)
          ? lhs.instructions
          : lhs.instructions + rhs.instructions;

  CostSummary result;
  result.instructions = sharedInstructions + 1;
  result.criticalPath =
      std::max(lhs.criticalPath, rhs.criticalPath) +
      operationLatency(expression->op, config);

  if (config.enableStrengthReduction && expression->op == Op::Mul &&
      rhsIsConstant && rhsConstant != 0 &&
      (rhsConstant & (rhsConstant - 1u)) == 0 &&
      config.supportedOperations.find(Op::Shl) !=
          config.supportedOperations.end()) {
    result.criticalPath =
        std::max(lhs.criticalPath, rhs.criticalPath) +
        operationLatency(Op::Shl, config);
  }

  if (config.enablePatternFolding && expression->op == Op::Add) {
    const ExpressionPtr multiply =
        expression->lhs->op == Op::Mul
            ? expression->lhs
            : (expression->rhs->op == Op::Mul ? expression->rhs : nullptr);
    const ExpressionPtr other = multiply == expression->lhs ? expression->rhs
                                                             : expression->lhs;
    if (multiply && config.supportedOperations.find(Op::Mul) !=
                         config.supportedOperations.end()) {
      const CostSummary factorLhs =
          summarize(multiply->lhs, config, memo);
      const CostSummary factorRhs =
          summarize(multiply->rhs, config, memo);
      const CostSummary otherSummary = summarize(other, config, memo);
      result.instructions = factorLhs.instructions + factorRhs.instructions +
                            otherSummary.instructions + 1;
      result.criticalPath =
          std::max({factorLhs.criticalPath, factorRhs.criticalPath,
                    otherSummary.criticalPath}) +
          operationLatency(Op::Add, config);
    }
  }

  if (config.enableCse) {
    memo.emplace(key, result);
  }
  return result;
}

}  // namespace

TargetBackend::TargetBackend(BackendConfig config) : config_(std::move(config)) {
  if (config_.width == 0 || config_.width > 32) {
    throw std::invalid_argument("backend bit-vector width must be between 1 and 32");
  }
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

  std::map<std::string, CostSummary> memo;
  const CostSummary summary = summarize(candidate.program, config_, memo);
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
