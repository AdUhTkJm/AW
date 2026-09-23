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

#include "aw/Status.h"

namespace aw::solver {

// Compressed sparse column integer matrix.
struct Matrix {
  uint32_t rows = 0;
  uint32_t cols = 0;
  std::vector<uint32_t> colStart;  // cols + 1
  std::vector<uint32_t> rowIndex;  // nnz
  std::vector<int64_t> value;      // nnz
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

  // Reduced-cost fixing. The solver probes for any plan within this much of
  // the LP relaxation; if one exists, every column whose LP reduced cost
  // exceeds the LP/integer gap is dropped before the final solve. 0 disables
  // the pass. Values much above 10 are not worth the extra probe.
  double reducedCostGap = 10.0;
};

struct Result {
  PlanStatus status = PlanStatus::INVALID_INPUT;

  // Executions per recipe. Empty unless a solution was found.
  std::vector<int64_t> x;
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
// Every entry of `c` must be at least 1: the solver bounds the variables with
// the objective cap, which is only sound when each variable costs something.
Result solve(const Matrix& A, std::span<const int64_t> b,
             std::span<const int64_t> c, const Options& options = {});

}  // namespace aw::solver

#endif
