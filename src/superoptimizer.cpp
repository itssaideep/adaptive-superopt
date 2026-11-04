#include "adaptive_superopt/superoptimizer.h"

#include "adaptive_superopt/expression.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace adaptive_superopt {
namespace {

double staticCostFor(const ExpressionPtr& expression) {
  return 1.0 + static_cast<double>(
                   nodeCount(expression) -
                   operationCount(expression, Op::Variable) -
                   operationCount(expression, Op::Constant));
}

std::vector<double> featuresFor(const ExpressionPtr& expression) {
  return {
      1.0,
      staticCostFor(expression),
      static_cast<double>(expressionDepth(expression)),
      static_cast<double>(operationCount(expression, Op::Add)),
      static_cast<double>(operationCount(expression, Op::Sub)),
      static_cast<double>(operationCount(expression, Op::Mul)),
      static_cast<double>(operationCount(expression, Op::UDiv)),
      static_cast<double>(operationCount(expression, Op::Xor)),
      static_cast<double>(operationCount(expression, Op::And)),
      static_cast<double>(operationCount(expression, Op::Or)),
      static_cast<double>(operationCount(expression, Op::Shl)),
  };
}

Candidate candidateFor(const ExpressionPtr& expression) {
  const std::string id = toString(expression);
  const Op familyOperation = expression->op;
  return Candidate{
      id,
      opName(familyOperation),
      staticCostFor(expression),
      featuresFor(expression),
      true,
      0.01 + 0.005 * static_cast<double>(nodeCount(expression)),
      expression,
  };
}

void validateBackendResult(const BackendResult& backend) {
  if (backend.success &&
      (!(backend.objective > 0.0) || !std::isfinite(backend.objective))) {
    throw std::invalid_argument(
        "successful backend result must have a positive finite objective");
  }
}

SelectionResult runStatic(const std::vector<Candidate>& candidates,
                          std::size_t budget,
                          const BackendEvaluator& evaluate,
                          double baselineStaticCost,
                          double baselineBackendObjective) {
  SelectionResult result;
  std::vector<std::size_t> order(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    order[index] = index;
  }
  std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs,
                                                   std::size_t rhs) {
    if (candidates[lhs].staticCost != candidates[rhs].staticCost) {
      return candidates[lhs].staticCost < candidates[rhs].staticCost;
    }
    return candidates[lhs].id < candidates[rhs].id;
  });

  double bestObjective = baselineBackendObjective;
  for (const std::size_t index : order) {
    if (result.backendInvocations >= budget) {
      break;
    }
    const Candidate& candidate = candidates[index];
    const double staticGain =
        (baselineStaticCost - candidate.staticCost) / baselineStaticCost;
    BackendResult backend = evaluate(candidate);
    ++result.backendInvocations;
    validateBackendResult(backend);
    if (backend.success && backend.objective < bestObjective) {
      bestObjective = backend.objective;
      result.best = candidate;
      result.bestBackendResult = backend;
    }
    result.evaluations.push_back(EvaluationRecord{
        candidate.id,
        staticGain,
        staticGain,
        0.0,
        0.0,
        std::move(backend),
        result.backendInvocations,
    });
  }
  return result;
}

}  // namespace

SearchResult Superoptimizer::run(const ExpressionPtr& reference,
                                 SearchConfig config) {
  if (!reference) {
    throw std::invalid_argument("reference expression must not be null");
  }
  if (config.generator.width != config.verifier.width ||
      config.generator.variableCount != config.verifier.variableCount) {
    throw std::invalid_argument(
        "generator and verifier domains must have matching width and variables");
  }
  if (config.backend.width != config.generator.width) {
    throw std::invalid_argument(
        "generator and backend domains must have matching bit width");
  }

  TargetBackend backend(config.backend);
  const Candidate referenceCandidate = candidateFor(reference);
  const BackendResult referenceBackend =
      backend.evaluate(referenceCandidate);
  validateBackendResult(referenceBackend);
  if (!referenceBackend.success) {
    throw std::runtime_error("reference backend evaluation failed: " +
                             referenceBackend.diagnostic);
  }

  const ExpressionGenerator generator(config.generator);
  const std::vector<ExpressionPtr> generated = generator.generate();
  const ExhaustiveVerifier verifier(config.verifier);

  std::vector<Candidate> verified;
  std::map<std::string, bool> seen;
  for (const ExpressionPtr& expression : generated) {
    const VerificationResult verification = verifier.verify(reference, expression);
    if (!verification.equivalent()) {
      continue;
    }
    Candidate candidate = candidateFor(expression);
    if (seen.emplace(candidate.id, true).second) {
      verified.push_back(std::move(candidate));
    }
  }

  if (seen.emplace(referenceCandidate.id, true).second) {
    verified.push_back(referenceCandidate);
  }

  SearchResult result;
  result.reference = referenceCandidate;
  result.referenceBackend = referenceBackend;
  result.generatedCandidates = generated.size();
  result.verifiedCandidates = verified.size();
  result.candidates = verified;

  CachingBackend adaptiveBackend(backend.evaluator());
  const auto adaptiveStart = std::chrono::steady_clock::now();
  result.adaptive = AdaptiveSelector(
                        referenceCandidate.staticCost,
                        referenceBackend.objective,
                        config.selector)
                        .select(verified, adaptiveBackend.evaluator());
  result.adaptiveRuntimeMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - adaptiveStart)
          .count();
  result.adaptiveCacheHits = adaptiveBackend.cacheHits();

  CachingBackend staticBackend(backend.evaluator());
  const auto staticStart = std::chrono::steady_clock::now();
  result.staticBaseline = runStatic(
      verified, config.selector.backendBudget, staticBackend.evaluator(),
      referenceCandidate.staticCost, referenceBackend.objective);
  result.staticRuntimeMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - staticStart)
          .count();
  result.staticCacheHits = staticBackend.cacheHits();

  CachingBackend exhaustiveBackend(backend.evaluator());
  const auto exhaustiveStart = std::chrono::steady_clock::now();
  result.exhaustive = runStatic(
      verified, verified.size(), exhaustiveBackend.evaluator(),
      referenceCandidate.staticCost, referenceBackend.objective);
  result.exhaustiveRuntimeMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - exhaustiveStart)
          .count();
  result.exhaustiveCacheHits = exhaustiveBackend.cacheHits();

  const auto retainReferenceAsIncumbent = [&](SelectionResult& selection) {
    if (!selection.best.has_value() ||
        selection.bestBackendResult->objective > referenceBackend.objective) {
      selection.best = referenceCandidate;
      selection.bestBackendResult = referenceBackend;
    }
  };
  retainReferenceAsIncumbent(result.adaptive);
  retainReferenceAsIncumbent(result.staticBaseline);
  retainReferenceAsIncumbent(result.exhaustive);

  return result;
}

}  // namespace adaptive_superopt
