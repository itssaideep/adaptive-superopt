#pragma once

#include "adaptive_superopt/expression.h"

#include <cstddef>
#include <string>

namespace adaptive_superopt {

enum class VerificationStatus {
  Equivalent,
  NotEquivalent,
  Rejected,
};

struct VerificationConfig {
  unsigned width = 8;
  std::size_t variableCount = 1;
  std::size_t maxCases = 1u << 20;
};

struct VerificationResult {
  VerificationStatus status = VerificationStatus::Rejected;
  std::size_t casesChecked = 0;
  std::string diagnostic;

  bool equivalent() const { return status == VerificationStatus::Equivalent; }
};

class ExhaustiveVerifier {
 public:
  explicit ExhaustiveVerifier(VerificationConfig config = {});

  VerificationResult verify(const ExpressionPtr& reference,
                            const ExpressionPtr& candidate) const;

 private:
  VerificationConfig config_;
};

}  // namespace adaptive_superopt
