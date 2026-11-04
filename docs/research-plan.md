# Research plan

## Working research question

Given a fixed budget of target-backend compilations, can an adaptive selector
that combines Souper's static cost with online backend feedback produce better
execution-cycle and code-size results than static-cost selection, while
approaching exhaustive backend-guided search with fewer backend invocations?

## Implemented system

The repository now contains a runnable architecture-independent reference
implementation. It generates bounded fixed-width expression trees, verifies
them exhaustively, evaluates them with a deterministic VLIW-style cost model,
and compares adaptive, static-cost and exhaustive search under a backend
invocation budget.

The implementation provides a controlled environment for testing the search
policy before connecting a real compiler backend.

The reproducible budget sweep is implemented by
`adaptive_superopt_budget_experiment`. It uses the same verified candidate set
for budgets 1, 2, 3, 5, 10 and 20, and writes CSV rows for adaptive, static and
exhaustive selection. The current benchmark suite covers masked arithmetic,
scaled multiplication, unsigned division, identity simplification and
masking/OR identities.

The same run produces `results/static_accuracy.csv`. It compares every
verified candidate's static cost with the backend objective and records raw
and relative error plus static-versus-backend rank disagreement.

The reference backend deliberately models deceptive rankings: raw expression
operation count is used as the static prior, while backend cost accounts for
operation latency, VLIW issue width, dependencies, common subexpressions,
neutral-operation folding, multiply strength reduction and fused multiply-add
patterns.

## Proposed contribution

The proposed contribution is a budget-aware candidate selector. SMT
verification remains a correctness gate; the adaptive model only controls the
order in which verified candidates receive expensive backend evaluation.

For candidate `c`, use a static gain prior and update an online model of the
residual between static and backend-observed gain:

```text
static_gain(c) = (static(reference) - static(c)) / static(reference)
backend_gain(c) = (backend(reference) - backend(c)) / backend(reference)
residual(c) = backend_gain(c) - static_gain(c)
```

Select the next candidate using expected improvement plus uncertainty, divided
by an optional estimate of backend evaluation time. Stop at the configured
backend invocation budget and return the best successful backend result.

## Baselines

1. Reference optimizing compiler without superoptimization.
2. Souper static-cost selection.
3. Static top-`B` selection under the same backend budget.
4. Exhaustive backend evaluation over the generated SMT-valid candidate set.
5. Optional random or round-robin selection as a search sanity check.

All methods must use the same candidate generator, candidate limits and SMT
verification results.

## Metrics

- final simulator or hardware execution cycles;
- backend-reported scheduled cycles;
- code size in bytes, instructions and VLIW packets;
- search wall-clock runtime;
- SMT time and backend time separately;
- backend invocation count and cache misses;
- SMT validity rate;
- backend improvement rate;
- end-to-end optimization success rate;
- budget-quality curves and regret relative to exhaustive search;
- static/backend ranking correlation and regression rate.

## Experimental safeguards

- Treat SMT `unknown` and timeout as rejection.
- Keep solver correctness and backend profitability separate.
- Use the full enclosing function when backend scheduling context matters.
- Pin compiler, backend and target revisions.
- Include cache state in experiment logs.
- Tune selector parameters only on development workloads.
- Evaluate held-out applications or use leave-one-application-out validation.
- Report dynamic simulation cycles separately from static schedule estimates.

## Literature anchors

- [Souper](https://arxiv.org/abs/1711.04422) documents the difficulty of
  target-aware LLVM-level profitability estimation.
- [STOKE](https://doi.org/10.1145/2451116.2451150) establishes stochastic
  superoptimization for x86-64.
- [GreenThumb](https://doi.org/10.1145/2892208.2892233) provides an extensible
  multi-strategy superoptimizer framework.
- [Scaling Up Superoptimization](https://doi.org/10.1145/2872362.2872387)
  demonstrates pruning and cooperative search.
- [Dataflow-based pruning](https://doi.org/10.1145/3428245) addresses solver
  cost in Souper.
- [Alive2](https://doi.org/10.1145/3453483.3454030) provides SMT-based LLVM
  translation validation.
- [Minotaur](https://doi.org/10.1145/3689766) is the closest target-aware LLVM
  precedent, using cheap target costs followed by more detailed machine-code
  analysis.

The novelty claim should remain scoped to the combination of online backend
budget allocation, verified candidates and target-backend evaluation until the
final systematic literature search is complete. The current local verifier is
exhaustive only over its configured finite bit-vector domain; it should not be
described as a general LLVM semantic proof.
