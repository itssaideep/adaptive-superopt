#include "adaptive_superopt/generator.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace adaptive_superopt {
namespace {

bool isCommutative(Op op) {
  return op == Op::Add || op == Op::Mul || op == Op::Xor || op == Op::And ||
         op == Op::Or;
}

std::uint32_t widthMask(unsigned width) {
  if (width == 32) {
    return 0xffffffffu;
  }
  return (std::uint32_t{1} << width) - 1u;
}

}  // namespace

ExpressionGenerator::ExpressionGenerator(GeneratorConfig config)
    : config_(std::move(config)) {
  if (config_.width == 0 || config_.width > 32) {
    throw std::invalid_argument("bit-vector width must be between 1 and 32");
  }
  if (config_.maxExpressions == 0) {
    throw std::invalid_argument("maxExpressions must be positive");
  }
}

std::vector<ExpressionPtr> ExpressionGenerator::generate() const {
  std::vector<std::vector<ExpressionPtr>> levels(config_.maxDepth + 1);
  std::vector<ExpressionPtr> all;
  std::unordered_set<std::string> seen;

  const auto add = [&](const ExpressionPtr& expression) {
    if (all.size() >= config_.maxExpressions) {
      return false;
    }
    const std::string key = toString(expression);
    if (!seen.insert(key).second) {
      return true;
    }
    all.push_back(expression);
    const std::size_t depth = expressionDepth(expression);
    if (depth >= levels.size()) {
      levels.resize(depth + 1);
    }
    levels[depth].push_back(expression);
    return true;
  };

  for (std::size_t variable = 0; variable < config_.variableCount;
       ++variable) {
    if (!add(makeVariable(variable))) {
      return all;
    }
  }

  std::vector<std::uint32_t> constants = config_.constants;
  if (constants.empty()) {
    constants = {0u, 1u, 2u};
    if (config_.width < 32) {
      constants.push_back(widthMask(config_.width));
    }
  }
  for (const std::uint32_t constant : constants) {
    if (!add(makeConstant(constant & widthMask(config_.width)))) {
      return all;
    }
  }

  // Seed expressions make it possible to evaluate a known rewrite corpus
  // without relying on the grammar's enumeration order to reach every
  // interesting identity before the candidate limit.
  for (const ExpressionPtr& seed : config_.seedExpressions) {
    if (!seed) {
      throw std::invalid_argument("seed expressions must not be null");
    }
    if (!add(seed)) {
      return all;
    }
  }

  for (std::size_t depth = 1; depth <= config_.maxDepth; ++depth) {
    bool reachedLimit = false;
    for (std::size_t lhsDepth = 0; lhsDepth < depth && !reachedLimit;
         ++lhsDepth) {
      for (std::size_t rhsDepth = 0; rhsDepth < depth && !reachedLimit;
           ++rhsDepth) {
        if (std::max(lhsDepth, rhsDepth) + 1 != depth) {
          continue;
        }
        for (const ExpressionPtr& lhs : levels[lhsDepth]) {
          for (const ExpressionPtr& rhs : levels[rhsDepth]) {
            for (const Op operation : config_.operations) {
              if ((operation == Op::Shl || operation == Op::UDiv) &&
                  rhs->op != Op::Constant) {
                continue;
              }
              if (operation == Op::UDiv && rhs->value == 0) {
                continue;
              }
              if (isCommutative(operation) &&
                  toString(lhs) > toString(rhs)) {
                continue;
              }
              const ExpressionPtr expression =
                  makeBinary(operation, lhs, rhs);
              if (!add(expression)) {
                reachedLimit = true;
                break;
              }
            }
            if (reachedLimit) {
              break;
            }
          }
          if (reachedLimit) {
            break;
          }
        }
      }
    }
    if (reachedLimit) {
      break;
    }
  }
  return all;
}

}  // namespace adaptive_superopt
