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
constexpr int64_t kCapGrowth = 8;
constexpr int kMaxAttempts = 8;

// Set AW_SOLVER_DEBUG to get the cap search traced to stderr.
bool searchDebug() {
  static const bool enabled = std::getenv("AW_SOLVER_DEBUG") != nullptr;
  return enabled;
}

// Avoids the std::abs(INT64_MIN) trap.
uint64_t magnitude(int64_t value) {
  return value < 0 ? (uint64_t) (-(value + 1)) + 1 : (uint64_t) value;
}

uint64_t saturatingAdd(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

int64_t ceilDiv(int64_t numerator, int64_t denominator) {
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// The largest value every variable may take while every constraint's and the
// objective's worst-case activity still fits in int64. Only used as the final
// ceiling on the growing cap, and as the domain when the costs make the cap
// argument unsound.
int64_t activitySafeBound(const Matrix &A, std::span<const int64_t> c) {
  std::vector<uint64_t> rowAbs((size_t) A.rows, 0);
  for (size_t k = 0; k < A.rowIndex.size(); k++)
    rowAbs[A.rowIndex[k]] = saturatingAdd(rowAbs[A.rowIndex[k]], magnitude(A.value[k]));

  uint64_t maxRow = 0;
  for (uint64_t value : rowAbs)
    maxRow = std::max(maxRow, value);

  uint64_t objectiveAbs = 0;
  for (int64_t value : c)
    objectiveAbs = saturatingAdd(objectiveAbs, magnitude(value));

  // Leave some room for arithmetic in presolver.
  const uint64_t limit = (uint64_t) INT64_MAX / 2;
  uint64_t bound = limit;
  if (maxRow != 0)
    bound = std::min(bound, limit / maxRow);
  if (objectiveAbs != 0)
    bound = std::min(bound, limit / objectiveAbs);
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

// The cap is a cap on sum(c_r x_r), which only bounds x_r when every cost is
// at least 1. The planner always passes all-ones, so the general case just
// falls back to a single uncapped solve.
bool costsBoundVariables(std::span<const int64_t> c) {
  for (int64_t value : c)
    if (value < 1)
      return false;
  return true;
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

// The LP relaxation's optimum, used only to seed the objective cap. It is a
// lower bound on the integer optimum, so a cap derived from it is normally
// just below the answer, and one growth step past it lands close. Returns 0
// when GLOP cannot solve the relaxation, in which case the cap starts from the
// trivial lower bound and grows instead.
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

  // GLOP does not like a model whose coefficients span many orders of
  // magnitude, and a target of 1e9 against a coefficient of 1e5 is exactly
  // that. Scaling a row by a positive factor leaves the feasible set alone, so
  // normalise everything into roughly [0, 1].
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

Result solve(const Matrix &A, std::span<const int64_t> b,
             std::span<const int64_t> c, const Options &options) {
  Result result;
  if (A.rows != b.size() || A.cols != c.size() ||
      A.colStart.size() != (size_t) A.cols + 1 ||
      A.rowIndex.size() != A.value.size())
    return result;

  for (uint32_t k = 0; k < A.rowIndex.size(); k++)
    if (A.rowIndex[k] >= A.rows)
      return result;

  // A row that demands something but has no way to produce it is infeasible no
  // matter what the cap is. Catching it here keeps the answer exact instead of
  // depending on how far the cap grew.
  {
    std::vector<uint8_t> hasProducer((size_t) A.rows, 0);
    for (uint32_t k = 0; k < A.rowIndex.size(); k++)
      if (A.value[k] > 0)
        hasProducer[A.rowIndex[k]] = 1;
    for (uint32_t i = 0; i < A.rows; i++)
      if (b[i] > 0 && !hasProducer[i]) {
        result.status = PlanStatus::INFEASIBLE;
        return result;
      }
  }

  // The cap argument needs every cost to be at least 1. When it is not, fall
  // back to the widest representable domain and a single solve.
  const bool capped = costsBoundVariables(c);
  const int64_t ceiling = activitySafeBound(A, c);
  const RowMajor rows = transpose(A);

  int64_t cap = ceiling;
  if (capped) {
    if (options.objectiveCap > 0) {
      cap = options.objectiveCap;
    } else {
      // Seed from the LP relaxation: it is cheap, it tolerates the amplifying
      // cycles that make a per-item cost estimate useless, and being a lower
      // bound it is at worst one growth step away from a usable cap.
      const double relaxation = relaxationValue(A, rows, b, c);
      if (searchDebug())
        std::fprintf(stderr, "[solver] relaxation=%.6g\n", relaxation);
      if (std::isfinite(relaxation) && relaxation >= 0.0)
        cap = (int64_t) std::ceil(relaxation) + std::max<int64_t>(64, (int64_t) relaxation / 8);
      else
        cap = lowerBoundOnTotal(A, b);
    }
    cap = std::min(std::max<int64_t>(cap, 1), ceiling);
  }

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
  for (; attempt < (capped ? kMaxAttempts : 1); attempt++) {
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
    if (!capped || cap >= ceiling)
      break;
    cap = std::min(cap * kCapGrowth, ceiling);
  }

  if (haveBest)
    return best;

  const bool everyAttemptInfeasible = infeasibleAttempts == attempts && attempts > 0;
  if (everyAttemptInfeasible && (!capped || cap >= ceiling)) {
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
