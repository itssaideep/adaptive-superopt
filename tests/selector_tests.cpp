#include "adaptive_superopt/selector.h"

#include "adaptive_superopt/backend.h"
#include "adaptive_superopt/expression.h"
#include "adaptive_superopt/superoptimizer.h"
#include "adaptive_superopt/verifier.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using adaptive_superopt::BackendResult;
using adaptive_superopt::BackendConfig;
using adaptive_superopt::CachingBackend;
using adaptive_superopt::Candidate;
using adaptive_superopt::AdaptiveSelector;
using adaptive_superopt::ExhaustiveVerifier;
using adaptive_superopt::ExpressionPtr;
using adaptive_superopt::Op;
using adaptive_superopt::SearchConfig;
using adaptive_superopt::SelectorConfig;
using adaptive_superopt::Superoptimizer;
using adaptive_superopt::VerificationStatus;
using adaptive_superopt::makeBinary;
using adaptive_superopt::makeConstant;
using adaptive_superopt::makeVariable;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::vector<Candidate> candidates() {
  return {
      {"bad-static", "specialized", 50.0, {1.0, 0.0}, true, 1.0},
      {"good-backend", "dataflow", 70.0, {0.0, 1.0}, true, 1.0},
      {"unverified", "dataflow", 1.0, {0.0, 1.0}, false, 1.0},
  };
}

void testBudgetAndVerification() {
  SelectorConfig config;
  config.backendBudget = 1;
  config.seedCandidates = 1;

  std::size_t calls = 0;
  AdaptiveSelector selector(100.0, 100.0, config);
  const auto result = selector.select(
      candidates(),
      [&](const Candidate& candidate) {
        ++calls;
        expect(candidate.id != "unverified",
               "unverified candidates must not reach the backend");
        return BackendResult{true, 90.0, 90.0, 10.0, 1.0, ""};
      });

  expect(calls == 1, "selector must respect the backend budget");
  expect(result.backendInvocations == 1,
         "logical backend invocation count must be recorded");
  expect(result.best.has_value(), "successful evaluation should produce a best candidate");
}

void testExhaustiveBudgetFindsBest() {
  SelectorConfig config;
  config.backendBudget = 2;
  config.seedCandidates = 0;
  config.exploration = 0.0;

  const std::map<std::string, double> objectives = {
      {"bad-static", 120.0},
      {"good-backend", 70.0},
  };
  AdaptiveSelector selector(100.0, 100.0, config);
  const auto result = selector.select(
      candidates(),
      [&](const Candidate& candidate) {
        return BackendResult{true, objectives.at(candidate.id),
                             objectives.at(candidate.id), 10.0, 1.0, ""};
      });

  expect(result.best.has_value(), "exhaustive run should find a result");
  expect(result.best->id == "good-backend",
         "best backend objective should win regardless of static ranking");
}

void testBackendFailureIsNotSelected() {
  SelectorConfig config;
  config.backendBudget = 2;
  config.seedCandidates = 1;

  AdaptiveSelector selector(100.0, 100.0, config);
  const auto result = selector.select(
      candidates(),
      [&](const Candidate& candidate) {
        if (candidate.id == "bad-static") {
          return BackendResult{false, 0.0, 0.0, 0.0, 1.0, "compile failed"};
        }
        return BackendResult{true, 80.0, 80.0, 10.0, 1.0, ""};
      });

  expect(result.best.has_value(), "a later successful candidate should win");
  expect(result.best->id != "bad-static",
         "failed backend evaluations must not become the selected result");
}

void testBackendFeedbackChangesTheNextChoice() {
  SelectorConfig config;
  config.backendBudget = 2;
  config.seedCandidates = 1;
  config.exploration = 0.0;

  const std::vector<Candidate> pool = {
      {"static-favorite", "specialized", 40.0, {1.0, 0.0}, true, 1.0},
      {"same-family", "specialized", 45.0, {1.0, 0.0}, true, 1.0},
      {"backend-surprise", "dataflow", 70.0, {0.0, 1.0}, true, 1.0},
  };

  AdaptiveSelector selector(100.0, 100.0, config);
  const auto result = selector.select(
      pool,
      [&](const Candidate& candidate) {
        if (candidate.id == "static-favorite") {
          // The backend disproves the static ranking.
          return BackendResult{true, 130.0, 130.0, 10.0, 1.0, ""};
        }
        return BackendResult{true, 70.0, 70.0, 10.0, 1.0, ""};
      });

  expect(result.evaluations.size() == 2,
         "feedback test should consume exactly two backend calls");
  expect(result.evaluations[0].candidateId == "static-favorite",
         "the first call should use the static prior");
  expect(result.evaluations[1].candidateId == "backend-surprise",
         "the residual model should redirect the second call");
  expect(result.best.has_value() && result.best->id == "backend-surprise",
         "the backend surprise should become the best result");
}

