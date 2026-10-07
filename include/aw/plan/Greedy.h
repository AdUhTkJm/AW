#ifndef AW_PLAN_GREEDY_H
#define AW_PLAN_GREEDY_H

// The greedy DAG pre-pass, on its own header so that `reachableSubgraph` can
// call it without pulling in the solver. See Greedy.cpp for the algorithm.

#include <cstdint>
#include <span>
#include <vector>

#include "aw/plan/CraftingGraph.h"

namespace aw {

// Greedy DAG pre-pass used by `planCrafting` and by the flash probe of
// `reachableSubgraph`, and exposed for testing.
//
// Builds an acyclic view of the reachable recipe graph by cutting back-edges
// from a DFS at the target, then resolves each item once in reverse post-order.
// Returns an empty vector when the retained routes cannot satisfy the request;
// otherwise the firing vector is verified against the exact balance
// `planCrafting` hands the solver, so it is always feasible. It never reports
// a false positive.
aw::vector<int64_t> greedyDagPlan(const Subgraph& sub, ItemId target, Amount amount,
                                  std::span<const Amount> invSrc) noexcept;

}  // namespace aw

#endif
