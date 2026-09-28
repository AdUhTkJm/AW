#ifndef AW_PLAN_H
#define AW_PLAN_H

#include <cstdint>
#include <span>
#include <vector>

#include "aw/CraftingGraph.h"
#include "aw/Solver.h"
#include "aw/Status.h"

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