void testExhaustiveVerificationAndBitVectorSemantics() {
  const ExpressionPtr x = makeVariable(0);
  const ExpressionPtr equivalent = makeBinary(Op::Xor, x, makeConstant(0));
  const ExpressionPtr different = makeBinary(Op::Add, x, makeConstant(1));

  adaptive_superopt::VerificationConfig config;
  config.width = 3;
  config.variableCount = 1;
  config.maxCases = 8;
  const ExhaustiveVerifier verifier(config);

  const auto equivalentResult = verifier.verify(x, equivalent);
  expect(equivalentResult.status == VerificationStatus::Equivalent,
         "bit-vector verifier should accept an equivalent expression");
  expect(equivalentResult.casesChecked == 8,
         "verifier should enumerate the complete input domain");

  const auto differentResult = verifier.verify(x, different);
  expect(differentResult.status == VerificationStatus::NotEquivalent,
         "bit-vector verifier should reject a non-equivalent expression");
}

void testBackendCacheAndUnsupportedOperation() {
  BackendConfig config;
  config.supportedOperations = {Op::Add};
  const adaptive_superopt::TargetBackend backend(config);
  CachingBackend cached(backend.evaluator());

  const ExpressionPtr x = makeVariable(0);
  const Candidate add{"add", "add", 2.0, {1.0}, true, 1.0,
                      makeBinary(Op::Add, x, makeConstant(1))};
  const Candidate shift{"shift", "shift", 2.0, {1.0}, true, 1.0,
                        makeBinary(Op::Shl, x, makeConstant(1))};

  expect(cached.evaluate(add).success, "supported operation should compile");
  expect(cached.evaluate(add).success, "cached operation should compile");
  expect(cached.backendCalls() == 1 && cached.cacheHits() == 1,
         "backend cache should avoid repeated compilation");
  expect(!cached.evaluate(shift).success,
         "unsupported operation should be rejected by the backend");
}

void testCompleteSearchPipeline() {
  const ExpressionPtr x = makeVariable(0);
  const ExpressionPtr masked = makeBinary(Op::And, x, makeConstant(15));
  const ExpressionPtr reference = makeBinary(Op::Add, masked, masked);

  SearchConfig config;
  config.generator.width = 4;
  config.generator.variableCount = 1;
  config.generator.maxDepth = 2;
  config.generator.maxExpressions = 250;
  config.verifier.width = 4;
  config.verifier.variableCount = 1;
  config.selector.backendBudget = 3;
  config.selector.seedCandidates = 1;
  config.backend.latency[Op::Shl] = 3.0;

  const auto result = Superoptimizer::run(reference, config);
  expect(result.generatedCandidates > 0,
         "complete pipeline should generate candidates");
  expect(result.verifiedCandidates > 0,
         "complete pipeline should retain verified candidates");
  expect(result.adaptive.backendInvocations <= 3,
         "adaptive search must respect its backend budget");
  expect(result.staticBaseline.backendInvocations <= 3,
         "static baseline must use the same backend budget");
  expect(result.exhaustive.backendInvocations == result.verifiedCandidates,
         "exhaustive baseline should evaluate every verified candidate");
  expect(result.adaptive.best.has_value(),
         "adaptive search should return a successful backend result");
  expect(result.exhaustive.best.has_value(),
         "exhaustive search should return a successful backend result");
}

}  // namespace

int main() {
  testBudgetAndVerification();
  testExhaustiveBudgetFindsBest();
  testBackendFailureIsNotSelected();
  testBackendFeedbackChangesTheNextChoice();
  testExhaustiveVerificationAndBitVectorSemantics();
  testBackendCacheAndUnsupportedOperation();
  testCompleteSearchPipeline();
  std::cout << "all selector tests passed\n";
  return EXIT_SUCCESS;
}
