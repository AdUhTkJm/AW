#include "aw/LpSolver.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace aw::lp {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr uint32_t kNoIndex = UINT32_MAX;
// Pivot floor for the sparse LU refactorization. Recipe amounts are integers
// of at least one, so anything smaller is noise rather than a basis entry.
constexpr double kLuPivotTolerance = 1e-12;

// ---------------------------------------------------------------------------
// Sparse LU with partial pivoting (left looking).
// ---------------------------------------------------------------------------
//
// Produces P B = L U, where pstep[k] is the source row that became the k-th
// pivot row and pinv maps a source row back to its pivot step. L is unit lower
// triangular, U is upper triangular. Both are stored sparsely. L lives by
// column; its entries keep the source row and recover the pivot step through
// pinv. U is duplicated by row (for U x = y) and by column (for U^T z = c).
struct SparseLu {
  uint32_t n = 0;
  std::vector<uint32_t> pinv;   // source row -> pivot step, kNoIndex if none
  std::vector<uint32_t> pstep;  // pivot step -> source row
  std::vector<double> diag;     // U[k][k]

  std::vector<std::vector<uint32_t>> lRow;  // per column, source rows
  std::vector<std::vector<double>> lVal;
  std::vector<std::vector<uint32_t>> uRowCol;  // per step, columns
  std::vector<std::vector<double>> uRowVal;
  std::vector<std::vector<uint32_t>> uColRow;  // per step, source rows
  std::vector<std::vector<double>> uColVal;

  std::vector<double> work;
  std::vector<uint8_t> mark;
  std::vector<uint32_t> touched;
  bool singular = false;

  void factorize(uint32_t size, const std::vector<uint32_t> &bp,
                 const std::vector<uint32_t> &bRow,
                 const std::vector<double> &bVal, double tol);

  // v := B^{-1} v
  void ftran(std::vector<double> &v);
  // v := B^{-T} v
  void btran(std::vector<double> &v);
};

// One product form eta column: E = I with column p replaced by d. The entries
// below exclude p; pivot holds d[p].
struct Eta {
  uint32_t p = 0;
  double pivot = 1.0;
  std::vector<uint32_t> rows;
  std::vector<double> vals;
};

// ---------------------------------------------------------------------------
// Two phase revised simplex
// ---------------------------------------------------------------------------
struct Simplex {
  const Matrix &A;
  const std::vector<double> &b;
  const std::vector<double> &c;
  const Options &opt;

  uint32_t m = 0;      // rows (items)
  uint32_t n = 0;      // structural variables (recipes)
  uint32_t nArt = 0;   // artificial variables (one per row with b_i > 0)
  uint32_t N = 0;      // total variables: n + m + nArt

  std::vector<uint32_t> artRow;    // artificial k -> row
  std::vector<uint32_t> artOfRow;  // row -> artificial k, or kNoIndex

  std::vector<uint32_t> basis;     // variable at each basis position
  std::vector<uint8_t> isBasic;    // over all N variables
  std::vector<double> xB;          // basic values
  std::vector<double> cost;        // over all N variables

  SparseLu lu;
  std::vector<Eta> etas;

  std::vector<double> y;     // dual values
  std::vector<double> col;   // entering column in basis coordinates

  uint64_t iterations = 0;
  uint32_t refactorizations = 0;
  bool bland = false;
  uint32_t degenerate = 0;
  PlanStatus failStatus = PlanStatus::NUMERICAL_FAIL;

  Simplex(const Matrix &matrix, const std::vector<double> &rhs,
          const std::vector<double> &objective, const Options &options)
      : A(matrix), b(rhs), c(objective), opt(options) {
    m = matrix.rows;
    n = matrix.cols;
  }

  uint32_t surplusVar(uint32_t i) const { return n + i; }
  uint32_t artVar(uint32_t k) const { return n + m + k; }
  bool isArtificial(uint32_t v) const { return v >= n + m; }

  bool magnitudeOk(const std::vector<double> &v) const {
    for (double value : v) {
      if (!(std::fabs(value) <= opt.maxMagnitude))
        return false;
    }
    return true;
  }

