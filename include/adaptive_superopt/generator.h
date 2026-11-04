#pragma once

#include "adaptive_superopt/expression.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adaptive_superopt {

struct GeneratorConfig {
  unsigned width = 8;
  std::size_t variableCount = 1;
  std::size_t maxDepth = 2;
  std::size_t maxExpressions = 1000;
  std::vector<std::uint32_t> constants;
  std::vector<ExpressionPtr> seedExpressions;
  std::vector<Op> operations = {Op::Add, Op::Sub, Op::Mul, Op::UDiv,
                                Op::Xor, Op::And, Op::Or, Op::Shl};
};

class ExpressionGenerator {
 public:
  explicit ExpressionGenerator(GeneratorConfig config = {});

  std::vector<ExpressionPtr> generate() const;

 private:
  GeneratorConfig config_;
};

}  // namespace adaptive_superopt
