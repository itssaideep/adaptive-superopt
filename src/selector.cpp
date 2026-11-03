#include "adaptive_superopt/selector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace adaptive_superopt {
namespace {

using Matrix = std::vector<std::vector<double>>;

double dot(const std::vector<double>& lhs, const std::vector<double>& rhs) {
  double result = 0.0;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    result += lhs[i] * rhs[i];
  }
  return result;
}

// Small ridge systems are sufficient for the low-dimensional feature vector
// used by the selector. Gaussian elimination keeps the core dependency-free.
std::vector<double> solve(const Matrix& matrix,
                          const std::vector<double>& rhs) {
  const std::size_t n = rhs.size();
  if (matrix.size() != n) {
    throw std::invalid_argument("ridge matrix has an invalid row count");
  }

  Matrix augmented(n, std::vector<double>(n + 1, 0.0));
  for (std::size_t row = 0; row < n; ++row) {
    if (matrix[row].size() != n) {
      throw std::invalid_argument("ridge matrix is not square");
    }
    for (std::size_t column = 0; column < n; ++column) {
      augmented[row][column] = matrix[row][column];
    }
    augmented[row][n] = rhs[row];
  }

  for (std::size_t column = 0; column < n; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1; row < n; ++row) {
      if (std::abs(augmented[row][column]) >
          std::abs(augmented[pivot][column])) {
        pivot = row;
      }
    }

    if (std::abs(augmented[pivot][column]) < 1e-12) {
      throw std::runtime_error("singular ridge system");
    }
    std::swap(augmented[pivot], augmented[column]);

    const double divisor = augmented[column][column];
    for (std::size_t entry = column; entry <= n; ++entry) {
      augmented[column][entry] /= divisor;
    }

    for (std::size_t row = 0; row < n; ++row) {
      if (row == column) {
        continue;
      }
      const double factor = augmented[row][column];
      for (std::size_t entry = column; entry <= n; ++entry) {
        augmented[row][entry] -= factor * augmented[column][entry];
      }
    }
  }

  std::vector<double> result(n, 0.0);
  for (std::size_t row = 0; row < n; ++row) {
    result[row] = augmented[row][n];
  }
  return result;
}

std::vector<std::size_t> staticOrder(const std::vector<Candidate>& candidates) {
  std::vector<std::size_t> order(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs,
                                                   std::size_t rhs) {
    if (candidates[lhs].staticCost != candidates[rhs].staticCost) {
      return candidates[lhs].staticCost < candidates[rhs].staticCost;
    }
    return candidates[lhs].id < candidates[rhs].id;
  });
  return order;
}

std::vector<std::size_t> chooseSeeds(const std::vector<Candidate>& candidates,
                                     std::size_t seedCount) {
  const auto order = staticOrder(candidates);
  std::vector<std::size_t> result;
  std::set<std::string> families;

  // First sample the best candidate from each family. This prevents the
  // static prior from completely hiding a candidate family with a different
  // backend response.
  for (const std::size_t index : order) {
    if (result.size() >= seedCount) {
      break;
    }
    if (families.insert(candidates[index].family).second) {
      result.push_back(index);
    }
  }

  for (const std::size_t index : order) {
    if (result.size() >= seedCount) {
      break;
    }
    if (std::find(result.begin(), result.end(), index) == result.end()) {
      result.push_back(index);
    }
  }
  return result;
}

}  // namespace

AdaptiveSelector::AdaptiveSelector(double baselineStaticCost,
                                   double baselineBackendObjective,
                                   SelectorConfig config)
    : baselineStaticCost_(baselineStaticCost),
      baselineBackendObjective_(baselineBackendObjective),
      config_(config) {
  if (!(baselineStaticCost_ > 0.0) ||
      !(baselineBackendObjective_ > 0.0)) {
    throw std::invalid_argument("baseline costs must be positive");
  }
  if (!(config_.ridge > 0.0)) {
    throw std::invalid_argument("ridge must be positive");
  }
  if (config_.exploration < 0.0 || config_.timePenalty < 0.0 ||
      config_.failurePenalty < 0.0) {
    throw std::invalid_argument("selector penalties must be non-negative");
  }
}

