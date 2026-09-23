// CP-SAT backend for aw::solver. The only file in the project that includes
// OR-Tools headers, and the only one that must be compiled with exceptions and
// RTTI enabled (see CMakeLists.txt).
#ifdef IN_VSCODE
#define OR_PROTO_DLL // Make sure clangd works properly.
#endif

#include "aw/Solver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

#include <absl/types/span.h>
#include <ortools/linear_solver/linear_solver.h>
#include <ortools/sat/cp_model.h>
#include <ortools/sat/cp_model_solver.h>
#include <ortools/sat/sat_parameters.pb.h>
#include <ortools/util/sorted_interval_list.h>

namespace aw::solver {
namespace {

namespace sat = operations_research::sat;
using operations_research::Domain;


// The objective cap is searched by attempting a small value and then
// exponentially scale it. This is used to bound CP-SAT.
constexpr int64_t CAP_GROWTH = 8;
constexpr int MAX_ATTEMPTS = 8;

// Set AW_SOLVER_DEBUG to get the cap search traced to stderr.
bool searchDebug() {
  static const bool enabled = std::getenv("AW_SOLVER_DEBUG") != nullptr;
  return enabled;
}

// Same as abs, but avoids the std::abs(INT64_MIN) trap.
uint64_t magnitude(int64_t value) {
  return value < 0 ? (uint64_t) (-(value + 1)) + 1 : (uint64_t) value;
}

uint64_t satAdd(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

int64_t ceilDiv(int64_t numerator, int64_t denominator) {
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// The largest value any variable may take to keep everything in range of int64_t.
int64_t absoluteCap(const Matrix &A, std::span<const int64_t> c) {
  std::vector<uint64_t> rowAbs(A.rows, 0);
  for (size_t k = 0; k < A.rowIndex.size(); k++)
    rowAbs[A.rowIndex[k]] = satAdd(rowAbs[A.rowIndex[k]], magnitude(A.value[k]));

  uint64_t maxRow = *std::ranges::max_element(rowAbs);
  uint64_t goal = 0;
  for (int64_t value : c)
    goal = satAdd(goal, magnitude(value));

  // Leave some room for arithmetic in presolver.
  const uint64_t limit = INT64_MAX / 2;
  uint64_t bound = limit;
  if (maxRow != 0)
    bound = std::min(bound, limit / maxRow);
  if (goal != 0)
    bound = std::min(bound, limit / goal);
  return (int64_t) std::max<uint64_t>(bound, 1);
}

// A valid lower bound on sum(x): row i needs b_i, and no coefficient exceeds
// maxPositive[i], so at least ceil(b_i / maxPositive[i]) executions are needed
// (negative coefficients only make the row harder to satisfy).
//
// This is deliberately weak. A tighter estimate would need the LP relaxation:
// these graphs contain cycles that amplify (four planks from one log) with no
// "raw" base item, so a Bellman-Ford style cost relaxation, which needs
// something to seed from, reports the whole graph unreachable. The cap is
// therefore grown empirically instead.
int64_t lowerBoundOnTotal(const Matrix &A, std::span<const int64_t> b) {
  std::vector<int64_t> maxPositive((size_t) A.rows, 0);
  for (size_t k = 0; k < A.rowIndex.size(); k++) {
    const int64_t value = A.value[k];
    if (value > 0)
      maxPositive[A.rowIndex[k]] = std::max(maxPositive[A.rowIndex[k]], value);
  }
  int64_t bound = 1;
  for (uint32_t i = 0; i < A.rows; i++) {
    if (b[i] > 0 && maxPositive[i] > 0)
      bound = std::max(bound, ceilDiv(b[i], maxPositive[i]));
  }
  return bound;
}

void fillStatus(const sat::CpSolverResponse &response, Result &result) {
  switch (response.status()) {
    case sat::CpSolverStatus::OPTIMAL:
    case sat::CpSolverStatus::FEASIBLE:
      result.status = PlanStatus::OK;
      result.provenOptimal = response.status() == sat::CpSolverStatus::OPTIMAL;
      break;
    case sat::CpSolverStatus::INFEASIBLE:
      result.status = PlanStatus::INFEASIBLE;
      break;
    case sat::CpSolverStatus::MODEL_INVALID:
      result.status = PlanStatus::INVALID_INPUT;
      break;
    case sat::CpSolverStatus::UNKNOWN:
      // The time limit ran out before any solution was found.
      result.status = PlanStatus::ITER_LIMIT;
      break;
    default:
      break;
  }
}

// Row-major view of the same matrix, so a constraint is one pass over a
// contiguous run.
struct RowMajor {
  std::vector<uint32_t> start;   // rows + 1
  std::vector<uint32_t> column;  // nnz
  std::vector<int64_t> value;    // nnz
};

RowMajor transpose(const Matrix &A) {
  RowMajor rows;
  rows.start.assign((size_t) A.rows + 1, 0);
  for (uint32_t k = 0; k < A.rowIndex.size(); k++)
    rows.start[A.rowIndex[k] + 1]++;
  for (uint32_t i = 0; i < A.rows; i++)
    rows.start[i + 1] += rows.start[i];

  const uint32_t nnz = (uint32_t) A.rowIndex.size();
  rows.column.resize(nnz);
  rows.value.resize(nnz);
  std::vector<uint32_t> cursor(rows.start.begin(), rows.start.end() - 1);
  for (uint32_t r = 0; r < A.cols; r++) {
    for (uint32_t k = A.colStart[r]; k < A.colStart[r + 1]; k++) {
      const uint32_t at = cursor[A.rowIndex[k]]++;
      rows.column[at] = r;
      rows.value[at] = A.value[k];
    }
  }
  return rows;
}

double relaxationValue(const Matrix &A, const RowMajor &rows,
                       std::span<const int64_t> b, std::span<const int64_t> c) {
  operations_research::MPSolver solver("relaxation",
                                       operations_research::MPSolver::GLOP_LINEAR_PROGRAMMING);
  solver.SuppressOutput();

  std::vector<operations_research::MPVariable*> variables;
  variables.reserve(A.cols);
  for (uint32_t r = 0; r < A.cols; r++)
    variables.push_back(solver.MakeNumVar(0.0, solver.infinity(), ""));

  operations_research::MPObjective* objective = solver.MutableObjective();
  for (uint32_t r = 0; r < A.cols; r++)
    if (c[r] != 0)
      objective->SetCoefficient(variables[r], (double) c[r]);
  objective->SetMinimization();

  // Normalize everything into roughly [0, 1] for numerical stability.
  double largest = 0.0;
  for (uint32_t k = 0; k < A.rowIndex.size(); k++)
    largest = std::max(largest, std::fabs((double) A.value[k]));
  for (uint32_t i = 0; i < A.rows; i++)
    largest = std::max(largest, std::fabs((double) b[i]));
  const double scale = largest > 0.0 ? 1.0 / largest : 1.0;

  for (uint32_t i = 0; i < A.rows; i++) {
    if (rows.start[i] == rows.start[i + 1])
      continue;
    operations_research::MPConstraint* constraint =
        solver.MakeRowConstraint((double) b[i] * scale, solver.infinity());
    for (uint32_t k = rows.start[i]; k < rows.start[i + 1]; k++)
      constraint->SetCoefficient(variables[rows.column[k]], (double) rows.value[k] * scale);
  }

  if (solver.Solve() != operations_research::MPSolver::OPTIMAL)
    return std::numeric_limits<double>::quiet_NaN();
  return objective->Value();
}

// One solve with x in [0, cap] and sum(c_r x_r) <= cap. `timeLimitSeconds`
// <= 0 means no limit.
Result solveWithCap(const Matrix &A, const RowMajor &rows, std::span<const int64_t> b,
                    std::span<const int64_t> c, const Options &options,
                    int64_t cap, double timeLimitSeconds) {
  Result result;

  sat::CpModelBuilder model;
  std::vector<sat::IntVar> variables;
  variables.reserve(A.cols);
  for (uint32_t r = 0; r < A.cols; r++)
    variables.push_back(model.NewIntVar(Domain(0, cap)));

  std::vector<sat::IntVar> terms;
  std::vector<int64_t> coefficients;
  for (uint32_t i = 0; i < A.rows; i++) {
    terms.clear();
    coefficients.clear();
    for (uint32_t k = rows.start[i]; k < rows.start[i + 1]; k++) {
      if (rows.value[k] == 0)
        continue;
      terms.push_back(variables[rows.column[k]]);
      coefficients.push_back(rows.value[k]);
    }
    if (terms.empty()) {
      // An empty row reads 0 >= b[i].
      // Either useless or makes the whole plan infeasible.
      if (b[i] > 0) {
        result.status = PlanStatus::INFEASIBLE;
        return result;
      }
      continue;
    }
    model.AddGreaterOrEqual(
        sat::LinearExpr::WeightedSum(absl::Span<const sat::IntVar>(terms),
                                     absl::Span<const int64_t>(coefficients)),
        b[i]);
  }

  terms.clear();
  coefficients.clear();
  for (uint32_t r = 0; r < A.cols; r++) {
    if (c[r] == 0)
      continue;
    terms.push_back(variables[r]);
    coefficients.push_back(c[r]);
  }
  if (terms.empty()) {
    // Nothing to optimize; x = 0 is optimal.
    result.status = PlanStatus::OK;
    result.provenOptimal = true;
    result.x.assign(A.cols, 0);
    return result;
  }

  const sat::LinearExpr objective =
      sat::LinearExpr::WeightedSum(absl::Span<const sat::IntVar>(terms),
                                   absl::Span<const int64_t>(coefficients));
  model.Minimize(objective);
  model.AddLessOrEqual(objective, cap);

  sat::SatParameters parameters;
  if (timeLimitSeconds > 0)
    parameters.set_max_time_in_seconds(timeLimitSeconds);
  parameters.set_relative_gap_limit(options.relativeGap);
  if (options.absoluteGap > 0)
    parameters.set_absolute_gap_limit((double) options.absoluteGap);
  if (options.numWorkers > 0)
    parameters.set_num_search_workers(options.numWorkers);
  if (options.randomSeed != 0)
    parameters.set_random_seed(options.randomSeed);
  parameters.set_log_search_progress(false);

  sat::CpSolverResponse response;
  try {
    response = sat::SolveWithParameters(model.Build(), parameters);
  } catch (...) {
    // OR-Tools reports malformed models with exceptions in some builds.
    result.status = PlanStatus::NUMERICAL_FAIL;
    return result;
  }

  fillStatus(response, result);
  result.bestBound = response.best_objective_bound();
  result.numConflicts = response.num_conflicts();
  result.numBranches = response.num_branches();

  if (result.status == PlanStatus::OK) {
    result.x.resize(A.cols);
    for (uint32_t r = 0; r < A.cols; r++)
      result.x[r] = response.solution((int) r);
    result.objective = (int64_t) std::llround(response.objective_value());
    // CP-SAT scales the objective internally, so it can call an incumbent
    // optimal while its own bound still sits a little under it. Do not
    // over-claim in that case, and only then report a gap.
    if (result.provenOptimal &&
        response.objective_value() - response.best_objective_bound() > 0.5)
      result.provenOptimal = false;
    if (!result.provenOptimal) {
      const double denominator = std::max(1.0, std::fabs(response.objective_value()));
      result.gap = (response.objective_value() - response.best_objective_bound()) / denominator;
    }
  }
  return result;
}

}  // namespace

// `c` is always all-ones currently, kept for possible later refactoring.
// TODO: Remove it when the design stabilizes.
Result solve(const Matrix &A, std::span<const int64_t> b,
             std::span<const int64_t> c, const Options &options) {
  // Check validity of A.
  // Should be alright so let's not include this in release mode.
  Result result;
#ifndef NDEBUG
  if (A.rows != b.size() || A.cols != c.size() ||
      A.colStart.size() != (size_t) A.cols + 1 ||
      A.rowIndex.size() != A.value.size())
    return result;

  for (uint32_t k = 0; k < A.rowIndex.size(); k++) {
    if (A.rowIndex[k] >= A.rows)
      return result;
  }
#endif

  // A row that demands something but has no way to produce it is infeasible no
  // matter what the cap is. Prune them away first.
  std::vector<uint8_t> hasProducer((size_t) A.rows, 0);
  for (uint32_t k = 0; k < A.rowIndex.size(); k++) {
    if (A.value[k] > 0)
      hasProducer[A.rowIndex[k]] = 1;
  }
  for (uint32_t i = 0; i < A.rows; i++)
    if (b[i] > 0 && !hasProducer[i]) {
      result.status = PlanStatus::INFEASIBLE;
      return result;
    }

  const int64_t ceiling = absoluteCap(A, c);
  const RowMajor rows = transpose(A);

  int64_t cap = ceiling;
  if (options.objectiveCap > 0) {
    cap = options.objectiveCap;
  } else {
    // Seed from the LP relaxation. Cheap enough, just a few milliseconds.
    // LP gives a lower bound, so our first cap attempt can be just a bit above it,
    // hence the `+ max(64, relaxation/8)`.
    const double relaxation = relaxationValue(A, rows, b, c);
    if (searchDebug())
      fprintf(stderr, "[solver] relaxation=%.6g\n", relaxation);

    if (std::isfinite(relaxation) && relaxation >= 0.0)
      cap = (int64_t) std::ceil(relaxation) + std::max<int64_t>(64, (int64_t) relaxation / 8);
    else
      cap = lowerBoundOnTotal(A, b);
  }
  cap = std::clamp<int64_t>(cap, 1, ceiling);
  
  const bool limited = options.maxTimeSeconds > 0;
  const auto started = std::chrono::steady_clock::now();
  const auto elapsedSeconds = [&started] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };

  Result best;
  bool haveBest = false;
  const bool debug = searchDebug();
  int attempts = 0;
  int infeasibleAttempts = 0;
  int attempt = 0;
  for (; attempt < MAX_ATTEMPTS; attempt++) {
    double budget = -1.0;
    if (limited) {
      const double remaining = options.maxTimeSeconds - elapsedSeconds();
      if (remaining <= 0.0)
        break;
      // Hand each attempt everything that is left rather than a slice: a
      // too-small cap is normally proven infeasible quickly, and reserving time
      // for a retry measurably loses more than it gains on these instances.
      budget = remaining;
    }

    Result current = solveWithCap(A, rows, b, c, options, cap, budget);
    if (debug)
      std::fprintf(stderr,
                   "[solver] attempt=%d cap=%lld budget=%.2f status=%d obj=%lld "
                   "bound=%.0f conflicts=%lld branches=%lld\n",
                   attempt, (long long) cap, budget, (int) current.status,
                   (long long) current.objective, current.bestBound,
                   (long long) current.numConflicts, (long long) current.numBranches);
    if (current.status == PlanStatus::OK) {
      if (!haveBest || current.objective < best.objective) {
        best = std::move(current);
        haveBest = true;
      }
      // The cap did not bind, so no cheaper plan exists beyond it: the answer
      // is optimal for the problem, not just for the box.
      if (best.objective < cap)
        return best;
    } else if (current.status == PlanStatus::NUMERICAL_FAIL) {
      return current;
    } else if (current.status == PlanStatus::INFEASIBLE) {
      ++infeasibleAttempts;
    }

    attempts = attempt + 1;
    if (cap >= ceiling)
      break;
    cap = std::min(cap * CAP_GROWTH, ceiling);
  }

  if (haveBest)
    return best;

  if (infeasibleAttempts == attempts && attempts > 0 && cap >= ceiling) {
    result.status = PlanStatus::INFEASIBLE;
    return result;
  }

  // Nothing usable yet. A cap at the ceiling is both the only place where
  // infeasibility can be concluded (a smaller cap being infeasible just means
  // the cap was too small) and the loosest box, which is the most likely to
  // admit some plan. Spend whatever is left on one solve there.
  const double remaining = limited ? options.maxTimeSeconds - elapsedSeconds() : -1.0;
  if (!limited || remaining > 0.0) {
    const Result last = solveWithCap(A, rows, b, c, options, ceiling, remaining);
    if (last.status == PlanStatus::OK)
      return last;
    if (last.status == PlanStatus::INFEASIBLE) {
      result.status = PlanStatus::INFEASIBLE;
      return result;
    }
    if (last.status == PlanStatus::NUMERICAL_FAIL)
      return last;
  }

  // The budget ran out before a plan was found.
  result.status = PlanStatus::ITER_LIMIT;
  return result;
}

}  // namespace aw::solver
