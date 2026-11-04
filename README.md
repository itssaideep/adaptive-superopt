# Budget-Aware Adaptive Superoptimization

Standalone C++17 research prototype for selecting profitable, verified
optimizations when target-backend evaluations are expensive.

The current implementation is architecture-independent and operates on small
fixed-width bit-vector expressions. It provides a complete local workflow
before connecting LLVM, Souper, Z3, or a production compiler backend.

## Research question

Can backend feedback improve candidate selection under a fixed evaluation
budget compared with static-cost ranking, while approaching exhaustive search
with fewer backend calls?

## Workflow

```text
bounded expression grammar
            ↓
finite-domain equivalence verification
            ↓
static cost and feature extraction
            ↓
adaptive candidate selection
            ↓
target-backend cost model
            ↓
cycles, code size, and search metrics
```

The system:

1. Generates structurally diverse expression candidates.
2. Verifies candidates exhaustively over a configured finite input domain.
3. Evaluates candidates with a deterministic VLIW-style backend model.
4. Uses a static prior and online backend feedback to rank candidates.
5. Compares adaptive, static, and exhaustive search.
6. Records backend calls, runtime, objective, improvement, and optimality.

## Current implementation

- **Expressions:** variables, constants, add, subtract, multiply, unsigned
  divide, XOR, AND, OR, and left shift.
- **Verification:** exhaustive fixed-width evaluation with configurable case
  limits.
- **Backend model:** operation latency, dependency depth, VLIW issue width,
  common-subexpression elimination, neutral-operation folding, strength
  reduction, fused multiply-add patterns, and caching.
- **Adaptive selector:** static-cost prior, online ridge residual model,
  uncertainty-aware acquisition, family-diverse seeding, and hard backend
  budgets.
- **Baselines:** static-cost selection and exhaustive backend evaluation.

## Requirements

- CMake 3.20 or newer
- C++17 compiler

The project has no external C++ library dependencies.

## Build and test

```text
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

With a single-configuration generator, executables are usually under
`build/` instead of `build/Release/`.

On Windows with LLVM-MinGW, add its `bin` directory to `PATH` before running
the executables so the runtime DLLs can be found.

## Run the demo

```text
build/Release/adaptive_superopt_demo.exe
```

Options:

```text
--width N             Bit-vector width, 1–32
--depth N             Maximum generated expression depth
--max-candidates N    Candidate generation limit
--budget N            Adaptive/static backend-call budget
```

Example:

```text
build/Release/adaptive_superopt_demo.exe --width 8 --depth 3 --budget 5
```

## Run the experiments

Create the output directory and run the progressive budget sweep:

```text
mkdir results
build/Release/adaptive_superopt_budget_experiment.exe > results/budget_experiment.csv
```

The experiment uses 30 benchmarks covering:

- easy identity cases;
- masking and bitwise transformations;
- strength-reduction and reassociation cases;
- unsigned division cases;
- deceptive multiply-versus-shift backend rankings.

It evaluates budgets **1, 2, 3, 5, 10, and 20** using the same verified
candidate set for each budget. The executable writes:

- `results/budget_experiment.csv` — adaptive, static, and exhaustive results;
- `results/static_accuracy.csv` — static cost, backend cost, prediction error,
  and static/backend rank disagreement.

Important CSV fields include:

| Field | Meaning |
|---|---|
| `verified_candidates` | Number of equivalent candidates retained |
| `backend_calls` | Actual target-backend evaluations |
| `runtime_ms` | Selection runtime for that method |
| `best_objective` | Best backend objective found |
| `improvement_percent` | Improvement relative to the reference |
| `global_optimum` | Whether the exhaustive result was reached |

## Semantics and correctness boundary

The local verifier defines arithmetic over unsigned bit-vectors:

- addition, subtraction, and multiplication wrap modulo `2^width`;
- division is unsigned;
- division by zero is rejected;
- shifts are masked to the configured width;
- inputs and constants are truncated to the configured width.

The correctness guarantee is exhaustive equivalence within the configured
finite domain. It is not a general proof of LLVM semantics, undefined
behavior, memory effects, floating-point behavior, or target-specific
instruction semantics.

## Project layout

```text
include/adaptive_superopt/
  expression.h       Expression representation and evaluation API
  generator.h        Candidate generation API
  verifier.h         Finite-domain verification API
  backend.h          Backend model and cache API
  selector.h         Adaptive selection API
  superoptimizer.h   End-to-end pipeline API

src/
  expression.cpp
  generator.cpp
  verifier.cpp
  backend.cpp
  selector.cpp
  superoptimizer.cpp

tools/
  demo.cpp
  budget_experiment.cpp

tests/
  selector_tests.cpp

docs/
  research-plan.md
```

## Extending the system

### Add a benchmark

Add a reference expression and equivalent seed expressions in
`tools/budget_experiment.cpp`. Keep the input width, verifier configuration,
and backend configuration identical across compared methods.

### Connect a real backend

Implement a `BackendEvaluator` that returns `BackendResult` and preserves the
same candidate identity, objective definition, failure handling, and metric
logging. The selector should receive only semantically verified candidates.

### Connect LLVM/Souper/Z3

Replace the bounded expression generator and verifier with adapters that
produce fixed-width LLVM candidates and perform LLVM-aware equivalence or
translation validation. The current selector/backend interfaces are the
integration boundary.

## Research limitations

The local backend is a deterministic model, not a real compiler or simulator.
Its costs are useful for controlled experiments, but claims about hardware,
LLVM code generation, or DSP scheduling require a real target backend and
independent validation. The static-accuracy CSV is also diagnostic: raw static
cost and backend objective are not calibrated physical units.

See [`docs/research-plan.md`](docs/research-plan.md) for baselines, metrics,
experimental safeguards, and literature context.
