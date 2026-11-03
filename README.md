# Budget-Aware Adaptive Superoptimization

Standalone research system for budget-aware superoptimization of small
fixed-width bit-vector expressions:

> **Budget-Aware Adaptive Superoptimization for Target-Specific Compilers**

The project investigates whether SMT-verified optimization candidates can be
selected more effectively when a limited number of candidates may be compiled
through an expensive target backend.

## What it does

The executable runs a complete local experiment:

1. Generate expression candidates from a bounded grammar.
2. Exhaustively verify candidates over a fixed-width input domain.
3. Estimate target cost using a deterministic VLIW-style backend model.
4. Select candidates with the adaptive budget-aware policy.
5. Compare adaptive selection with static-cost and exhaustive baselines.
6. Report objective, cycles, code size and backend invocation counts.

The system is intentionally architecture-independent. The target backend is a
replaceable C++ adapter, and LLVM/Souper/Z3 integrations can be added without
changing the selector contract.

## Components

- `expression.*`: immutable expression trees and fixed-width evaluation;
- `generator.*`: bounded candidate enumeration;
- `verifier.*`: exhaustive equivalence checking;
- `backend.*`: deterministic target-cost model and evaluation cache;
- `selector.*`: static prior, online residual model and budgeted acquisition;
- `superoptimizer.*`: end-to-end pipeline and baseline runners.

## Build and test

Requirements:

- CMake 3.20 or newer;
- a C++17 compiler.

```text
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the deterministic demonstration:

```text
build/Release/adaptive_superopt_demo.exe
```

On a single-configuration generator, the executable is normally under
`build/adaptive_superopt_demo` instead.

The demo accepts `--width`, `--depth`, `--max-candidates` and `--budget`.
For example:

```text
build/Release/adaptive_superopt_demo.exe --budget 2 --depth 2
```

On Windows with LLVM-MinGW, run the executable with the compiler's `bin`
directory on `PATH` so its runtime DLLs can be found.

## Planned integration boundary

```text
expression candidate
        ↓
fixed-width equivalence check
        ↓
static feature extraction
        ↓
AdaptiveSelector
        ↓
target backend adapter
        ↓
cycles, code size and VLIW-style scheduling reports
```

The current correctness guarantee is exhaustive equivalence within the
configured finite input domain. It is not a proof for arbitrary LLVM
undefined-behavior semantics. LLVM/Souper/Z3 and real compiler backends remain
integration adapters for a later target-specific deployment.

See [`docs/research-plan.md`](docs/research-plan.md) for the research
question, baselines, metrics and experimental protocol.
