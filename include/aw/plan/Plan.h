#ifndef AW_PLAN_H
#define AW_PLAN_H

#include <cstdint>
#include <span>
#include <vector>

#include "aw/plan/CraftingGraph.h"
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
};

// Plans `amount` new units of the item at subgraph node `target`.
//
// `options` exposes the solver's budget (relative gap, time limit, workers).
// The defaults are tuned for an interactive caller and are expected to be
// overridden from the mod's config.
PlanResult planCrafting(const Subgraph& sub, NodeId target, Amount amount,
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
  aw::vector<NodeId> item;      // input of that recipe, in subgraph item ids
  aw::vector<Amount> need;      // amount of `item` the recipe consumes at once
};

bool planIsFireable(const Subgraph& sub, std::span<const Amount> invSrc,
                    std::span<const int64_t> exec,
                    FireabilityWitness* witness = nullptr) noexcept;

// Greedy DAG pre-pass used by `planCrafting`, and exposed for testing.
//
// Builds an acyclic view of the reachable recipe graph by cutting back-edges
// from a DFS at the target, then resolves each item once in reverse post-order.
// Returns an empty vector when the retained routes cannot satisfy the request;
// otherwise the firing vector is verified against the exact balance
// `planCrafting` hands the solver, so it is always feasible. It never reports
// a false positive.
aw::vector<int64_t> greedyDagPlan(const Subgraph& sub, NodeId target, Amount amount,
                                  std::span<const Amount> invSrc) noexcept;

}  // namespace aw

#endif