  // Builds the basis columns sparsely and refactorizes.
  bool refactor() {
    std::vector<uint32_t> bp;
    std::vector<uint32_t> bRow;
    std::vector<double> bVal;
    bp.reserve((size_t) m + 1);
    bp.push_back(0);
    for (uint32_t k = 0; k < m; k++) {
      const uint32_t v = basis[k];
      if (v < n) {
        for (uint32_t e = A.colStart[v]; e < A.colStart[v + 1]; e++) {
          bRow.push_back(A.rowIndex[e]);
          bVal.push_back(A.value[e]);
        }
      } else if (v < n + m) {
        bRow.push_back(v - n);
        bVal.push_back(-1.0);
      } else {
        bRow.push_back(artRow[v - n - m]);
        bVal.push_back(1.0);
      }
      bp.push_back((uint32_t) bRow.size());
    }
    lu.factorize(m, bp, bRow, bVal, kLuPivotTolerance);
    refactorizations++;
    etas.clear();
    return !lu.singular;
  }

  static void applyEtaFtran(const Eta &eta, std::vector<double> &v) {
    const double t = v[eta.p] / eta.pivot;
    const size_t count = eta.rows.size();
    for (size_t k = 0; k < count; k++)
      v[eta.rows[k]] -= eta.vals[k] * t;
    v[eta.p] = t;
  }

  static void applyEtaBtran(const Eta &eta, std::vector<double> &v) {
    double sum = 0.0;
    const size_t count = eta.rows.size();
    for (size_t k = 0; k < count; k++)
      sum += eta.vals[k] * v[eta.rows[k]];
    v[eta.p] = (v[eta.p] - sum) / eta.pivot;
  }

  void ftran(std::vector<double> &v) {
    lu.ftran(v);
    for (const Eta &eta : etas)
      applyEtaFtran(eta, v);
  }

  void btran(std::vector<double> &v) {
    for (size_t k = etas.size(); k-- > 0;)
      applyEtaBtran(etas[k], v);
    lu.btran(v);
  }

  // Reduced cost of a non-artificial variable.
  double reducedCost(uint32_t j, const std::vector<double> &dual) const {
    if (j < n) {
      double d = cost[j];
      for (uint32_t e = A.colStart[j]; e < A.colStart[j + 1]; e++)
        d -= dual[A.rowIndex[e]] * A.value[e];
      return d;
    }
    // Surplus column is -e_i.
    return cost[j] + dual[j - n];
  }

  void fillColumn(uint32_t j, std::vector<double> &out) const {
    out.assign(m, 0.0);
    if (j < n) {
      for (uint32_t e = A.colStart[j]; e < A.colStart[j + 1]; e++)
        out[A.rowIndex[e]] += A.value[e];
    } else if (j < n + m) {
      out[j - n] = -1.0;
    } else {
      out[artRow[j - n - m]] = 1.0;
    }
  }

  // Runs the primal simplex on the current basis until no reduced cost is
  // below -eps.
  bool optimize() {
    while (true) {
      if (iterations >= opt.maxIter) {
        failStatus = PlanStatus::ITER_LIMIT;
        return false;
      }

      y.assign(m, 0.0);
      for (uint32_t k = 0; k < m; k++)
        y[k] = cost[basis[k]];
      btran(y);
      if (!magnitudeOk(y)) {
        failStatus = PlanStatus::NUMERICAL_FAIL;
        return false;
      }

      uint32_t enter = kNoIndex;
      if (bland) {
        for (uint32_t j = 0; j < n + m; j++) {
          if (isBasic[j])
            continue;
          if (reducedCost(j, y) < -opt.eps) {
            enter = j;
            break;
          }
        }
      } else {
        double best = -opt.eps;
        for (uint32_t j = 0; j < n + m; j++) {
          if (isBasic[j])
            continue;
          const double d = reducedCost(j, y);
          if (d < best) {
            best = d;
            enter = j;
          }
        }
      }
      if (enter == kNoIndex)
        return true;

      fillColumn(enter, col);
      ftran(col);

      uint32_t leave = kNoIndex;
      double bestRatio = kInfinity;
      const double ratioTol = 1e-12;
      for (uint32_t i = 0; i < m; i++) {
        if (col[i] <= opt.pivotTol)
          continue;
        const double ratio = xB[i] / col[i];
        if (ratio < bestRatio - ratioTol) {
          bestRatio = ratio;
          leave = i;
        } else if (leave != kNoIndex && std::fabs(ratio - bestRatio) <= ratioTol &&
                   basis[i] < basis[leave]) {
          leave = i;
        }
      }
      if (leave == kNoIndex) {
        failStatus = PlanStatus::NUMERICAL_FAIL;
        return false;
      }

      const double theta = xB[leave] / col[leave];
      for (uint32_t i = 0; i < m; i++)
        xB[i] -= theta * col[i];
      xB[leave] = theta;

      isBasic[basis[leave]] = 0;
      basis[leave] = enter;
      isBasic[enter] = 1;

      Eta eta;
      eta.p = leave;
      eta.pivot = col[leave];
      for (uint32_t i = 0; i < m; i++) {
        if (i == leave || col[i] == 0.0)
          continue;
        eta.rows.push_back(i);
        eta.vals.push_back(col[i]);
      }
      etas.push_back(std::move(eta));

      iterations++;
      if (!magnitudeOk(xB) || !(std::fabs(theta) <= opt.maxMagnitude)) {
        failStatus = PlanStatus::NUMERICAL_FAIL;
        return false;
      }

      if (theta <= opt.eps) {
        if (++degenerate > 2 * m)
          bland = true;
      } else {
        degenerate = 0;
      }

      if (etas.size() >= opt.refactorIntv) {
        if (!refactor()) {
          failStatus = PlanStatus::NUMERICAL_FAIL;
          return false;
        }
      }
    }
  }

