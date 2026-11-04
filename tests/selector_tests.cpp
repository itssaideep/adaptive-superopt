#include "adaptive_superopt/selector.h"

#include "adaptive_superopt/backend.h"
#include "adaptive_superopt/expression.h"
#include "adaptive_superopt/generator.h"
#include "adaptive_superopt/superoptimizer.h"
#include "adaptive_superopt/verifier.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using adaptive_superopt::BackendResult;
using adaptive_superopt::BackendConfig;
using adaptive_superopt::CachingBackend;
using adaptive_superopt::Candidate;
using adaptive_superopt::AdaptiveSelector;
using adaptive_superopt::ExhaustiveVerifier;
using adaptive_superopt::ExpressionGenerator;
using adaptive_superopt::ExpressionPtr;
using adaptive_superopt::GeneratorConfig;
using adaptive_superopt::Op;
using adaptive_superopt::SearchConfig;
using adaptive_superopt::SelectorConfig;
using adaptive_superopt::Superoptimizer;
using adaptive_superopt::VerificationConfig;
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

void expectThrows(const std::function<void()>& action,
                  const std::string& message) {
  try {
    action();
  } catch (const std::exception&) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool hasInvalidShift(const ExpressionPtr& expression) {
  if (!expression || expression->op == Op::Variable ||
      expression->op == Op::Constant) {
    return false;
  }
  if (expression->op == Op::Shl &&
      (!expression->rhs || expression->rhs->op != Op::Constant)) {
    return true;
  }
  return hasInvalidShift(expression->lhs) || hasInvalidShift(expression->rhs);
}

bool hasInvalidDivision(const ExpressionPtr& expression) {
  if (!expression || expression->op == Op::Variable ||
      expression->op == Op::Constant) {
    return false;
  }
  if (expression->op == Op::UDiv &&
      (!expression->rhs || expression->rhs->op != Op::Constant ||
       expression->rhs->value == 0)) {
    return true;
  }
  return hasInvalidDivision(expression->lhs) ||
         hasInvalidDivision(expression->rhs);
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

void testBitVectorWraparoundAndMultiVariableVerification() {
  const ExpressionPtr x = makeVariable(0);
  const ExpressionPtr y = makeVariable(1);
  const ExpressionPtr add = makeBinary(Op::Add, x, makeConstant(1));
  const ExpressionPtr subtract = makeBinary(Op::Sub, x, makeConstant(2));
  const ExpressionPtr shift = makeBinary(Op::Shl, x, makeConstant(1));

  expect(adaptive_superopt::evaluate(add, {15}, 4) == 0,
         "addition should wrap at the configured bit width");
  expect(adaptive_superopt::evaluate(subtract, {0}, 4) == 14,
         "subtraction should wrap at the configured bit width");
  expect(adaptive_superopt::evaluate(shift, {9}, 4) == 2,
         "left shift should wrap at the configured bit width");
  expect(adaptive_superopt::evaluate(
             makeBinary(Op::Mul, x, makeConstant(2)), {15}, 4) == 14,
         "multiplication should wrap at the configured bit width");
  expect(adaptive_superopt::evaluate(
             makeBinary(Op::UDiv, x, makeConstant(4)), {15}, 4) == 3,
         "unsigned division should use fixed-width unsigned semantics");
  expect(adaptive_superopt::evaluate(
             makeBinary(Op::Or, x, makeConstant(1)), {8}, 4) == 9,
         "bitwise OR should preserve fixed-width bit-vector semantics");
  expect(adaptive_superopt::evaluate(x, {31}, 4) == 15,
         "variable inputs should be masked to the configured bit width");
  expectThrows(
      [&] { adaptive_superopt::evaluate(
                makeBinary(Op::UDiv, x, makeConstant(0)), {1}, 4); },
      "unsigned division by zero should be rejected");

  adaptive_superopt::VerificationConfig config;
  config.width = 2;
  config.variableCount = 2;
  config.maxCases = 16;
  const ExhaustiveVerifier verifier(config);
  const auto result = verifier.verify(makeBinary(Op::Add, x, y),
                                      makeBinary(Op::Add, y, x));
  expect(result.status == VerificationStatus::Equivalent,
         "multi-variable commutativity should verify over the full domain");
  expect(result.casesChecked == 16,
         "multi-variable verification should check every input tuple");
}

void testVerificationBudgetAndInvalidExpressions() {
  const ExpressionPtr x = makeVariable(0);
  const ExhaustiveVerifier oversized(VerificationConfig{4, 2, 15});
  const auto oversizedResult = oversized.verify(x, x);
  expect(oversizedResult.status == VerificationStatus::Rejected,
         "verification should reject an input domain over its case budget");

  const ExhaustiveVerifier invalid(VerificationConfig{4, 1, 16});
  const auto invalidResult = invalid.verify(x, makeVariable(1));
  expect(invalidResult.status == VerificationStatus::Rejected,
         "verification should reject expressions with missing variables");

  expectThrows(
      [] { ExhaustiveVerifier(VerificationConfig{0, 1, 1}); },
      "verifier should reject a zero-bit domain");
}

void testGeneratorBoundsAndCanonicalization() {
  GeneratorConfig config;
  config.width = 4;
  config.variableCount = 1;
  config.maxDepth = 2;
  config.maxExpressions = 40;
  const ExpressionGenerator generator(config);
  const auto expressions = generator.generate();
  std::set<std::string> ids;
  for (const ExpressionPtr& expression : expressions) {
    ids.insert(adaptive_superopt::toString(expression));
    expect(adaptive_superopt::expressionDepth(expression) <= 2,
           "generator should respect the maximum expression depth");
    expect(!hasInvalidShift(expression),
           "generated shifts should use constant shift amounts");
    expect(!hasInvalidDivision(expression),
           "generated divisions should use nonzero constant divisors");
  }
  expect(expressions.size() <= 40,
         "generator should respect the maximum expression count");
  expect(ids.size() == expressions.size(),
         "generator should not emit duplicate expressions");

  GeneratorConfig tiny = config;
  tiny.maxExpressions = 1;
  expect(ExpressionGenerator(tiny).generate().size() == 1,
         "generator should support the smallest positive candidate limit");
}

void testSelectorBoundaryRejections() {
  const Candidate candidate{"candidate", "test", 1.0, {1.0}, true, 1.0};
  const auto evaluator = [](const Candidate&) {
    return BackendResult{true, 1.0, 1.0, 4.0, 1.0, ""};
  };

  SelectorConfig zeroBudget;
  zeroBudget.backendBudget = 0;
  const auto empty = AdaptiveSelector(2.0, 2.0, zeroBudget)
                         .select({candidate}, evaluator);
  expect(empty.backendInvocations == 0 && empty.evaluations.empty(),
         "zero budget should perform no backend calls");

  const Candidate unverified{"unverified", "test", 1.0, {1.0}, false, 1.0};
  SelectorConfig budget;
  budget.backendBudget = 1;
  const auto rejected = AdaptiveSelector(2.0, 2.0, budget)
                            .select({unverified}, evaluator);
  expect(rejected.backendInvocations == 0,
         "a pool without verified candidates should perform no calls");

  expectThrows(
      [&] {
        AdaptiveSelector(2.0, 2.0, budget).select(
            {candidate}, [](const Candidate&) {
              return BackendResult{true, 0.0, 0.0, 4.0, 1.0, "bad objective"};
            });
      },
      "selector should reject successful backend results with zero objective");

  const Candidate mismatched{"mismatched", "test", 1.0, {1.0, 2.0}, true, 1.0};
  expectThrows(
      [&] {
        AdaptiveSelector(2.0, 2.0, budget).select(
            {candidate, mismatched}, evaluator);
      },
      "selector should reject inconsistent feature dimensions");
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

void testStaticAndBackendRankingsCanDisagree() {
  BackendConfig config;
  config.width = 8;
  config.latency[Op::Mul] = 3.0;
  config.latency[Op::Shl] = 1.0;
  const adaptive_superopt::TargetBackend backend(config);
  const ExpressionPtr x = makeVariable(0);

  const Candidate multiply{
      "multiply-by-three", "multiply", 2.0, {1.0}, true, 1.0,
      makeBinary(Op::Mul, x, makeConstant(3))};
  const Candidate shiftAdd{
      "shift-add-by-three", "shift-add", 3.0, {1.0}, true, 1.0,
      makeBinary(Op::Add, x, makeBinary(Op::Shl, x, makeConstant(1)))};

  const BackendResult multiplyResult = backend.evaluate(multiply);
  const BackendResult shiftAddResult = backend.evaluate(shiftAdd);
  expect(multiply.staticCost < shiftAdd.staticCost,
         "static model should prefer the single multiply");
  expect(shiftAddResult.objective < multiplyResult.objective,
         "backend model should prefer the parallel shift-add form");
}

void testBackendFoldingAndCse() {
  BackendConfig config;
  config.width = 8;
  const adaptive_superopt::TargetBackend backend(config);
  const ExpressionPtr x = makeVariable(0);
  const ExpressionPtr term = makeBinary(Op::Add, x, makeConstant(1));

  const Candidate folded{"folded", "folding", 3.0, {1.0}, true, 1.0,
                         makeBinary(Op::Mul,
                                    makeBinary(Op::Add, x, makeConstant(0)),
                                    makeConstant(1))};
  const Candidate cse{"cse", "cse", 4.0, {1.0}, true, 1.0,
                      makeBinary(Op::Add, term, term)};
  const Candidate expanded{"expanded", "expanded", 4.0, {1.0}, true, 1.0,
                           makeBinary(
                               Op::Add,
                               makeBinary(Op::Add, x, makeConstant(1)),
                               makeBinary(
                                   Op::Add,
                                   makeBinary(Op::Add, x, makeConstant(0)),
                                   makeConstant(1)))};

  const BackendResult foldedResult = backend.evaluate(folded);
  const BackendResult cseResult = backend.evaluate(cse);
  const BackendResult expandedResult = backend.evaluate(expanded);
  expect(foldedResult.objective == 1.0,
         "backend should fold neutral arithmetic patterns");
  expect(cseResult.codeSizeBytes < expandedResult.codeSizeBytes,
         "backend CSE should reduce repeated subexpressions");
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
  config.backend.width = 4;
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
  testBitVectorWraparoundAndMultiVariableVerification();
  testVerificationBudgetAndInvalidExpressions();
  testGeneratorBoundsAndCanonicalization();
  testSelectorBoundaryRejections();
  testBackendCacheAndUnsupportedOperation();
  testStaticAndBackendRankingsCanDisagree();
  testBackendFoldingAndCse();
  testCompleteSearchPipeline();
  std::cout << "all selector tests passed\n";
  return EXIT_SUCCESS;
}
