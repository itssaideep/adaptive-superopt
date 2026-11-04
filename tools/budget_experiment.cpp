#include "adaptive_superopt/backend.h"
#include "adaptive_superopt/expression.h"
#include "adaptive_superopt/superoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using adaptive_superopt::ExpressionPtr;
using adaptive_superopt::Op;
using adaptive_superopt::SearchConfig;
using adaptive_superopt::SearchResult;
using adaptive_superopt::Superoptimizer;
using adaptive_superopt::makeBinary;
using adaptive_superopt::makeConstant;
using adaptive_superopt::makeVariable;
using adaptive_superopt::toString;

namespace {

struct Benchmark {
  std::string name;
  std::string category;
  ExpressionPtr reference;
  std::vector<ExpressionPtr> seeds;
};

std::string csv(const std::string& value) {
  std::string escaped = "\"";
  for (const char character : value) {
    if (character == '"') {
      escaped += "\"\"";
    } else {
      escaped += character;
    }
  }
  escaped += '"';
  return escaped;
}

double improvementPercent(const SearchResult& result,
                          const adaptive_superopt::SelectionResult& selection) {
  if (!selection.best.has_value()) {
    return 0.0;
  }
  return 100.0 *
         (result.referenceBackend.objective -
          selection.bestBackendResult->objective) /
         result.referenceBackend.objective;
}

ExpressionPtr binary(Op op, ExpressionPtr lhs, ExpressionPtr rhs) {
  return makeBinary(op, std::move(lhs), std::move(rhs));
}

std::vector<ExpressionPtr> identitySeeds(const ExpressionPtr& x) {
  const ExpressionPtr zero = makeConstant(0);
  const ExpressionPtr one = makeConstant(1);
  const ExpressionPtr mask = makeConstant(255);
  return {
      x,
      binary(Op::Add, x, zero),
      binary(Op::Sub, x, zero),
      binary(Op::Mul, x, one),
      binary(Op::UDiv, x, one),
      binary(Op::Xor, x, zero),
      binary(Op::And, x, mask),
      binary(Op::Or, x, zero),
      binary(Op::Shl, x, zero),
      binary(Op::Mul, binary(Op::Add, x, zero), one),
      binary(Op::Add, binary(Op::Mul, x, one), zero),
      binary(Op::Sub, binary(Op::Add, x, zero), zero),
      binary(Op::Or, binary(Op::And, x, mask), zero),
      binary(Op::Xor, binary(Op::And, x, mask), zero),
      binary(Op::Mul, binary(Op::Xor, x, zero), one),
      binary(Op::Add, binary(Op::Shl, x, zero), zero),
      binary(Op::Sub, binary(Op::Mul, x, one), zero),
      binary(Op::UDiv, binary(Op::Mul, x, one), one),
      binary(Op::Shl, binary(Op::Add, x, zero), zero),
      binary(Op::Add, x, binary(Op::Sub, x, x)),
      binary(Op::Mul, binary(Op::And, x, mask), one),
      binary(Op::Or, binary(Op::Xor, x, zero), zero),
  };
}

std::vector<ExpressionPtr> maskedAddSeeds(const ExpressionPtr& x,
                                           std::uint32_t maskValue) {
  const ExpressionPtr zero = makeConstant(0);
  const ExpressionPtr one = makeConstant(1);
  const ExpressionPtr two = makeConstant(2);
  const ExpressionPtr four = makeConstant(4);
  const ExpressionPtr mask = makeConstant(maskValue);
  const ExpressionPtr m = binary(Op::And, x, mask);
  const ExpressionPtr doubled = binary(Op::Add, m, m);
  return {
      doubled,
      binary(Op::Shl, m, one),
      binary(Op::Mul, m, two),
      binary(Op::Add, binary(Op::Add, m, zero), m),
      binary(Op::Add, m, binary(Op::Add, m, zero)),
      binary(Op::Add, binary(Op::Mul, m, one), binary(Op::Mul, m, one)),
      binary(Op::Add, binary(Op::Shl, m, zero), binary(Op::Shl, m, zero)),
      binary(Op::Add, binary(Op::Xor, m, zero), binary(Op::Xor, m, zero)),
      binary(Op::Add, binary(Op::Or, m, zero), binary(Op::Or, m, zero)),
      binary(Op::Add, binary(Op::And, m, makeConstant(255)),
             binary(Op::And, m, makeConstant(255))),
      binary(Op::Add, binary(Op::Add, m, one), binary(Op::Sub, m, one)),
      binary(Op::UDiv, binary(Op::Mul, m, makeConstant(4)), two),
      binary(Op::UDiv, binary(Op::Shl, m, two), two),
      binary(Op::Add, binary(Op::Mul, m, two), zero),
      binary(Op::Sub, binary(Op::Mul, m, two), zero),
      binary(Op::Add, binary(Op::Shl, m, one), zero),
      binary(Op::Sub, binary(Op::Shl, m, one), zero),
      binary(Op::Add, m, binary(Op::Mul, m, one)),
      binary(Op::Add, binary(Op::Mul, m, one), m),
      binary(Op::Add, m, binary(Op::Sub, m, zero)),
      binary(Op::Add, m, binary(Op::And, m, makeConstant(255))),
      binary(Op::UDiv, binary(Op::Mul, m, four), two),
  };
}

std::vector<ExpressionPtr> scaledAddSeeds(const ExpressionPtr& x,
                                           std::uint32_t lhsFactor,
                                           std::uint32_t rhsFactor) {
  const ExpressionPtr zero = makeConstant(0);
  const ExpressionPtr one = makeConstant(1);
  const std::uint32_t sum = lhsFactor + rhsFactor;
  const ExpressionPtr lhs = makeConstant(lhsFactor);
  const ExpressionPtr rhs = makeConstant(rhsFactor);
  const ExpressionPtr total = makeConstant(sum);
  const ExpressionPtr lhsProduct = binary(Op::Mul, x, lhs);
  const ExpressionPtr rhsProduct = binary(Op::Mul, x, rhs);
  const ExpressionPtr reference = binary(Op::Add, lhsProduct, rhsProduct);
  return {
      reference,
      binary(Op::Mul, x, total),
      binary(Op::Add, rhsProduct, lhsProduct),
      binary(Op::Add, reference, zero),
      binary(Op::Sub, binary(Op::Mul, x, total), zero),
      binary(Op::Mul, binary(Op::Add, x, zero), total),
      binary(Op::Mul, binary(Op::Sub, x, zero), total),
      binary(Op::Add, binary(Op::Mul, x, total), zero),
      binary(Op::Add, binary(Op::Mul, x, makeConstant(sum - 1)), x),
      binary(Op::Sub, binary(Op::Mul, x, makeConstant(sum + 1)), x),
      binary(Op::Add, binary(Op::Mul, x, lhs),
             binary(Op::Add, rhsProduct, zero)),
      binary(Op::Add, binary(Op::Add, lhsProduct, zero), rhsProduct),
      binary(Op::Add, binary(Op::Mul, x, one),
             binary(Op::Mul, x, makeConstant(sum - 1))),
      binary(Op::Mul, binary(Op::Mul, x, one), total),
      binary(Op::Add, binary(Op::Mul, x, lhs),
             binary(Op::Mul, x, rhs)),
      binary(Op::Sub, binary(Op::Mul, x, total),
             binary(Op::Mul, x, zero)),
      binary(Op::Add, binary(Op::Mul, x, total),
             binary(Op::Sub, x, x)),
  };
}

std::vector<ExpressionPtr> scaledDivideSeeds(const ExpressionPtr& x,
                                               std::uint32_t factor,
                                               std::uint32_t divisor) {
  const ExpressionPtr zero = makeConstant(0);
  const ExpressionPtr one = makeConstant(1);
  const ExpressionPtr factorConstant = makeConstant(factor);
  const ExpressionPtr divisorConstant = makeConstant(divisor);
  const ExpressionPtr numerator = binary(Op::Mul, x, factorConstant);
  const ExpressionPtr zeroFromX = binary(Op::Sub, x, x);
  return {
      binary(Op::UDiv, numerator, divisorConstant),
      binary(Op::UDiv, binary(Op::Add, numerator, zero), divisorConstant),
      binary(Op::UDiv, binary(Op::Mul, numerator, one), divisorConstant),
      binary(Op::UDiv, binary(Op::Xor, numerator, zero), divisorConstant),
      binary(Op::UDiv, binary(Op::Or, numerator, zero), divisorConstant),
      binary(Op::UDiv, binary(Op::Sub, numerator, zero), divisorConstant),
      binary(Op::UDiv, binary(Op::Add, numerator, zeroFromX), divisorConstant),
      binary(Op::UDiv, binary(Op::Mul, binary(Op::Add, x, zero), factorConstant),
             divisorConstant),
      binary(Op::UDiv, binary(Op::Mul, binary(Op::Sub, x, zero), factorConstant),
             divisorConstant),
      binary(Op::UDiv, binary(Op::Mul, binary(Op::Mul, x, one), factorConstant),
             divisorConstant),
      binary(Op::UDiv, binary(Op::And, numerator, makeConstant(255)),
             divisorConstant),
      binary(Op::UDiv, binary(Op::Or, numerator, zeroFromX), divisorConstant),
      binary(Op::UDiv, binary(Op::Xor, numerator, zeroFromX), divisorConstant),
      binary(Op::UDiv, binary(Op::Add, numerator, binary(Op::Sub, zero, zero)),
             divisorConstant),
  };
}

std::vector<ExpressionPtr> deceptiveMultiplySeeds(const ExpressionPtr& x,
                                                    std::uint32_t factor) {
  const ExpressionPtr zero = makeConstant(0);
  const ExpressionPtr reference = binary(Op::Mul, x, makeConstant(factor));
  std::vector<ExpressionPtr> terms;
  for (unsigned bit = 0; bit < 8; ++bit) {
    if ((factor & (1u << bit)) == 0) {
      continue;
    }
    terms.push_back(bit == 0
                        ? x
                        : binary(Op::Shl, x, makeConstant(1u << bit)));
  }
  ExpressionPtr shiftAdd = terms.front();
  for (std::size_t index = 1; index < terms.size(); ++index) {
    shiftAdd = binary(Op::Add, shiftAdd, terms[index]);
  }
  return {
      reference,
      shiftAdd,
      binary(Op::Add, shiftAdd, zero),
      binary(Op::Sub, shiftAdd, zero),
      binary(Op::Add, binary(Op::Mul, x, makeConstant(factor - 1)), x),
      binary(Op::Sub, binary(Op::Mul, x, makeConstant(factor + 1)), x),
      binary(Op::Add, binary(Op::Xor, shiftAdd, zero), zero),
      binary(Op::Mul, binary(Op::Add, x, zero), makeConstant(factor)),
      binary(Op::Mul, binary(Op::Sub, x, zero), makeConstant(factor)),
      binary(Op::Add, reference, binary(Op::Sub, x, x)),
      binary(Op::Sub, reference, binary(Op::Sub, x, x)),
  };
}

bool isGlobalOptimum(const SearchResult& result,
                     const adaptive_superopt::SelectionResult& selection) {
  return selection.best.has_value() && result.exhaustive.best.has_value() &&
         std::abs(selection.bestBackendResult->objective -
                  result.exhaustive.bestBackendResult->objective) < 1e-9;
}

void writeRow(const Benchmark& benchmark, std::size_t budget,
              const std::string& method, const SearchResult& result,
              const adaptive_superopt::SelectionResult& selection,
              double runtimeMs) {
  const std::string best =
      selection.best.has_value() ? selection.best->id : "";
  const double objective = selection.best.has_value()
                               ? selection.bestBackendResult->objective
                               : 0.0;
  std::cout << csv(benchmark.name) << ',' << csv(benchmark.category) << ','
            << csv(method) << ',' << budget
            << ',' << result.generatedCandidates << ','
            << result.verifiedCandidates << ',' << selection.backendInvocations
            << ',' << std::fixed << std::setprecision(4) << runtimeMs << ','
            << std::setprecision(6) << objective << ','
            << improvementPercent(result, selection) << ','
            << (isGlobalOptimum(result, selection) ? "true" : "false")
            << ',' << csv(best) << '\n';
}

void writeStaticAccuracy(std::ofstream& output, const Benchmark& benchmark,
                         const SearchResult& result,
                         const adaptive_superopt::BackendConfig& backendConfig) {
  adaptive_superopt::TargetBackend backend(backendConfig);
  std::vector<adaptive_superopt::BackendResult> backendResults;
  backendResults.reserve(result.candidates.size());
  for (const auto& candidate : result.candidates) {
    backendResults.push_back(backend.evaluate(candidate));
  }

  const auto staticBefore = [&](std::size_t lhs, std::size_t rhs) {
    if (result.candidates[lhs].staticCost != result.candidates[rhs].staticCost) {
      return result.candidates[lhs].staticCost < result.candidates[rhs].staticCost;
    }
    return result.candidates[lhs].id < result.candidates[rhs].id;
  };
  const auto backendBefore = [&](std::size_t lhs, std::size_t rhs) {
    if (backendResults[lhs].success != backendResults[rhs].success) {
      return backendResults[lhs].success;
    }
    if (!backendResults[lhs].success) {
      return result.candidates[lhs].id < result.candidates[rhs].id;
    }
    if (backendResults[lhs].objective != backendResults[rhs].objective) {
      return backendResults[lhs].objective < backendResults[rhs].objective;
    }
    return result.candidates[lhs].id < result.candidates[rhs].id;
  };

  std::vector<std::size_t> staticOrder(result.candidates.size());
  std::vector<std::size_t> backendOrder(result.candidates.size());
  for (std::size_t index = 0; index < result.candidates.size(); ++index) {
    staticOrder[index] = index;
    backendOrder[index] = index;
  }
  std::stable_sort(staticOrder.begin(), staticOrder.end(), staticBefore);
  std::stable_sort(backendOrder.begin(), backendOrder.end(), backendBefore);

  std::vector<std::size_t> staticRank(result.candidates.size());
  std::vector<std::size_t> backendRank(result.candidates.size());
  for (std::size_t rank = 0; rank < staticOrder.size(); ++rank) {
    staticRank[staticOrder[rank]] = rank + 1;
    backendRank[backendOrder[rank]] = rank + 1;
  }

  for (std::size_t index = 0; index < result.candidates.size(); ++index) {
    const auto& candidate = result.candidates[index];
    const auto& backendResult = backendResults[index];
    const double absoluteError =
        backendResult.success
            ? std::abs(candidate.staticCost - backendResult.objective)
            : 0.0;
    const double relativeError =
        backendResult.success && backendResult.objective != 0.0
            ? absoluteError / std::abs(backendResult.objective)
            : 0.0;
    output << csv(benchmark.name) << ',' << csv(benchmark.category) << ','
           << csv(candidate.id) << ',' << std::fixed << std::setprecision(6)
           << candidate.staticCost << ','
           << (backendResult.success ? "true" : "false") << ','
           << (backendResult.success ? backendResult.objective : 0.0) << ','
           << absoluteError << ',' << relativeError << ','
           << staticRank[index] << ',' << backendRank[index] << ','
           << (staticRank[index] > backendRank[index]
                   ? staticRank[index] - backendRank[index]
                   : backendRank[index] - staticRank[index])
           << '\n';
  }
}

std::vector<Benchmark> benchmarks() {
  const ExpressionPtr x = makeVariable(0);
  std::vector<Benchmark> suite;
  suite.push_back({
      "masked_add", "masking",
      makeBinary(Op::Add, makeBinary(Op::And, x, makeConstant(15)),
                 makeBinary(Op::And, x, makeConstant(15))),
      maskedAddSeeds(x, 15)});
  suite.push_back({
      "scaled_add", "strength_reduction",
      makeBinary(Op::Add, makeBinary(Op::Mul, x, makeConstant(2)),
                 makeBinary(Op::Mul, x, makeConstant(4))),
      scaledAddSeeds(x, 2, 4)});
  suite.push_back({
      "scaled_divide", "division",
      makeBinary(Op::UDiv, makeBinary(Op::Mul, x, makeConstant(8)),
                 makeConstant(4)),
      scaledDivideSeeds(x, 8, 4)});
  suite.push_back({
      "identity", "easy",
      makeBinary(Op::Mul, makeBinary(Op::Add, x, makeConstant(0)),
                 makeConstant(1)),
      identitySeeds(x)});
  suite.push_back({
      "masked_or", "masking",
      makeBinary(Op::Or, makeBinary(Op::And, x, makeConstant(255)),
                 makeBinary(Op::And, x, makeConstant(15))),
      identitySeeds(x)});

  const std::vector<std::uint32_t> masks = {3, 7, 15, 31, 63};
  for (const std::uint32_t mask : masks) {
    const ExpressionPtr masked = binary(Op::And, x, makeConstant(mask));
    suite.push_back({"masked_add_" + std::to_string(mask), "masking",
                     binary(Op::Add, masked, masked),
                     maskedAddSeeds(x, mask)});
  }

  const std::vector<std::pair<std::uint32_t, std::uint32_t>> scalePairs = {
      {1, 2}, {2, 4}, {3, 5}, {4, 8}, {5, 7}};
  for (const auto& pair : scalePairs) {
    suite.push_back({
        "scaled_add_" + std::to_string(pair.first) + "_" +
            std::to_string(pair.second),
        "strength_reduction",
        binary(Op::Add, binary(Op::Mul, x, makeConstant(pair.first)),
               binary(Op::Mul, x, makeConstant(pair.second))),
        scaledAddSeeds(x, pair.first, pair.second)});
  }

  const std::vector<std::uint32_t> deceptiveFactors = {3, 5, 7, 9, 11};
  for (const std::uint32_t factor : deceptiveFactors) {
    suite.push_back({"deceptive_mul_" + std::to_string(factor),
                     "deceptive_backend_ranking",
                     binary(Op::Mul, x, makeConstant(factor)),
                     deceptiveMultiplySeeds(x, factor)});
  }

  for (std::size_t variant = 0; variant < 5; ++variant) {
    ExpressionPtr reference = x;
    for (std::size_t step = 0; step <= variant; ++step) {
      reference = binary(
          step % 2 == 0 ? Op::Add : Op::Xor, reference,
          makeConstant(0));
    }
    suite.push_back({"identity_variant_" + std::to_string(variant), "easy",
                     reference, identitySeeds(x)});
  }

  const std::vector<std::pair<std::uint32_t, std::uint32_t>> dividePairs = {
      {4, 2}, {8, 2}, {8, 4}, {12, 4}, {16, 4}};
  for (const auto& pair : dividePairs) {
    suite.push_back({
        "scaled_divide_" + std::to_string(pair.first) + "_" +
            std::to_string(pair.second),
        "division",
        binary(Op::UDiv, binary(Op::Mul, x, makeConstant(pair.first)),
               makeConstant(pair.second)),
        scaledDivideSeeds(x, pair.first, pair.second)});
  }
  return suite;
}

}  // namespace