SelectionResult AdaptiveSelector::select(
    const std::vector<Candidate>& candidates,
    const BackendEvaluator& evaluate) const {
  if (!evaluate) {
    throw std::invalid_argument("a backend evaluator is required");
  }

  SelectionResult result;
  if (config_.backendBudget == 0 || candidates.empty()) {
    return result;
  }

  std::size_t featureDimension = 0;
  bool dimensionInitialized = false;
  std::vector<std::size_t> eligible;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const Candidate& candidate = candidates[index];
    if (!candidate.semanticallyVerified) {
      continue;
    }
    if (!dimensionInitialized) {
      featureDimension = candidate.features.size();
      dimensionInitialized = true;
    } else if (candidate.features.size() != featureDimension) {
      throw std::invalid_argument("candidate feature dimensions differ");
    }
    eligible.push_back(index);
  }

  if (eligible.empty()) {
    return result;
  }

  Matrix gram(featureDimension,
              std::vector<double>(featureDimension, 0.0));
  std::vector<double> response(featureDimension, 0.0);
  for (std::size_t i = 0; i < featureDimension; ++i) {
    gram[i][i] = config_.ridge;
  }

  std::vector<bool> evaluated(candidates.size(), false);
  double bestObjective = std::numeric_limits<double>::infinity();
  double bestGain = 0.0;
  bool hasSuccessfulEvaluation = false;

  const auto staticGain = [&](const Candidate& candidate) {
    return (baselineStaticCost_ - candidate.staticCost) /
           baselineStaticCost_;
  };

  const auto modelParameters = [&]() {
    if (featureDimension == 0) {
      return std::vector<double>{};
    }
    return solve(gram, response);
  };

  const auto uncertainty = [&](const Candidate& candidate) {
    if (featureDimension == 0) {
      return 0.0;
    }
    const auto inverseTimesFeature = solve(gram, candidate.features);
    return std::sqrt(std::max(0.0,
                              dot(candidate.features, inverseTimesFeature)));
  };

  auto evaluateIndex = [&](std::size_t index, double predictedGain,
                           double candidateUncertainty, double acquisition) {
    const Candidate& candidate = candidates[index];
    BackendResult backend = evaluate(candidate);
    ++result.backendInvocations;
    evaluated[index] = true;

    const double candidateStaticGain = staticGain(candidate);
    double observedGain = -config_.failurePenalty;
    if (backend.success) {
      if (!(backend.objective > 0.0) ||
          !std::isfinite(backend.objective)) {
        throw std::invalid_argument(
            "successful backend result must have a positive objective");
      }
      observedGain = (baselineBackendObjective_ - backend.objective) /
                     baselineBackendObjective_;
      if (!hasSuccessfulEvaluation || backend.objective < bestObjective) {
        bestObjective = backend.objective;
        bestGain = observedGain;
        hasSuccessfulEvaluation = true;
        result.best = candidate;
        result.bestBackendResult = backend;
      }
    }

    if (featureDimension != 0) {
      const double residual = observedGain - candidateStaticGain;
      for (std::size_t row = 0; row < featureDimension; ++row) {
        response[row] += candidate.features[row] * residual;
        for (std::size_t column = 0; column < featureDimension; ++column) {
          gram[row][column] += candidate.features[row] *
                               candidate.features[column];
        }
      }
    }

    result.evaluations.push_back(EvaluationRecord{
        candidate.id,
        candidateStaticGain,
        predictedGain,
        candidateUncertainty,
        acquisition,
        std::move(backend),
        result.backendInvocations,
    });
  };

  const auto seeds = chooseSeeds(candidates, config_.seedCandidates);
  for (const std::size_t seed : seeds) {
    if (result.backendInvocations >= config_.backendBudget ||
        std::find(eligible.begin(), eligible.end(), seed) == eligible.end()) {
      continue;
    }
    evaluateIndex(seed, staticGain(candidates[seed]), 0.0, 0.0);
  }

  while (result.backendInvocations < config_.backendBudget) {
    const auto parameters = modelParameters();
    std::size_t selected = candidates.size();
    double selectedAcquisition = -std::numeric_limits<double>::infinity();
    double selectedPrediction = 0.0;
    double selectedUncertainty = 0.0;

    for (const std::size_t index : eligible) {
      if (evaluated[index]) {
        continue;
      }
      const Candidate& candidate = candidates[index];
      const double candidateStaticGain = staticGain(candidate);
      const double predictedGain =
          candidateStaticGain + dot(parameters, candidate.features);
      const double candidateUncertainty = uncertainty(candidate);
      const double expectedTime = std::max(0.0, candidate.estimatedBackendTimeMs);
      const double acquisition =
          (std::max(0.0, predictedGain - bestGain) +
           config_.exploration * candidateUncertainty) /
          (1.0 + config_.timePenalty * expectedTime);

      if (acquisition > selectedAcquisition ||
          (acquisition == selectedAcquisition &&
           candidate.id < candidates[selected].id)) {
        selected = index;
        selectedAcquisition = acquisition;
        selectedPrediction = predictedGain;
        selectedUncertainty = candidateUncertainty;
      }
    }

    if (selected == candidates.size()) {
      break;
    }
    evaluateIndex(selected, selectedPrediction, selectedUncertainty,
                  selectedAcquisition);
  }

  return result;
}

}  // namespace adaptive_superopt
