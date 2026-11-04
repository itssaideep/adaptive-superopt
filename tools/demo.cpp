#include "adaptive_superopt/superoptimizer.h"

#include "adaptive_superopt/expression.h"

#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

using adaptive_superopt::BackendResult;
using adaptive_superopt::ExpressionPtr;
using adaptive_superopt::Op;
using adaptive_superopt::SearchConfig;
using adaptive_superopt::SelectionResult;
using adaptive_superopt::Superoptimizer;
using adaptive_superopt::makeBinary;
using adaptive_superopt::makeConstant;
using adaptive_superopt::makeVariable;
using adaptive_superopt::toString;

namespace {

struct Options {
  unsigned width = 4;
  std::size_t depth = 2;
  std::size_t maxCandidates = 500;
  std::size_t budget = 5;
};

std::size_t parseSize(const std::string& value, const std::string& option) {
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed == 0) {
      throw std::invalid_argument("invalid value");
    }
    return static_cast<std::size_t>(parsed);
  } catch (const std::exception&) {
    throw std::invalid_argument(option + " requires a positive integer");
  }
}

void printUsage(const char* executable) {
  std::cout << "usage: " << executable
            << " [--width N] [--depth N] [--max-candidates N] [--budget N]\n";
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--help") {
      printUsage(argv[0]);
      std::exit(EXIT_SUCCESS);
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument(option + " requires a value");
    }
    const std::string value = argv[++index];
    if (option == "--width") {
      options.width = static_cast<unsigned>(parseSize(value, option));
    } else if (option == "--depth") {
      options.depth = parseSize(value, option);
    } else if (option == "--max-candidates") {
      options.maxCandidates = parseSize(value, option);
    } else if (option == "--budget") {
      options.budget = parseSize(value, option);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  return options;
}

void printRun(const std::string& name, const SelectionResult& run) {
  std::cout << name << ": calls=" << run.backendInvocations
            << ", evaluated=" << run.evaluations.size();
  if (!run.best.has_value()) {
    std::cout << ", best=none\n";
    return;
  }
  const BackendResult& backend = *run.bestBackendResult;
  std::cout << ", best=" << run.best->id << ", objective="
            << std::fixed << std::setprecision(2) << backend.objective
            << ", cycles=" << backend.executionCycles
            << ", bytes=" << backend.codeSizeBytes << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parseOptions(argc, argv);
    if (options.width > 32) {
      throw std::invalid_argument("--width must be between 1 and 32");
    }
    const ExpressionPtr x = makeVariable(0);
    const std::uint32_t mask =
        options.width == 32 ? 0xffffffffu
                            : (std::uint32_t{1} << options.width) - 1u;
    const ExpressionPtr masked =
        makeBinary(Op::And, x, makeConstant(mask));
    const ExpressionPtr reference = makeBinary(Op::Add, masked, masked);

    SearchConfig config;
    config.generator.width = options.width;
    config.generator.variableCount = 1;
    config.generator.maxDepth = options.depth;
    config.generator.maxExpressions = options.maxCandidates;
    config.verifier.width = options.width;
    config.verifier.variableCount = 1;
    config.selector.backendBudget = options.budget;
    config.selector.seedCandidates = 1;
    config.selector.exploration = 1.0;
    config.selector.timePenalty = 0.1;
    config.backend.lanes = 2;
    config.backend.width = options.width;
    config.backend.codeSizeWeight = 0.02;
    config.backend.latency[Op::Shl] = 3.0;

    const auto result = Superoptimizer::run(reference, config);
    std::cout << "reference: " << toString(reference) << '\n'
              << "generated: " << result.generatedCandidates << '\n'
              << "verified: " << result.verifiedCandidates << '\n'
              << "reference objective: " << result.referenceBackend.objective
              << "\n";
    printRun("adaptive", result.adaptive);
    printRun("static", result.staticBaseline);
    printRun("exhaustive", result.exhaustive);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
