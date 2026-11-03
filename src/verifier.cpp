#include "adaptive_superopt/verifier.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace adaptive_superopt {

ExhaustiveVerifier::ExhaustiveVerifier(VerificationConfig config)
    : config_(config) {
  if (config_.width == 0 || config_.width > 32) {
    throw std::invalid_argument("bit-vector width must be between 1 and 32");
  }
  if (config_.maxCases == 0) {
    throw std::invalid_argument("maxCases must be positive");
  }
}

VerificationResult ExhaustiveVerifier::verify(
    const ExpressionPtr& reference, const ExpressionPtr& candidate) const {
  VerificationResult result;
  if (!reference || !candidate) {
    result.diagnostic = "reference and candidate must not be null";
    return result;
  }

  std::uint64_t domain = std::uint64_t{1} << config_.width;
  if (config_.width == 32) {
    domain = std::numeric_limits<std::uint64_t>::max();
  }

  std::uint64_t totalCases = 1;
  for (std::size_t variable = 0; variable < config_.variableCount;
       ++variable) {
    if (totalCases > config_.maxCases / domain) {
      result.diagnostic = "input space exceeds the exhaustive verification budget";
      return result;
    }
    totalCases *= domain;
  }
  if (totalCases > config_.maxCases) {
    result.diagnostic = "input space exceeds the exhaustive verification budget";
    return result;
  }

  std::vector<std::uint32_t> inputs(config_.variableCount, 0);
  bool equivalent = true;
  std::function<void(std::size_t)> visit = [&](std::size_t variable) {
    if (!equivalent) {
      return;
    }
    if (variable == config_.variableCount) {
      ++result.casesChecked;
      if (evaluate(reference, inputs, config_.width) !=
          evaluate(candidate, inputs, config_.width)) {
        equivalent = false;
      }
      return;
    }
    for (std::uint64_t value = 0; value < domain && equivalent; ++value) {
      inputs[variable] = static_cast<std::uint32_t>(value);
      visit(variable + 1);
    }
  };

  try {
    visit(0);
  } catch (const std::exception& error) {
    result.diagnostic = error.what();
    return result;
  }

  result.status = equivalent ? VerificationStatus::Equivalent
                             : VerificationStatus::NotEquivalent;
  if (!equivalent) {
    result.diagnostic = "counterexample found during exhaustive evaluation";
  }
  return result;
}

}  // namespace adaptive_superopt
