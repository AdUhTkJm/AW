#include "aw/utils/Int128.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "aw/plan/Options.h"
#include "StartupCut.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace aw {
namespace {

// True when some posted group already covers exactly this set of columns, so a
// retry that reports the same deadlock does not post a duplicate. The sizes are
// compared first, which turns the subset test into an equality test.
bool hasEntryGroup(const aw::vector<solver::Options::EntryGroup> &groups,
                   std::span<const uint32_t> columns) noexcept {
  for (const solver::Options::EntryGroup &group : groups) {
    if (group.columns.size() != columns.size())
      continue;
    bool all = true;
    for (uint32_t column : group.columns) {
      bool found = false;
      for (uint32_t wanted : columns)
        if (wanted == column) {
          found = true;
          break;
        }
      if (!found) {
        all = false;
        break;
      }
    }
    if (all)
      return true;
  }
  return false;
}

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

// A valid lower bound on the objective c^T x: row i needs b_i, so firing
// producers of it costs at least `min over producers r of ceil(b_i / A[i][r])
// * c_r`. Every term is at least the single-row LP optimum b_i * min_r
// (c_r / A[i][r]), so the minimum of the terms is too, and the integer optimum
// is at least that LP optimum. With a uniform cost the minimum is attained by
// the largest producer and this is exactly `ceil(b_i / maxPositive[i])`.
//
// This is deliberately weak. A tighter estimate would need the LP relaxation:
// these graphs contain cycles that amplify (four planks from one log) with no
// "raw" base item, so a Bellman-Ford style cost relaxation, which needs
// something to seed from, reports the whole graph unreachable. The cap is
// therefore grown empirically instead.
int64_t costLowerBound(const solver::Matrix &A, std::span<const int64_t> b,
                       std::span<const int64_t> c) {
  // A free producer fills its row at no cost, so the row contributes nothing.
  aw::vector<uint8_t> freeRow = aw::vector<uint8_t>::zeroes(A.rows);
  aw::vector<aw::int128> best(A.rows, 0);
  aw::vector<uint8_t> seen = aw::vector<uint8_t>::zeroes(A.rows);
  for (uint32_t r = 0; r < A.cols; r++) {
    for (uint32_t k = A.colStart[r]; k < A.colStart[r + 1]; k++) {
      const int64_t value = A.value[k];
      if (value <= 0)
        continue;
      const uint32_t row = A.rowIndex[k];
      if (b[row] <= 0)
        continue;
      if (c[r] <= 0) {
        freeRow[row] = 1;
        continue;
      }
      const aw::int128 candidate = (aw::int128) ceilDiv(b[row], value) * (aw::int128) c[r];
      if (!seen[row] || candidate < best[row]) {
        seen[row] = 1;
        best[row] = candidate;
      }
    }
  }

  aw::int128 bound = 1;
  for (uint32_t i = 0; i < A.rows; i++) {
    if (freeRow[i] || !seen[i])
      continue;
    if (best[i] > bound)
      bound = best[i];
  }
  return bound > (aw::int128) INT64_MAX ? INT64_MAX : (int64_t) bound;
}

}  // namespace