int main() {
  try {
    std::filesystem::create_directories("results");
    const std::vector<std::size_t> budgets = {1, 2, 3, 5, 10, 20};
    const std::vector<Benchmark> suite = benchmarks();
    std::ofstream staticAccuracy("results/static_accuracy.csv");
    if (!staticAccuracy) {
      throw std::runtime_error("could not create results/static_accuracy.csv");
    }
    staticAccuracy
        << "benchmark,category,candidate,static_cost,backend_success,"
           "backend_objective,absolute_error,relative_error,static_rank,"
           "backend_rank,rank_difference\n";
    std::cout << "benchmark,category,method,budget,generated_candidates,"
                 "verified_candidates,backend_calls,runtime_ms,best_objective,"
                 "improvement_percent,global_optimum,best_candidate\n";

    for (const Benchmark& benchmark : suite) {
      std::vector<std::string> expectedCandidates;
      bool first = true;
      for (const std::size_t budget : budgets) {
        SearchConfig config;
        config.generator.width = 8;
        config.generator.variableCount = 1;
        config.generator.maxDepth = 3;
        config.generator.maxExpressions = 500;
        config.generator.constants = {0, 1, 2, 4, 8, 15, 255};
        config.generator.seedExpressions = benchmark.seeds;
        config.verifier.width = 8;
        config.verifier.variableCount = 1;
        config.verifier.maxCases = 256;
        config.backend.width = 8;
        config.backend.latency[Op::Mul] = 3.0;
        config.backend.latency[Op::Shl] = 1.0;
        config.selector.backendBudget = budget;
        config.selector.seedCandidates = 2;
        config.selector.exploration = 1.0;
        config.selector.timePenalty = 0.1;

        const SearchResult result =
            Superoptimizer::run(benchmark.reference, config);
        std::vector<std::string> actualCandidates;
        for (const auto& candidate : result.candidates) {
          actualCandidates.push_back(candidate.id);
        }
        if (first) {
          expectedCandidates = actualCandidates;
          writeStaticAccuracy(staticAccuracy, benchmark, result,
                              config.backend);
          first = false;
        } else if (actualCandidates != expectedCandidates) {
          throw std::runtime_error(
              "candidate set changed between budget experiments");
        }

        writeRow(benchmark, budget, "adaptive", result, result.adaptive,
                 result.adaptiveRuntimeMs);
        writeRow(benchmark, budget, "static", result, result.staticBaseline,
                 result.staticRuntimeMs);
        writeRow(benchmark, budget, "exhaustive", result, result.exhaustive,
                 result.exhaustiveRuntimeMs);
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
