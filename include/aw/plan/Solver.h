#ifndef AW_SOLVER_H
#define AW_SOLVER_H

// Integer solver, backed by Google OR-Tools CP-SAT.
//
// Solves
//
//   minimize    c^T x
//   subject to  A x >= b,  0 <= x
//
// over non-negative *integer* x. Sparse (CSC) integer matrix.
//
// This header deliberately keeps OR-Tools out of the include path: the
// hand-written code is compiled with -fno-exceptions -fno-rtti, while
// OR-Tools/Abseil/protobuf need both.

#include <cstdint>
#include <span>
#include <vector>

#include "aw/plan/Status.h"
#include "aw/utils/PodVector.h"

namespace aw::solver {

// Compressed sparse column integer matrix.
struct Matrix {
  uint32_t rows = 0;
  uint32_t cols = 0;
  aw::vector<uint32_t> colStart;  // cols + 1
  aw::vector<uint32_t> rowIndex;  // nnz
  aw::vector<int64_t> value;      // nnz

  Matrix() = default;
  Matrix(uint32_t rows, uint32_t cols): rows(rows), cols(cols) {}
};

struct Options {
  // The fraction by which the difference between returned and optimal solution is allowed.
  double relativeGap = 0.01;
  // Absolute gap floor. 0 leaves only the relative rule in force.
  int64_t absoluteGap = 0;

  // Wall-clock budget. <= 0 means no limit. On expiry, CP-SAT gives a feasible plan.
  double maxTimeSeconds = 2.0;

  // Search threads. 0 for auto.
  int numWorkers = 16;

  // Only observable with numWorkers == 1. CP-SAT's own default is 1.
  int randomSeed = 1;
  
  // Starting cap on the total objective, and therefore on every variable.
  // 0 derives one from a lower bound on the optimum. See implementation details.
  int64_t objectiveCap = 0;

  // Known-feasible objective value, from a heuristic pre-pass. 0 means none.
  // Unlike `objectiveCap` this does NOT replace the derived starting cap; it
  // only clamps it and bounds the cap-growth retries. A loose heuristic plan
  // can be orders of magnitude above the optimum, and seeding the variable
  // domains with it makes propagation blow up, so it is used purely as an
  // upper bound: a plan exists at this objective, hence no cap above it is
  // ever worth a retry.
  int64_t objectiveUpperBound = 0;

  // Reduced-cost fixing. The solver probes for any plan within this much of
  // the LP relaxation; if one exists, every column is bounded by
  // floor((incumbent - LP) / d_r), so columns whose bound is 0 are dropped and
  // the rest get a tightened domain before the final solve. 0 disables the
  // pass.
  //
  // Off by default. The probe is a second full CP-SAT solve (model load and
  // presolve, plus proving the tightened cap), which measured 0.5-1.0 s on the
  // recipe graphs. There the integer optimum sits far enough above the LP bound
  // that the probe is infeasible and fixes nothing, so the time is pure
  // overhead; enable it explicitly on instances where the optimum is known to
  // be close to the LP bound.
  double reducedCostGap = 0.0;

  // Flash mode: return the first feasible plan instead of a cheap one.
  //
  // CP-SAT is told to stop as soon as it has any integer-feasible solution, so
  // the answer is a real plan but its cost is arbitrary. The objective and the
  // cap search are untouched -- they still bound the search and keep the
  // variable domains small -- but nothing is spent improving or proving the
  // incumbent, and the reduced-cost fixing probe is skipped because it exists
  // only to accelerate an optimality proof. `Result::provenOptimal` is usually
  // false, but it is still true when the first plan CP-SAT finds happens to be
  // provably optimal on its own (for example when the LP bound already meets
  // it); flash never spends time trying to earn that proof.
  //
  // This trades plan quality for latency: it is what an interactive caller
  // wants when the alternative is no answer at all within its budget. Use
  // `maxTimeSeconds` (plus `relativeGap`) for the opposite trade, where the
  // solver keeps improving until the budget runs out.
  bool flash = false;

  // Optional warm start: one suggested value per column of the model handed to
  // `solve`. It is installed as a CP-SAT solution hint, so it guides the search
  // toward a known plan without constraining it. Values are clamped into the
  // column domain, and the hint is ignored entirely when its length does not
  // match the model being solved (for example after reduced-cost column
  // fixing). The planner fills this from its greedy DAG pre-pass.
  std::span<const int64_t> solutionHint;
};

struct Result {
  PlanStatus status = PlanStatus::INVALID_INPUT;

  // Executions per recipe. Empty unless a solution was found.
  aw::vector<int64_t> x;
  int64_t objective = 0;

  bool provenOptimal = false;
  double bestBound = 0.0;
  double gap = 0.0;

  // Profiling counters.
  int64_t numConflicts = 0;
  int64_t numBranches = 0;

  // Columns removed by reduced-cost fixing before the final solve. 0 when the
  // pass did not run or removed nothing. The returned `x` is always full
  // length, with zeros at the fixed columns.
  uint32_t fixedColumns = 0;
};

// `b` has `A.rows` entries, `c` has `A.cols` entries. Negative entries in `b`
// are free starting stock, matching the balance form used by the planner.
//
// Every entry of `c` must be at least 0. A costed column is bounded by the
// objective cap (c_r x_r <= c^T x <= cap). A *zero-cost* column is not, so the
// solver caps it with the model instead: it bounds the column by the demand
// plus the consumption capacity of the rows it produces into, under the same
// cap. Those rows may be `>=` and so allow overproduction; capping anyway is
// safe because producing more than the plan consumes is pure waste, so an
// optimum always exists inside the cap. `ceiling` (see `absoluteCap`) is the
// int64-range fallback when a zero-cost column produces into nothing.
Result solve(const Matrix& A, std::span<const int64_t> b,
             std::span<const int64_t> c, const Options& options = {});

}  // namespace aw::solver

#endif
