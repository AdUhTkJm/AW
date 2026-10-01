#include "aw/utils/Int128.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace aw {
namespace {

struct ColumnEntry {
  uint32_t row;
  int64_t value;
};

// A greedy plan can be wildly above the optimum: a single bad route choice can
// multiply demand along a long conversion chain (hundreds of millions of
// executions on the ATM corpus). Such a plan is still a valid answer, but it is
// useless to hand back in flash mode and dangerous to seed the solver with, so
// only plans within this factor of a trivial lower bound are used.
constexpr int64_t GREEDY_QUALITY_FACTOR = 64;

int64_t ceilDiv(int64_t numerator, int64_t denominator) {
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// A valid lower bound on the optimum: row i needs b_i, so at least
// ceil(b_i / maxPositive[i]) executions are needed. Same bound as the solver's
// own `lowerBoundOnTotal`.
int64_t trivialLowerBound(const solver::Matrix &A, std::span<const int64_t> b) {
  aw::vector<int64_t> maxPositive = aw::vector<int64_t>::zeroes(A.rows);
  for (uint32_t k = 0; k < A.rowIndex.size(); k++) {
    const int64_t value = A.value[k];
    if (value > 0 && value > maxPositive[A.rowIndex[k]])
      maxPositive[A.rowIndex[k]] = value;
  }
  int64_t bound = 1;
  for (uint32_t i = 0; i < A.rows; i++)
    if (b[i] > 0 && maxPositive[i] > 0) {
      const int64_t candidate = ceilDiv(b[i], maxPositive[i]);
      if (candidate > bound)
        bound = candidate;
    }
  return bound;
}

}  // namespace

// The problem is
//
//   min   sum_{real r} x_r
//   s.t.  produced(i) - consumed(i) >= b_i        for every item i
//         x_r >= 0, x_r integer
//
// Here x_r is the number of times recipe `r` is executed. A tag edge (a recipe
// whose output is a synthetic tag node) costs nothing: choosing which member
// fills a tag slot is free in-game. It is given a natural upper bound by the
// solver instead -- it can never usefully exceed what the plan consumes of its
// tag -- so the free column stays finite under the `>=` row above.
//
// b_target = amount and b_i = -inventory(i) for every other item.
PlanResult planCrafting(const Subgraph &sub, NodeId target, Amount amount,
                        std::span<const Amount> invSrc,
                        const solver::Options &options) {
  PlanResult result;

  const BaseCraftingGraph &g = sub.graph;
  if (target >= g.nItem)
    return result;

  const uint32_t n = g.nRecipe;
  const uint32_t m = g.nItem;

  // One balance row per item:
  //   produced(i) - consumed(i) >= b_i
  // A recipe r adds +outputAmt at its output row and -inputAmt at each input
  // row. Inputs are ascending in the subgraph, but the output row may collide
  // with one of them, so merge equal rows.
  solver::Matrix A;
  A.rows = m;
  A.cols = n;
  A.colStart.reserve((size_t) n + 1);
  A.colStart.push_back_unchecked(0);
  // Each recipe contributes its output row plus one row per input, before the
  // per-column merge; the merge can only drop rows, so this bounds the total.
  const size_t entryCap = (size_t) n + g.r2i.targets.size();
  A.rowIndex.reserve(entryCap);
  A.value.reserve(entryCap);
  aw::vector<ColumnEntry> column;
  // Note that our matrix is column-major, so we're processing column-by-column here.
  for (uint32_t r = 0; r < n; r++) {
    // Push "produced(i) - consumed(i)".
    column.clear();
    column.push_back({g.output[r], g.outputAmt[r]});
    const auto inputs = g.inputsOf(r);
    const auto weights = g.inputAmountsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      column.push_back({inputs[k], -weights[k]});

    std::sort(column.begin(), column.end(),
              [](const ColumnEntry &lhs, const ColumnEntry &rhs) {
                return lhs.row < rhs.row;
              });

    // Merge entries with the same row.
    size_t kept = 0;
    for (size_t k = 0; k < column.size();) {
      const uint32_t row = column[k].row;
      int64_t value = 0;
      while (k < column.size() && column[k].row == row) {
        value += column[k].value;
        k++;
      }
      if (value != 0) {
        column[kept].row = row;
        column[kept].value = value;
        kept++;
      }
    }

    // Push the column to A.
    for (size_t k = 0; k < kept; k++) {
      A.rowIndex.push_back_unchecked(column[k].row);
      A.value.push_back_unchecked(column[k].value);
    }
    A.colStart.push_back_unchecked((uint32_t) A.rowIndex.size());
  }

  // Push b[i].
  aw::vector<int64_t> rhs(m, 0);
  for (uint32_t i = 0; i < m; i++) {
    if (i == target) {
      // Always produce `amount` new units; the target inventory is not
      // subtracted. Recipes may still consume the target, so cycles through
      // the output remain available.
      rhs[i] = amount;
    } else {
      const NodeId source = sub.itemOrigin[i];
      const Amount available = source < invSrc.size() ? invSrc[source] : 0;
      rhs[i] = -available;
    }
  }

  // Tag edges are free. A recipe is a tag edge when it produces a non-real
  // node; those are exactly the synthetic member edges. Everything else costs
  // one crafting step. See include/aw/Solver.h for how the solver keeps the
  // free columns bounded without an equality.
  //
  // The real recipes are a prefix, so the rest of the vector stays at its
  // default zero cost. See the ordering note on BaseCraftingGraph::output.
  aw::vector<int64_t> objective = aw::vector<int64_t>::zeroes(n);
  for (uint32_t r = 0; r < n && g.output[r] < g.nReal; r++)
    objective[r] = 1;

  // Greedy DAG pre-pass. It is cheap (O(V + E)) and either returns a
  // balance-checked plan or nothing, so it is always run before the solver.
  // A hit is the whole answer in flash mode; otherwise it seeds the solver as
  // both the objective cap (a valid upper bound on the optimum) and a solution
  // hint.
  aw::vector<int64_t> greedy = greedyDagPlan(sub, target, amount, invSrc);
  solver::Options solveOptions = options;

  // Startup barriers, added lazily after a rejected plan, so an ordinary
  // feasible call pays nothing for the experiment. See Options::Barrier.
  aw::vector<int64_t> stockPerRow;
  const auto installBarriers = [&]() {
    if (!stockPerRow.empty())
      return;
    // `rhs` only carries stock for the non-target rows and hides the target's
    // behind the request, so read it from the caller's inventory instead.
    stockPerRow.resize(m, 0);
    for (uint32_t i = 0; i < m; i++) {
      const NodeId source = sub.itemOrigin[i];
      stockPerRow[i] = source < invSrc.size() ? (int64_t) invSrc[source] : 0;
    }
    solveOptions.stock = std::span<const int64_t>(stockPerRow.data(), stockPerRow.size());
  };

  bool greedyUsable = false;
  aw::int128 greedyCost = 0;
  if (!greedy.empty()) {
    for (uint32_t r = 0; r < n; r++)
      greedyCost += (aw::int128) objective[r] * greedy[r];
    greedyUsable = greedyCost > 0 && greedyCost <= (aw::int128) INT64_MAX;
  }
  if (greedyUsable) {
    if (options.flash) {
      result.status = PlanStatus::OK;
      result.provenOptimal = false;
      result.exec = std::move(greedy);
      // The greedy plan is acyclic, so this only ever confirms it; it is kept
      // so every returned plan passes the same check.
      if (!planIsFireable(sub, invSrc,
                          std::span<const int64_t>(result.exec.data(), result.exec.size())))
        result.status = PlanStatus::CYCLE_UNFULFILLED;
      return result;
    }
    if (greedyCost <= (aw::int128) trivialLowerBound(A, rhs) * GREEDY_QUALITY_FACTOR) {
      solveOptions.objectiveUpperBound = (int64_t) greedyCost;
      solveOptions.solutionHint = std::span<const int64_t>(greedy.data(), greedy.size());
    }
  }

  // Re-solve while the post-solve fireability check rejects the plan. Every
  // rejected vector becomes a no-good, so the next solve has to return a
  // different one; see solver::Options::noGoods and PlanStatus. The retries
  // share the caller's wall-clock budget instead of each getting a fresh one,
  // so a rejection cannot silently multiply the latency.
  const double totalBudget = solveOptions.maxTimeSeconds;
  const bool budgetLimited = totalBudget > 0.0;
  const auto retryStart = std::chrono::steady_clock::now();
  const auto budgetLeft = [&]() {
    return totalBudget - std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       retryStart)
                              .count();
  };
  int64_t conflicts = 0;
  int64_t branches = 0;
  for (int retry = 0;; retry++) {
    if (budgetLimited) {
      const double left = budgetLeft();
      if (left <= 0.0)
        return result;
      solveOptions.maxTimeSeconds = left;
    }
    const solver::Result solved = solver::solve(A, rhs, objective, solveOptions);
    conflicts += solved.numConflicts;
    branches += solved.numBranches;
    result.numConflicts = conflicts;
    result.numBranches = branches;
    if (solved.status != PlanStatus::OK) {
      // A retry that found nothing proves nothing about the original problem:
      // the no-goods exclude only the plans already rejected, so a retry's
      // INFEASIBLE (or ITER_LIMIT) says nothing about the rest. Keep the last
      // rejection, with its plan, instead of overwriting it. Only a first-solve
      // failure is reported with the solver's own status.
      if (result.status == PlanStatus::CYCLE_UNFULFILLED)
        return result;
      result.status = solved.status;
      result.bestBound = solved.bestBound;
      return result;
    }
    result.status = PlanStatus::OK;
    result.provenOptimal = solved.provenOptimal;
    result.gap = solved.gap;
    result.bestBound = solved.bestBound;
    result.fixedColumns = solved.fixedColumns;

    result.exec = solved.x;
    // The solver's balance is a net condition; refuse a plan that cannot be
    // turned into a firing sequence. See planIsFireable and PlanStatus.
    FireabilityWitness witness;
    if (planIsFireable(sub, invSrc,
                      std::span<const int64_t>(result.exec.data(), result.exec.size()),
                      &witness))
      return result;

    result.status = PlanStatus::CYCLE_UNFULFILLED;
    result.provenOptimal = false;
    if (retry >= solveOptions.maxCycleRetries)
      return result;

    // Record one startup barrier per blocked recipe: a sound cut that makes the
    // re-solve pay that recipe's seed. The exact no-good still goes in, so
    // progress is guaranteed even when the cut does not by itself exclude the
    // rejected vector.
    bool addedCut = false;
    for (size_t k = 0; k < witness.recipe.size(); k++) {
      const uint32_t column = witness.recipe[k];
      const uint32_t row = witness.item[k];
      bool seen = false;
      for (const solver::Options::Barrier &old : solveOptions.barriers)
        if (old.row == row && old.column == column) {
          seen = true;
          break;
        }
      if (seen)
        continue;
      solveOptions.barriers.push_back(
          solver::Options::Barrier{row, column, (int64_t) witness.need[k]});
      addedCut = true;
    }
    if (addedCut)
      installBarriers();

    // The rejected plan stays in `result.exec`, so a caller still sees what was
    // refused. Drop the warm start: it points at the vector just forbidden.
    solveOptions.noGoods.push_back(solved.x);
    solveOptions.solutionHint = {};
  }
}

}  // namespace aw
