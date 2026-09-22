#ifndef AW_LP_SOLVER_H
#define AW_LP_SOLVER_H

// Internal sparse LP solver. Not part of the public API.
//
// Solves
//
//   minimize    c^T x
//   subject to  A x >= b,  x >= 0
//
// with a two-phase revised simplex. The constraint matrix is stored sparse
// (CSC); the basis inverse is kept in product form (eta file) over a dense LU
// refactorization that is refreshed every `refactorInterval` pivots. All
// arithmetic is double; 1e9 is the largest magnitude the solver tolerates.

#include <cstdint>
#include <vector>
#include "aw/Status.h"

namespace aw::lp {

struct Matrix {
  uint32_t rows = 0;
  uint32_t cols = 0;
  std::vector<uint32_t> colStart;  // cols + 1
  std::vector<uint32_t> rowIndex;  // nnz
  std::vector<double> value;       // nnz
};

struct Options {
  double eps = 1e-6;
  // Smallest pivot accepted by the ratio test.
  double pivotTol = 1e-9;
  // Largest magnitude allowed anywhere in the simplex state.
  double maxMagnitude = 1e9;
  // Interval between dense LU refactorizations, measured by iterations.
  uint32_t refactorIntv = 100;
  uint32_t maxIter = 200000;
};

struct Result {
  PlanStatus status = PlanStatus::INVALID_INPUT;
  std::vector<double> x; // solution
  double objective = 0.0;
  uint32_t iterations = 0;
  uint32_t refactorizations = 0;
};

Result solve(const Matrix& A, const std::vector<double>& b,
             const std::vector<double>& c, const Options& options = {});

}  // namespace aw::lp

#endif