  // Pivots any basic artificial that sits at zero out of the basis. Such a
  // pivot is degenerate: the artificial leaves at value zero and the primal
  // solution does not move.
  bool cleanupArtificials() {
    for (uint32_t k = 0; k < nArt; k++) {
      const uint32_t artificial = artVar(k);
      uint32_t position = kNoIndex;
      for (uint32_t i = 0; i < m; i++) {
        if (basis[i] == artificial) {
          position = i;
          break;
        }
      }
      if (position == kNoIndex)
        continue;
      if (xB[position] > opt.eps)
        return false;

      // y = B^{-T} e_position, so y . A_j is (B^{-1} A_j) at `position`.
      y.assign(m, 0.0);
      y[position] = 1.0;
      btran(y);

      uint32_t pick = kNoIndex;
      for (uint32_t j = 0; j < n + m; j++) {
        if (isBasic[j])
          continue;
        double d = 0.0;
        if (j < n) {
          for (uint32_t e = A.colStart[j]; e < A.colStart[j + 1]; e++)
            d += y[A.rowIndex[e]] * A.value[e];
        } else {
          d = -y[j - n];
        }
        if (std::fabs(d) > opt.pivotTol) {
          pick = j;
          break;
        }
      }
      // The row is redundant with respect to every other column. Leaving the
      // artificial basic at zero is harmless for the primal solution.
      if (pick == kNoIndex)
        continue;

      fillColumn(pick, col);
      ftran(col);
      if (!(std::fabs(col[position]) > opt.pivotTol))
        return false;

      isBasic[basis[position]] = 0;
      basis[position] = pick;
      isBasic[pick] = 1;
      xB[position] = 0.0;

      Eta eta;
      eta.p = position;
      eta.pivot = col[position];
      for (uint32_t i = 0; i < m; i++) {
        if (i == position || col[i] == 0.0)
          continue;
        eta.rows.push_back(i);
        eta.vals.push_back(col[i]);
      }
      etas.push_back(std::move(eta));
      iterations++;
      if (etas.size() >= opt.refactorIntv) {
        if (!refactor())
          return false;
      }
    }
    return true;
  }

