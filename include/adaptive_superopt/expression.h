#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adaptive_superopt {

struct Expression;
using ExpressionPtr = std::shared_ptr<const Expression>;

enum class Op {
  Variable,
  Constant,
  Add,
  Sub,
  Xor,
  And,
  Shl,
};

struct Expression {
  Op op = Op::Constant;
  std::uint32_t value = 0;
  ExpressionPtr lhs;
  ExpressionPtr rhs;
};

ExpressionPtr makeVariable(std::size_t index);
ExpressionPtr makeConstant(std::uint32_t value);
ExpressionPtr makeBinary(Op op, ExpressionPtr lhs, ExpressionPtr rhs);

std::uint32_t evaluate(const ExpressionPtr& expression,
                       const std::vector<std::uint32_t>& inputs,
                       unsigned width);

std::string toString(const ExpressionPtr& expression);
std::string opName(Op op);

std::size_t nodeCount(const ExpressionPtr& expression);
std::size_t expressionDepth(const ExpressionPtr& expression);
std::size_t operationCount(const ExpressionPtr& expression, Op operation);

}  // namespace adaptive_superopt
