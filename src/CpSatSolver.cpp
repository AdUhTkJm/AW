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
constexpr int MAX_ATTEMPTS = 16;

// Slack on the reduced-cost test, so LP numerical noise never fixes a column
// that could still appear in the optimum.
constexpr double REDUCED_COST_EPSILON = 1e-6;

// Set AW_SOLVER_DEBUG to get the cap search traced to stderr.
bool searchDebug() {
  static const bool enabled = std::getenv("AW_SOLVER_DEBUG") != nullptr;
  return enabled;
}

// AW_SAT_* overrides exist for parameter sweeps during development. A negative
// value (also the "unset" sentinel) keeps the compiled-in default.
int envInt(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? -1 : (int) std::strtol(value, nullptr, 10);
}

double envDouble(const char *name, double fallback) {
  const char *value = std::getenv(name);
  return value == nullptr ? fallback : std::strtod(value, nullptr);
}

// Same as abs, but avoids the std::abs(INT64_MIN) trap.
uint64_t magnitude(int64_t value) {
  return value < 0 ? (uint64_t) (-(value + 1)) + 1 : (uint64_t) value;
}

uint64_t satAdd(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

uint64_t satMul(uint64_t a, uint64_t b) {
  return a != 0 && b > UINT64_MAX / a ? UINT64_MAX : a * b;
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

// One LP relaxation of the balance problem. It serves two purposes: the
// objective value seeds the first objective cap, and the reduced costs feed the
// fixing pass in `solve`.
struct LpResult {
  bool ok = false;
  double value = 0.0;
  std::vector<double> reducedCost;  // A.cols entries
};

LpResult lpRelaxation(const Matrix &A, const RowMajor &rows,
                      std::span<const int64_t> b, std::span<const int64_t> c) {
  LpResult out;
  out.reducedCost.assign(A.cols, 0.0);

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

  // Normalize everything into roughly [0, 1] for numerical stability. Row
  // scaling leaves the reduced costs unchanged: the dual absorbs the factor.
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
    return out;

  out.value = objective->Value();
  for (uint32_t r = 0; r < A.cols; r++)
    out.reducedCost[r] = variables[r]->reduced_cost();
  out.ok = true;
  return out;
}

// A copy of `A` that keeps only the columns in `keep` (ascending), together
// with the matching objective coefficients.
Matrix selectColumns(const Matrix &A, std::span<const int64_t> c,
                     const std::vector<uint32_t> &keep, std::vector<int64_t> &outC) {
  Matrix out;
  out.rows = A.rows;
  out.cols = (uint32_t) keep.size();
  out.colStart.reserve(keep.size() + 1);
  out.colStart.push_back(0);
  outC.clear();
  outC.reserve(keep.size());
  for (uint32_t r : keep) {
    for (uint32_t k = A.colStart[r]; k < A.colStart[r + 1]; k++) {
      out.rowIndex.push_back(A.rowIndex[k]);
      out.value.push_back(A.value[k]);
    }
    out.colStart.push_back((uint32_t) out.rowIndex.size());
    outC.push_back(c[r]);
  }
  return out;
}

// Re-expands a solution over the kept columns into the full column space, so
// callers always see one count per recipe with zeros at the fixed columns.
std::vector<int64_t> expandSolution(std::span<const int64_t> reduced,
                                    const std::vector<uint32_t> &keep, uint32_t cols) {
  std::vector<int64_t> full(cols, 0);
  for (size_t k = 0; k < keep.size(); k++)
    full[keep[k]] = reduced[k];
  return full;
}

// Upper bound for every column.
//
// A costed column obeys c_r x_r <= c^T x <= cap, so `cap` bounds it. A
// zero-cost column is not bounded by the objective. It is still bounded in any
// optimum, though: producing more of a tag than the plan consumes is waste, so
// there is always an optimal solution with
//
//   x_{T<-m} <= (b_T + sum_{consumers} amount * cap) / p_{T<-m}.
//
// That is the tag's *natural cap*. It is not implied by the `>=` row above
// (which lets the column overproduce), but capping at it never removes an
// optimum. The row capacity is computed once and shared by every member edge,
// so a tag with thousands of members stays linear. `ceiling` is the
// int64-range fallback for a zero-cost column that produces into no row.
std::vector<int64_t> columnDomains(const Matrix &A, const RowMajor &rows,
                                   std::span<const int64_t> b, std::span<const int64_t> c,
                                   int64_t cap, int64_t ceiling,
                                   std::span<const int64_t> upper) {
  std::vector<int64_t> domain((size_t) A.cols);
  for (uint32_t r = 0; r < A.cols; r++) {
    int64_t hi = c[r] != 0 ? cap : ceiling;
    if (!upper.empty() && upper[r] < hi)
      hi = upper[r];
    domain[r] = hi;
  }

  // Only rows that a zero-cost column produces into need a capacity. Their
  // negative entries are the consumers, which in the planner are costed
  // columns, so `cap` bounds them. A zero-cost consumer (a nested tag) is left
  // at `ceiling`, which only over-estimates the capacity and stays valid.
  std::vector<uint8_t> needsCapacity((size_t) A.rows, 0);
  for (uint32_t r = 0; r < A.cols; r++) {
    if (c[r] != 0)
      continue;
    for (uint32_t k = A.colStart[r]; k < A.colStart[r + 1]; k++)
      if (A.value[k] > 0)
        needsCapacity[A.rowIndex[k]] = 1;
  }

  std::vector<uint64_t> rowCapacity((size_t) A.rows, 0);
  for (uint32_t row = 0; row < A.rows; row++) {
    if (!needsCapacity[row])
      continue;
    // The row's demand plus the most every consumer can take. A negative b
    // only lowers it, so clamping at 0 is safe.
    uint64_t consumption = b[row] > 0 ? (uint64_t) b[row] : 0;
    for (uint32_t j = rows.start[row]; j < rows.start[row + 1]; j++) {
      if (rows.value[j] >= 0)
        continue;
      consumption = satAdd(consumption,
                           satMul(magnitude(rows.value[j]),
                                  (uint64_t) domain[rows.column[j]]));
    }
    rowCapacity[row] = consumption;
  }

  for (uint32_t r = 0; r < A.cols; r++) {
    if (c[r] != 0 || domain[r] == 0)
      continue;
    int64_t best = domain[r];
    for (uint32_t k = A.colStart[r]; k < A.colStart[r + 1]; k++) {
      const int64_t coefficient = A.value[k];
      if (coefficient <= 0)
        continue;
      const uint64_t candidate = rowCapacity[A.rowIndex[k]] / (uint64_t) coefficient;
      if (candidate < (uint64_t) best)
        best = (int64_t) candidate;
    }
    domain[r] = best;
  }
  return domain;
}

// One solve with x in [0, domain[r]], x_r <= upper[r] (when given) and
// sum(c_r x_r) <= cap. `ceiling` is the fallback domain for zero-cost columns.
// `timeLimitSeconds` <= 0 means no limit.
Result solveWithCap(const Matrix &A, const RowMajor &rows, std::span<const int64_t> b,
                    std::span<const int64_t> c, const Options &options,
                    int64_t cap, int64_t ceiling, double timeLimitSeconds,
                    std::span<const int64_t> upper = {}) {
  Result result;

  const std::vector<int64_t> domain = columnDomains(A, rows, b, c, cap, ceiling, upper);

  sat::CpModelBuilder model;
  std::vector<sat::IntVar> variables;
  variables.reserve(A.cols);
  for (uint32_t r = 0; r < A.cols; r++)
    variables.push_back(model.NewIntVar(Domain(0, domain[r])));

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
  if (!terms.empty()) {
    const sat::LinearExpr objective =
        sat::LinearExpr::WeightedSum(absl::Span<const sat::IntVar>(terms),
                                     absl::Span<const int64_t>(coefficients));
    model.Minimize(objective);
    model.AddLessOrEqual(objective, cap);
  }
  // With every objective coefficient zero there is nothing to optimize, but the
  // constraints still have to be met, so fall through to the solver rather than
  // claiming x = 0 is optimal.

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

  // Tune parameters to tell the solver that this comes from LP.
  parameters.set_linearization_level(2);
  parameters.set_search_branching(operations_research::sat::SatParameters::LP_SEARCH);
  parameters.set_use_feasibility_pump(true);

  // Flash mode: hand back the first integer-feasible solution and stop. The
  // cap still constrains the objective, so the model stays bounded, but no
  // optimality is pursued or proven. See Options::flash.
  parameters.set_stop_after_first_solution(options.flash);

  // For instances large enough, presolving actually harms.
  if (A.cols > 30000)
    parameters.set_cp_model_presolve(false);

  // Development overrides for parameter sweeps (see envInt above).
  if (const int value = envInt("AW_SAT_LINEARIZATION"); value >= 0)
    parameters.set_linearization_level(value);
  if (const int value = envInt("AW_SAT_BRANCHING"); value >= 0)
    parameters.set_search_branching((sat::SatParameters::SearchBranching) value);
  if (const int value = envInt("AW_SAT_FP"); value >= 0)
    parameters.set_use_feasibility_pump(value != 0);
  if (const int value = envInt("AW_SAT_LNS"); value >= 0)
    parameters.set_use_lns(value != 0);
  if (const int value = envInt("AW_SAT_LNS_ONLY"); value >= 0)
    parameters.set_use_lns_only(value != 0);
  if (const int value = envInt("AW_SAT_PRESOLVE"); value >= 0)
    parameters.set_cp_model_presolve(value != 0);
  if (const int value = envInt("AW_SAT_PROBING"); value >= 0)
    parameters.set_cp_model_probing_level(value);
  if (const int value = envInt("AW_SAT_SYMMETRY"); value >= 0)
    parameters.set_symmetry_level(value);
  if (const int value = envInt("AW_SAT_RANDOMIZE"); value >= 0)
    parameters.set_randomize_search(value != 0);
  if (const int value = envInt("AW_SAT_CUTS"); value >= 0)
    parameters.set_max_num_cuts(value);
  if (const int value = envInt("AW_SAT_PRESOLVE_ITERS"); value >= 0)
    parameters.set_max_presolve_iterations(value);
  if (const int value = envInt("AW_SAT_OBJLB"); value >= 0)
    parameters.set_use_objective_lb_search(value != 0);
  if (const int value = envInt("AW_SAT_OBJSHAVE"); value >= 0)
    parameters.set_use_objective_shaving_search(value != 0);
  if (envInt("AW_SAT_LOG") > 0) {
    parameters.set_log_search_progress(true);
  }
  if (const int value = envInt("AW_SAT_GAP_X1000"); value >= 0)
    parameters.set_relative_gap_limit((double) value / 1000.0);
  if (const int value = envInt("AW_SAT_ABS_GAP"); value >= 0)
    parameters.set_absolute_gap_limit((double) value);

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

  const RowMajor rows = transpose(A);

  // One LP relaxation serves two purposes: the objective cap (as before) and
  // the reduced costs used by the fixing pass below.
  const bool needRelaxation = options.objectiveCap == 0 ||
                              envDouble("AW_RC_GAP", options.reducedCostGap) > 0.0;
  const auto relaxationStart = std::chrono::steady_clock::now();
  const LpResult lp = needRelaxation ? lpRelaxation(A, rows, b, c) : LpResult{};
  if (searchDebug())
    fprintf(stderr, "[solver] relaxation=%.6g ok=%d time=%.3f\n", lp.value, (int) lp.ok,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - relaxationStart)
                .count());

  const int64_t ceiling = absoluteCap(A, c);

  int64_t cap = ceiling;
  if (options.objectiveCap > 0) {
    cap = options.objectiveCap;
  } else if (lp.ok && std::isfinite(lp.value) && lp.value >= 0.0) {
    // LP gives a lower bound, so our first cap attempt can be just a bit above
    // it, hence the `+ max(64, lp/8)`.
    cap = (int64_t) std::ceil(lp.value) + std::max<int64_t>(64, (int64_t) lp.value / 8);
  } else {
    cap = lowerBoundOnTotal(A, b);
  }
  cap = std::clamp<int64_t>(cap, 1, ceiling);

  const bool limited = options.maxTimeSeconds > 0;
  const auto started = std::chrono::steady_clock::now();
  const auto elapsedSeconds = [&started] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };
  const bool debug = searchDebug();

  // Reduced-cost fixing. Probe for any plan within `reducedCostGap` of the LP
  // bound: a feasible answer gives an incumbent tight enough for the reduced
  // costs to fix columns, while an infeasible answer proves the integrality
  // gap is too wide to bother. See docs/algorithm.typ.
  if (!options.flash && envDouble("AW_RC_GAP", options.reducedCostGap) > 0.0 && lp.ok &&
      std::isfinite(lp.value) && lp.value >= 0.0 && A.cols > 0) {
    const int64_t probeCap = std::clamp<int64_t>(
        (int64_t) std::ceil(lp.value + envDouble("AW_RC_GAP", options.reducedCostGap)), 1,
        ceiling);
    if (probeCap < cap) {
      double probeBudget = -1.0;
      if (limited) {
        const double left = options.maxTimeSeconds - elapsedSeconds();
        if (left > 0.0)
          probeBudget = std::min(left, std::max(0.005, left * 0.25));
      }
      if (!limited || probeBudget > 0.0) {
        const Result probe = solveWithCap(A, rows, b, c, options, probeCap, ceiling, probeBudget);
        if (debug)
          std::fprintf(stderr, "[solver] probe cap=%lld budget=%.2f status=%d obj=%lld t=%.3f\n",
                       (long long) probeCap, probeBudget, (int) probe.status,
                       (long long) probe.objective, elapsedSeconds());
        if (probe.status == PlanStatus::NUMERICAL_FAIL)
          return probe;
        // A plan strictly below the probe cap means the cap did not bind, so it
        // is already optimal for the uncapped problem.
        if (probe.status == PlanStatus::OK && probe.provenOptimal &&
            probe.objective < probeCap)
          return probe;
        if (probe.status == PlanStatus::OK &&
            (double) probe.objective >= lp.value) {
          // At an LP optimum `c^T x = LP + sum_r d_r x_r` for every feasible
          // x, so every plan with `c^T x <= incumbent` obeys
          // `x_r <= floor((incumbent - LP) / d_r)`. A zero bound drops the
          // column outright; a positive one only tightens its domain. The
          // guard above keeps a numerical overshoot of the LP bound from
          // making this too eager, and the fixing boundary keeps an extra
          // unit of slack.
          const double threshold = (double) probe.objective - lp.value;
          std::vector<uint32_t> keep;
          keep.reserve(A.cols);
          std::vector<int64_t> keepUpper;
          keepUpper.reserve(A.cols);
          uint32_t fixed = 0;
          bool tightened = false;
          for (uint32_t r = 0; r < A.cols; r++) {
            const double d = lp.reducedCost[r];
            // Costed columns are bounded by the objective; zero-cost columns
            // are not, so their neutral value is the absolute ceiling. The
            // natural cap in `columnDomains` only tightens them further.
            const int64_t natural = c[r] != 0 ? probe.objective : ceiling;
            int64_t bound = natural;
            if (d > threshold + REDUCED_COST_EPSILON) {
              bound = 0;
            } else if (d > REDUCED_COST_EPSILON) {
              bound = (int64_t) std::floor(threshold / d);
              // Not fixed above, so stay at least one execution to remain
              // conservative at the boundary.
              if (bound < 1)
                bound = 1;
              // `probe.objective` bounds a costed column, but not a free one.
              if (c[r] != 0)
                bound = std::min(bound, probe.objective);
            }
            if (bound <= 0) {
              fixed++;
              continue;
            }
            if (bound < natural)
              tightened = true;
            keep.push_back(r);
            keepUpper.push_back(bound);
          }
          if (fixed > 0 || tightened) {
            std::vector<int64_t> reducedC;
            const Matrix reduced = selectColumns(A, c, keep, reducedC);
            const RowMajor reducedRows = transpose(reduced);
            double budget = -1.0;
            if (limited)
              budget = options.maxTimeSeconds - elapsedSeconds();
            if (!limited || budget > 0.0) {
              Result out = solveWithCap(reduced, reducedRows, b, reducedC, options,
                                        probe.objective, ceiling, budget, keepUpper);
              if (debug)
                std::fprintf(stderr,
                             "[solver] reduced cols=%u fixed=%u status=%d obj=%lld\n",
                             reduced.cols, fixed, (int) out.status,
                             (long long) out.objective);
              if (out.status == PlanStatus::OK) {
                out.x = expandSolution(out.x, keep, A.cols);
                out.fixedColumns = fixed;
                return out;
              }
              if (out.status == PlanStatus::NUMERICAL_FAIL)
                return out;
            }
            // Out of time, or the reduced solve found nothing usable: the probe
            // is still a valid plan.
            return probe;
          }
        }
      }
    }
  }

  Result best;
  bool haveBest = false;
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

    Result current = solveWithCap(A, rows, b, c, options, cap, ceiling, budget);
    if (debug)
      std::fprintf(stderr,
                   "[solver] attempt=%d cap=%lld budget=%.2f status=%d obj=%lld "
                   "bound=%.0f conflicts=%lld branches=%lld t=%.3f\n",
                   attempt, (long long) cap, budget, (int) current.status,
                   (long long) current.objective, current.bestBound,
                   (long long) current.numConflicts, (long long) current.numBranches,
                   elapsedSeconds());
    if (current.status == PlanStatus::OK) {
      // Flash mode takes the first feasible plan whatever it costs; there is
      // no incumbent to compare it against and no larger cap to try.
      if (options.flash)
        return current;
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
    const Result last = solveWithCap(A, rows, b, c, options, ceiling, ceiling, remaining);
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