  Result run() {
    Result result;

    // Artificials are needed only for rows whose right hand side is positive.
    artOfRow.assign(m, kNoIndex);
    for (uint32_t i = 0; i < m; i++) {
      if (b[i] > 0.0) {
        artOfRow[i] = nArt;
        artRow.push_back(i);
        nArt++;
      }
    }
    N = n + m + nArt;

    basis.assign(m, 0);
    isBasic.assign(N, 0);
    xB.assign(m, 0.0);
    for (uint32_t i = 0; i < m; i++) {
      if (artOfRow[i] != kNoIndex) {
        basis[i] = artVar(artOfRow[i]);
        xB[i] = b[i];
      } else {
        basis[i] = surplusVar(i);
        xB[i] = -b[i];
      }
      isBasic[basis[i]] = 1;
    }
    if (!magnitudeOk(xB)) {
      result.status = PlanStatus::NUMERICAL_FAIL;
      return result;
    }

    if (!refactor()) {
      result.status = PlanStatus::NUMERICAL_FAIL;
      return result;
    }

    // Phase 1: minimize the sum of artificials.
    if (nArt != 0) {
      double artificialMass = 0.0;
      for (uint32_t k = 0; k < nArt; k++)
        artificialMass += b[artRow[k]];

      cost.assign(N, 0.0);
      for (uint32_t k = 0; k < nArt; k++)
        cost[artVar(k)] = 1.0;

      if (!optimize()) {
        result.status = failStatus;
        result.iterations = (uint32_t) iterations;
        return result;
      }

      double residual = 0.0;
      for (uint32_t i = 0; i < m; i++) {
        if (isArtificial(basis[i]))
          residual += xB[i];
      }
      if (residual > opt.eps * (1.0 + artificialMass)) {
        result.status = PlanStatus::INFEASIBLE;
        result.iterations = (uint32_t) iterations;
        return result;
      }

      if (!cleanupArtificials()) {
        result.status = PlanStatus::NUMERICAL_FAIL;
        result.iterations = (uint32_t) iterations;
        return result;
      }
    }

    // Phase 2: minimize the number of recipe executions.
    cost.assign(N, 0.0);
    for (uint32_t j = 0; j < n; j++)
      cost[j] = 1.0;

    if (!optimize()) {
      result.status = failStatus;
      result.iterations = (uint32_t) iterations;
      return result;
    }

    // Extract and validate.
    std::vector<double> x(n, 0.0);
    for (uint32_t k = 0; k < m; k++) {
      if (basis[k] < n)
        x[basis[k]] = xB[k];
      else if (isArtificial(basis[k]) && xB[k] > opt.eps)
        result.status = PlanStatus::NUMERICAL_FAIL;
    }

    std::vector<double> net(m, 0.0);
    for (uint32_t j = 0; j < n; j++) {
      if (x[j] == 0.0)
        continue;
      for (uint32_t e = A.colStart[j]; e < A.colStart[j + 1]; e++)
        net[A.rowIndex[e]] += A.value[e] * x[j];
    }
    for (uint32_t i = 0; i < m; i++) {
      const double tolerance = opt.eps * (1.0 + std::fabs(b[i]));
      if (net[i] < b[i] - tolerance)
        result.status = PlanStatus::NUMERICAL_FAIL;
    }
    for (uint32_t j = 0; j < n; j++) {
      if (x[j] < -opt.eps)
        result.status = PlanStatus::NUMERICAL_FAIL;
    }
    if (result.status == PlanStatus::NUMERICAL_FAIL) {
      result.iterations = (uint32_t) iterations;
      return result;
    }

    double objective = 0.0;
    for (uint32_t j = 0; j < n; j++)
      objective += x[j];

    result.status = PlanStatus::OK;
    result.x = std::move(x);
    result.objective = objective;
    result.iterations = (uint32_t) iterations;
    result.refactorizations = refactorizations;
    return result;
  }
};