// The problem is
//
//   min   sum_{real r} c_r x_r
//   s.t.  produced(i) - consumed(i) >= b_i        for every item i
//         x_r >= 0, x_r integer
//
// Here x_r is the number of times recipe `r` is executed, and c_r is what one
// execution of it costs, counted in executions of the underlying Minecraft
// recipe: 1 for an ordinary recipe, the batch size for a level of a chanced
// recipe (the mod folds a batch into one deterministic column, see
// ChanceBatching), and 0 for a tag edge. A tag edge (a recipe whose output is a
// synthetic tag node) costs nothing: choosing which member fills a tag slot is
// free in-game. It is given a natural upper bound by the solver instead -- it
// can never usefully exceed what the plan consumes of its tag -- so the free
// column stays finite under the `>=` row above.
//
// b_target = amount and b_i = -inventory(i) for every other item.
PlanResult planCrafting(const Subgraph &sub, ItemId target, Amount amount,
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
  // Each recipe contributes one row per output (anchor and byproducts) plus
  // one per input, before the per-column merge; the merge can only drop rows,
  // so this bounds the total. Counting one output per recipe would under-count
  // every byproduct recipe and overflow the two vectors below.
  const size_t entryCap = g.r2o.targets.size() + g.r2i.targets.size();
  A.rowIndex.reserve(entryCap);
  A.value.reserve(entryCap);
  aw::vector<ColumnEntry> column;
  // Note that our matrix is column-major, so we're processing column-by-column here.
  for (uint32_t r = 0; r < n; r++) {
    // Push "produced(i) - consumed(i)".
    column.clear();
    const auto outs = g.outputsOf(r);
    const auto outAmts = g.outputAmountsOf(r);
    for (size_t k = 0; k < outs.size(); k++)
      column.push_back({outs[k], outAmts[k]});
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
      const ItemId source = sub.itemOrigin[i];
      const Amount available = source < invSrc.size() ? invSrc[source] : 0;
      rhs[i] = -available;
    }
  }

  // Tag edges are free. A recipe is a tag edge when it produces a non-real
  // node; those are exactly the synthetic member edges. Everything else costs
  // what the blob said it costs, which is the batch size for a level of a
  // chanced recipe and 1 for an ordinary one. See BaseCraftingGraph::cost for
  // how the solver keeps a free column bounded without an equality.
  //
  // The real recipes are a prefix, so the rest of the vector stays at its
  // default zero cost. See the ordering note on BaseCraftingGraph::output.
  aw::vector<int64_t> objective = aw::vector<int64_t>::zeroes(n);
  for (uint32_t r = 0; r < n && g.output[r] < g.nReal; r++)
    objective[r] = g.cost[r];

  // Greedy DAG pre-pass. It is cheap (O(V + E)) and either returns a
  // balance-checked plan or nothing, so it is always run before the solver.
  // A hit is the whole answer in flash mode; otherwise it seeds the solver as
  // both the objective cap (a valid upper bound on the optimum) and a solution
  // hint.
  aw::vector<int64_t> greedy = greedyDagPlan(sub, target, amount, invSrc);
  solver::Options solveOptions = options;
  // The process-wide switch is the one the tools and the mod flip, so it is the
  // source of truth; the per-solve copy is what the retry loop below can clear
  // once a plan has been rejected.
  solveOptions.flash = solveOptions.flash || aw::options.flash;

  // Startup cuts need the physical stock per row, and they are the only reason
  // a solve ever needs it, so it is filled on demand. See
  // Options::EntryGroup and Options::SeedCut.
  aw::vector<int64_t> stockPerRow;
  const auto installStock = [&]() {
    if (!stockPerRow.empty())
      return;
    // `rhs` only carries stock for the non-target rows and hides the target's
    // behind the request, so read it from the caller's inventory instead.
    stockPerRow.resize(m, 0);
    for (uint32_t i = 0; i < m; i++) {
      const ItemId source = sub.itemOrigin[i];
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
  if (greedyUsable && solveOptions.flash) {
    // A hit is the whole answer in flash mode: the greedy plan is acyclic, so
    // the fireability check only ever confirms it. A plan that cannot be fired
    // is not an answer, though, so a rejected one is dropped rather than
    // returned, and the solver gets to look for a startable plan.
    // `greedyUsable` also gates the warm start, the objective upper bound and
    // the fallback below, so clearing it keeps the rejected vector out of all
    // three.
    if (planIsFireable(sub, invSrc,
                       std::span<const int64_t>(greedy.data(), greedy.size()))) {
      result.status = PlanStatus::OK;
      result.provenOptimal = false;
      result.exec = std::move(greedy);
      return result;
    }
    greedyUsable = false;
  }
  if (greedyUsable) {
    if (greedyCost <= (aw::int128) costLowerBound(A, rhs, objective) * GREEDY_QUALITY_FACTOR) {
      solveOptions.objectiveUpperBound = (int64_t) greedyCost;
      solveOptions.solutionHint = std::span<const int64_t>(greedy.data(), greedy.size());
    }
  }

  // The greedy DAG pre-pass returned a real plan. It is acyclic, so it passes
  // the fireability check, and it is a valid answer even when it is far above
  // the optimum. The optimizer prefers a cheaper plan and can spend its whole
  // cycle-retry budget on balance-feasible but unrealizable ones, or come back
  // INFEASIBLE from a cap-limited model, while this plan is sitting right here.
  // Keep it as the fallback so a rejected solve degrades to a worse plan
  // instead of to "no plan". Returns false when there is nothing to fall back
  // to, leaving `result` untouched.
  const auto adoptGreedyFallback = [&]() -> bool {
    if (!greedyUsable)
      return false;
    if (!planIsFireable(sub, invSrc,
                        std::span<const int64_t>(greedy.data(), greedy.size())))
      return false;
    result.status = PlanStatus::OK;
    result.provenOptimal = false;
    result.gap = 0.0;
    result.bestBound = 0.0;
    result.exec = greedy;
    return true;
  };

  // Eager startup cuts: every 1- and 2-cycle of the subgraph gets a group that
  // makes its first firing pay for itself out of the stock and the world
  // outside the cycle, and a two-recipe component also gets the joint seed cut
  // that weighs its two items against one demand. The cuts are implied by
  // fireability, so they can be posted before the first solve: the plan the
  // fireability check used to
  // reject is now excluded while it is still being searched for, which saves a
  // whole rejected solve per cycle, and the instance comes back INFEASIBLE
  // instead of CYCLE_UNFULFILLED when no firing sequence exists at all. A
  // greedy hit above never reaches this point. See buildStartupCuts.
  if (solveOptions.maxStartupGroups > 0)
    buildStartupCuts(sub, solveOptions.entryGroups, solveOptions.seedCuts,
                     solveOptions.maxStartupGroups, solveOptions.maxStartupGroupMembers);
  if (!solveOptions.entryGroups.empty() || !solveOptions.seedCuts.empty())
    installStock();

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
      if (left <= 0.0) {
        adoptGreedyFallback();
        return result;
      }
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
      if (result.status != PlanStatus::CYCLE_UNFULFILLED) {
        result.status = solved.status;
        result.bestBound = solved.bestBound;
      }
      adoptGreedyFallback();
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
                      &witness)) {
      // Tag edges are free, so the solver may leave any of them at its domain
      // cap; the row only asks for `>=`. Cut the conversions the plan does not
      // consume here, where `rhs` is still in scope. Best effort: an empty
      // return means nothing was dropped or the trim stopped firing, and the
      // solver's own vector stands. See TagTidy.cpp.
      aw::vector<int64_t> tidied =
          tidyTagConversions(sub, invSrc, rhs, result.exec);
      if (!tidied.empty())
        result.exec = std::move(tidied);
      return result;
    }

    result.status = PlanStatus::CYCLE_UNFULFILLED;
    result.provenOptimal = false;
    if (retry >= solveOptions.maxCycleRetries) {
      adoptGreedyFallback();
      return result;
    }

    // The plan just rejected was the first balance-feasible one, not a cheap
    // one, and it was not startable. Asking for "the first feasible plan"
    // again would only risk another unstartable cycle, so the remaining
    // retries optimize: an optimum is far more likely to come with a firing
    // sequence. This is what keeps flash from answering CYCLE_UNFULFILLED on
    // instances the optimizing solve answers with the same subgraph and the
    // same budget.
    solveOptions.flash = false;

    // Post one entry group over the whole deadlocked component, not one group
    // per recipe. Grouping is what makes the cut bite: the group's own members
    // are left out of the funding sum, so a member can only be named the entry
    // when its gross input is seedable from the stock or from a recipe outside
    // the component. Singleton groups cannot see this -- the rest of the cycle
    // counts as outside and funds the entry, which is why a longer cycle used to
    // come back unchanged on every retry. The exact no-good still goes in, so
    // progress is guaranteed even when the cut does not by itself exclude the
    // rejected vector.
    const std::span<const uint32_t> component(witness.recipe.data(),
                                              witness.recipe.size());
    bool addedCut = false;
    if (!hasEntryGroup(solveOptions.entryGroups, component)) {
      solver::Options::EntryGroup group;
      if (buildEntryGroup(sub, component, group)) {
        solveOptions.entryGroups.push_back(std::move(group));
        addedCut = true;
      }
    }
    if (addedCut)
      installStock();

    // The rejected plan stays in `result.exec`, so a caller still sees what was
    // refused. Drop the warm start: it points at the vector just forbidden.
    solveOptions.noGoods.push_back(solved.x);
    solveOptions.solutionHint = {};
  }
}

}  // namespace aw
