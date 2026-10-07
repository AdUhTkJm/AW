#ifndef AW_PLAN_H
#define AW_PLAN_H

#include <cstdint>
#include <span>
#include <vector>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Greedy.h"
#include "aw/plan/Solver.h"
#include "aw/plan/Status.h"

namespace aw {

struct PlanResult {
  PlanStatus status = PlanStatus::INVALID_INPUT;

  // Executions per recipe, indexed by subgraph recipe id. Empty unless the
  // solver found a solution. Counts are whole numbers: a recipe is crafted an
  // integer number of times.
  aw::vector<int64_t> exec;

  // False when the solver stopped on its gap or time budget rather than
  // proving optimality. The result is still usable; it is just not guaranteed
  // to be the cheapest plan.
  bool provenOptimal = false;
  double gap = 0.0;
  // Solver's dual bound at the moment it stopped, for diagnostics.
  double bestBound = 0.0;

  // Profiling counters from CP-SAT.
  int64_t numConflicts = 0;
  int64_t numBranches = 0;

  // Columns the solver dropped with reduced-cost fixing before its final
  // solve. Diagnostics only; 0 when the pass did not run.
  uint32_t fixedColumns = 0;

  // --- solution-finding diagnostics (aggregated over solver retries) -----
  //
  // Wall-clock milliseconds in the LP relaxation and in the reduced-cost probe
  // across every solver call. Both run outside the solver's wall-clock budget.
  double lpMs = 0.0;
  double probeMs = 0.0;
  // CP-SAT solves behind this plan (cap-growth attempts + probe + reduced),
  // summed over retries.
  int64_t capAttempts = 0;
  // CP-SAT deterministic work, summed over retries; stable across machines.
  double deterministicTime = 0.0;
  // Wall-clock from the start of `planCrafting` to the first balance-feasible
  // solve and to the first *proven-optimal* solve. -1 when never reached. Note
  // that a balance-feasible plan may still fail the fireability check and be
  // rejected, so `firstFeasibleMs` can precede any usable answer.
  double firstFeasibleMs = -1.0;
  double provenMs = -1.0;
  // Number of extra solves the fireability loop needed (> 0 means at least one
  // returned plan was rejected as unstartable).
  int64_t solverRetries = 0;
  // The concatenated per-attempt trajectory, for the finding-time table.
  aw::vector<solver::CapAttempt> attemptTrace;

  // True when `exec` is the greedy DAG pre-pass's own vector rather than a
  // CP-SAT solution: either the flash hit or the `adoptGreedyFallback` path.
  // Diagnostics only, so a benchmark can tell a greedy answer from a solved
  // one without guessing from the conflict counters.
  bool fromGreedy = false;
  // Wall-clock milliseconds the greedy DAG pre-pass spent inside
  // `planCrafting`. Includes the balance re-check but not the subgraph build,
  // and stays 0 for a flash probe's plan, whose pre-pass ran earlier, in
  // `reachableSubgraph`.
  double greedyMs = 0.0;
};

// Plans `amount` new units of the item at subgraph node `target`.
//
// `options` exposes the solver's budget (relative gap, time limit, workers).
// The defaults are tuned for an interactive caller and are expected to be
// overridden from the mod's config.
PlanResult planCrafting(const Subgraph& sub, ItemId target, Amount amount,
                        std::span<const Amount> invSrc,
                        const solver::Options& options = {});

// Post-solve realizability check for the firing vector `exec`.
//
// `planCrafting`'s balance `produced(i) - consumed(i) >= b_i` is a net
// condition: it can accept a plan that consumes an item before the plan
// produces it. This pass condenses the executed recipes into SCCs and fires
// them in dependency order, and returns false when some SCC can never start
// from `invSrc` plus the already-produced upstream output.
//
// The contract is deliberately asymmetric. `true` is a witness: the generated
// order is a real firing sequence. `false` proves this `exec` unrealizable, but
// not that no other plan exists, so callers report it as unproven rather than
// infeasible (see PlanStatus::CYCLE_UNFULFILLED). `invSrc` is indexed like
// `planCrafting`'s stock: by source item node.
//
// When it returns false and `witness` is not null, it is filled with one
// blocker per remaining recipe of the deadlocked component: a recipe and the
// input it could not afford. That is what `planCrafting` turns into startup
// barriers.
struct FireabilityWitness {
  aw::vector<uint32_t> recipe;  // subgraph recipe on the deadlocked component
  aw::vector<ItemId> item;      // input of that recipe, in subgraph item ids
  aw::vector<Amount> need;      // amount of `item` the recipe consumes at once
};

bool planIsFireable(const Subgraph& sub, std::span<const Amount> invSrc,
                    std::span<const int64_t> exec,
                    FireabilityWitness* witness = nullptr) noexcept;

// Post-solve tidying of tag conversions, and the pass `planCrafting` runs
// after the fireability check accepts a CP-SAT solution.
//
// A tag edge is free and a tag's row is only `produced - consumed >= b`, so the
// solver is free to fire every member edge up to its domain cap, which on a
// real pack leaves thousands of conversions of items the plan never consumes.
// This pass cuts a tag's member edges back to the amount the plan consumes.
//
// `b` and `invSrc` describe the same problem `planCrafting` handed the solver:
// `b` is its balance vector, one entry per subgraph item (`-stock` everywhere
// but the target, which holds the request), and `invSrc` is the source-indexed
// stock the fireability re-check needs. `exec` is the solver's firing vector,
// one entry per subgraph recipe.
//
// Every kept count is capped at the solver's own, so a trim never consumes more
// of an item than the plan already did and never touches a real recipe. It is
// best effort on top of that: a trim that stops the plan from firing is
// discarded, because whether it does depends on which member edge of a cyclic
// tag was kept. The return value is the trimmed vector, or an empty vector when
// nothing was dropped, when `options.tagTidy` is off, or when the trim did not
// survive the fireability re-check -- in each case the caller keeps the
// solver's own answer.
aw::vector<int64_t> tidyTagConversions(const Subgraph& sub,
                                       std::span<const Amount> invSrc,
                                       std::span<const int64_t> b,
                                       std::span<const int64_t> exec) noexcept;

// `greedyDagPlan` is declared in Greedy.h, which is included above so that the
// callers that only need the pre-pass do not have to know about it.

}  // namespace aw

#endif