void SparseLu::factorize(uint32_t size, const std::vector<uint32_t> &bp,
                const std::vector<uint32_t> &bRow,
                const std::vector<double> &bVal, double tol) {
  n = size;
  pinv.assign(n, kNoIndex);
  pstep.assign(n, kNoIndex);
  diag.assign(n, 0.0);
  lRow.clear();
  lRow.resize(n);
  lVal.clear();
  lVal.resize(n);
  uRowCol.clear();
  uRowCol.resize(n);
  uRowVal.clear();
  uRowVal.resize(n);
  uColRow.clear();
  uColRow.resize(n);
  uColVal.clear();
  uColVal.resize(n);
  work.assign(n, 0.0);
  mark.assign(n, 0);
  touched.clear();
  singular = false;

  for (uint32_t k = 0; k < n; k++) {
    touched.clear();
    for (uint32_t e = bp[k]; e < bp[k + 1]; e++) {
      const uint32_t r = bRow[e];
      if (!mark[r]) {
        mark[r] = 1;
        touched.push_back(r);
      }
      work[r] += bVal[e];
    }

    // Forward substitution against the already computed L columns.
    for (uint32_t j = 0; j < k; j++) {
      const uint32_t row = pstep[j];
      const double multiplier = work[row];
      if (multiplier == 0.0)
        continue;
      work[row] = 0.0;
      uRowCol[j].push_back(k);
      uRowVal[j].push_back(multiplier);
      uColRow[k].push_back(j);
      uColVal[k].push_back(multiplier);
      const auto &rows = lRow[j];
      const auto &vals = lVal[j];
      for (size_t e = 0; e < rows.size(); e++) {
        const uint32_t r = rows[e];
        if (!mark[r]) {
          mark[r] = 1;
          touched.push_back(r);
        }
        work[r] -= vals[e] * multiplier;
      }
    }

    uint32_t pivot = kNoIndex;
    double best = 0.0;
    for (uint32_t r : touched) {
      if (pinv[r] != kNoIndex)
        continue;
      const double value = std::fabs(work[r]);
      if (value > best) {
        best = value;
        pivot = r;
      }
    }
    if (!(best > tol)) {
      for (uint32_t r : touched) {
        mark[r] = 0;
        work[r] = 0.0;
      }
      singular = true;
      return;
    }

    pinv[pivot] = k;
    pstep[k] = pivot;
    const double diagonal = work[pivot];
    diag[k] = diagonal;
    uRowCol[k].push_back(k);
    uRowVal[k].push_back(diagonal);
    uColRow[k].push_back(k);
    uColVal[k].push_back(diagonal);

    for (uint32_t r : touched) {
      if (pinv[r] != kNoIndex)
        continue;
      const double value = work[r];
      if (value == 0.0)
        continue;
      lRow[k].push_back(r);
      lVal[k].push_back(value / diagonal);
    }
    for (uint32_t r : touched) {
      mark[r] = 0;
      work[r] = 0.0;
    }
  }
}

void SparseLu::ftran(std::vector<double> &v) {
  work.resize(n);
  for (uint32_t k = 0; k < n; k++)
    work[k] = v[pstep[k]];
  for (uint32_t k = 0; k < n; k++) {
    const double yk = work[k];
    if (yk == 0.0)
      continue;
    const auto &rows = lRow[k];
    const auto &vals = lVal[k];
    for (size_t e = 0; e < rows.size(); e++)
      work[pinv[rows[e]]] -= vals[e] * yk;
  }
  for (uint32_t k = n; k-- > 0;) {
    double sum = work[k];
    const auto &cols = uRowCol[k];
    const auto &vals = uRowVal[k];
    for (size_t e = 0; e < cols.size(); e++) {
      if (cols[e] == k)
        continue;
      sum -= vals[e] * work[cols[e]];
    }
    work[k] = sum / diag[k];
  }
  v.swap(work);
}

void SparseLu::btran(std::vector<double> &v) {
  for (uint32_t k = 0; k < n; k++) {
    double sum = v[k];
    const auto &rows = uColRow[k];
    const auto &vals = uColVal[k];
    for (size_t e = 0; e < rows.size(); e++) {
      if (rows[e] == k)
        continue;
      sum -= vals[e] * v[rows[e]];
    }
    v[k] = sum / diag[k];
  }
  for (uint32_t k = n; k-- > 0;) {
    double sum = v[k];
    const auto &rows = lRow[k];
    const auto &vals = lVal[k];
    for (size_t e = 0; e < rows.size(); e++)
      sum -= vals[e] * v[pinv[rows[e]]];
    v[k] = sum;
  }
  work.resize(n);
  for (uint32_t k = 0; k < n; k++)
    work[pstep[k]] = v[k];
  v.swap(work);
}



}  // namespace

Result solve(const Matrix &A, const std::vector<double> &b,
             const std::vector<double> &c, const Options &options) {
  Result result;

  // Check validity.
  // TODO: do we really need this? `solve()` seems to be called only from trusted places.
  if (A.colStart.size() != (size_t) A.cols + 1 || A.colStart.empty() ||
      A.colStart[0] != 0 || A.colStart.back() != A.rowIndex.size() ||
      A.rowIndex.size() != A.value.size() || b.size() != A.rows ||
      c.size() != A.cols) {
    return result;
  }
  for (uint32_t row : A.rowIndex) {
    if (row >= A.rows)
      return result;
  }

  Simplex simplex(A, b, c, options);
  return simplex.run();
}

}  // namespace aw::lp
