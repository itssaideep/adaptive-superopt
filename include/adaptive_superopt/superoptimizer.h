#pragma once

#include "adaptive_superopt/backend.h"
#include "adaptive_superopt/generator.h"
#include "adaptive_superopt/verifier.h"

#include <cstddef>
#include <chrono>
#include <vector>

namespace adaptive_superopt {

struct SearchConfig {
  GeneratorConfig generator;
  VerificationConfig verifier;
  SelectorConfig selector;
  BackendConfig backend;
};

struct SearchResult {
  Candidate reference;
  BackendResult referenceBackend;
  std::size_t generatedCandidates = 0;
  std::size_t verifiedCandidates = 0;
  std::vector<Candidate> candidates;
  SelectionResult adaptive;
  SelectionResult staticBaseline;
  SelectionResult exhaustive;
  double adaptiveRuntimeMs = 0.0;
  double staticRuntimeMs = 0.0;
  double exhaustiveRuntimeMs = 0.0;
  std::size_t adaptiveCacheHits = 0;
  std::size_t staticCacheHits = 0;
  std::size_t exhaustiveCacheHits = 0;
};

class Superoptimizer {
 public:
  static SearchResult run(const ExpressionPtr& reference,
                          SearchConfig config = {});
};

}  // namespace adaptive_superopt
