#include "adaptive_superopt/expression.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace adaptive_superopt {
namespace {

std::uint32_t maskForWidth(unsigned width) {
  if (width == 0 || width > 32) {
    throw std::invalid_argument("bit-vector width must be between 1 and 32");
  }
  if (width == 32) {
    return 0xffffffffu;
  }
  return (std::uint32_t{1} << width) - 1u;
}

void validateExpression(const ExpressionPtr& expression) {
  if (!expression) {
    throw std::invalid_argument("expression must not be null");
  }
  if (expression->op == Op::Variable || expression->op == Op::Constant) {
    return;
  }
  validateExpression(expression->lhs);
  validateExpression(expression->rhs);
}

std::uint32_t evaluateUnchecked(const ExpressionPtr& expression,
                                const std::vector<std::uint32_t>& inputs,
                                unsigned width) {
  const std::uint32_t mask = maskForWidth(width);
  switch (expression->op) {
    case Op::Variable:
      if (expression->value >= inputs.size()) {
        throw std::out_of_range("expression references a missing variable");
      }
      return inputs[expression->value] & mask;
    case Op::Constant:
      return expression->value & mask;
    case Op::Add:
      return (evaluateUnchecked(expression->lhs, inputs, width) +
              evaluateUnchecked(expression->rhs, inputs, width)) &
             mask;
    case Op::Sub:
      return (evaluateUnchecked(expression->lhs, inputs, width) -
              evaluateUnchecked(expression->rhs, inputs, width)) &
             mask;
    case Op::Xor:
      return evaluateUnchecked(expression->lhs, inputs, width) ^
             evaluateUnchecked(expression->rhs, inputs, width);
    case Op::And:
      return evaluateUnchecked(expression->lhs, inputs, width) &
             evaluateUnchecked(expression->rhs, inputs, width);
    case Op::Shl: {
      const std::uint32_t lhs =
          evaluateUnchecked(expression->lhs, inputs, width);
      const std::uint32_t shift =
          evaluateUnchecked(expression->rhs, inputs, width) % width;
      return (lhs << shift) & mask;
    }
  }
  throw std::logic_error("unknown expression operation");
}

void appendString(const ExpressionPtr& expression, std::ostringstream& output) {
  if (expression->op == Op::Variable) {
    output << 'x' << expression->value;
    return;
  }
  if (expression->op == Op::Constant) {
    output << "0x" << std::hex << expression->value << std::dec;
    return;
  }
  output << '(';
  appendString(expression->lhs, output);
  output << ' ' << opName(expression->op) << ' ';
  appendString(expression->rhs, output);
  output << ')';
}

std::size_t countNodes(const ExpressionPtr& expression) {
  if (expression->op == Op::Variable || expression->op == Op::Constant) {
    return 1;
  }
  return 1 + countNodes(expression->lhs) + countNodes(expression->rhs);
}

std::size_t depth(const ExpressionPtr& expression) {
  if (expression->op == Op::Variable || expression->op == Op::Constant) {
    return 0;
  }
  return 1 + std::max(depth(expression->lhs), depth(expression->rhs));
}

std::size_t countOperation(const ExpressionPtr& expression, Op operation) {
  const std::size_t own = expression->op == operation ? 1u : 0u;
  if (expression->op == Op::Variable || expression->op == Op::Constant) {
    return own;
  }
  return own + countOperation(expression->lhs, operation) +
         countOperation(expression->rhs, operation);
}

}  // namespace

ExpressionPtr makeVariable(std::size_t index) {
  return std::make_shared<const Expression>(
      Expression{Op::Variable, static_cast<std::uint32_t>(index), nullptr,
                  nullptr});
}

ExpressionPtr makeConstant(std::uint32_t value) {
  return std::make_shared<const Expression>(
      Expression{Op::Constant, value, nullptr, nullptr});
}

ExpressionPtr makeBinary(Op op, ExpressionPtr lhs, ExpressionPtr rhs) {
  if (op == Op::Variable || op == Op::Constant) {
    throw std::invalid_argument("binary expression requires a binary operation");
  }
  if (!lhs || !rhs) {
    throw std::invalid_argument("binary expression operands must not be null");
  }
  return std::make_shared<const Expression>(
      Expression{op, 0, std::move(lhs), std::move(rhs)});
}

std::uint32_t evaluate(const ExpressionPtr& expression,
                       const std::vector<std::uint32_t>& inputs,
                       unsigned width) {
  validateExpression(expression);
  return evaluateUnchecked(expression, inputs, width);
}

std::string opName(Op op) {
  switch (op) {
    case Op::Variable:
      return "var";
    case Op::Constant:
      return "const";
    case Op::Add:
      return "+";
    case Op::Sub:
      return "-";
    case Op::Xor:
      return "^";
    case Op::And:
      return "&";
    case Op::Shl:
      return "<<";
  }
  throw std::logic_error("unknown expression operation");
}

std::string toString(const ExpressionPtr& expression) {
  validateExpression(expression);
  std::ostringstream output;
  appendString(expression, output);
  return output.str();
}

std::size_t nodeCount(const ExpressionPtr& expression) {
  validateExpression(expression);
  return countNodes(expression);
}

std::size_t expressionDepth(const ExpressionPtr& expression) {
  validateExpression(expression);
  return depth(expression);
}

std::size_t operationCount(const ExpressionPtr& expression, Op operation) {
  validateExpression(expression);
  return countOperation(expression, operation);
}

}  // namespace adaptive_superopt
