#ifndef AW_PLAN_RANKED_CUT_H
#define AW_PLAN_RANKED_CUT_H

#include <span>

#include "aw/plan/CraftingGraph.h"

namespace aw {

// The reachable-item view the ranked cycle cut is built from.
//
// Every per-item array is indexed by subgraph item node and keeps -1 for an
// item outside the target's input cone.
struct ReachableView {
  // Tarjan component id of each reachable item.
  aw::vector<int32_t> component;
  // Shortest number of recipe hops from the target.
  aw::vector<int32_t> distance;
  // Discovery index in `order`.
  aw::vector<int32_t> orderIndex;
  // Reachable items in BFS discovery order from the target.
  aw::vector<uint32_t> order;
  // Reverse adjacency: item -> recipes consuming it. Only recipes that produce
  // a reachable item are listed, which is exactly the set the ranked order has
  // to place.
  BaseSparseSets consumers;

  ReachableView(const BaseCraftingGraph &g, NodeId target) noexcept;
};

// Item `x` has ordinal `order[x]`.
using RankOrder = aw::vector<int32_t>;

RankOrder rankProducible(const BaseCraftingGraph &g, const ReachableView &view,
                         std::span<const Amount> stock, bool stockSeeded) noexcept;

// True when the recipe producing `output` must be dropped from the acyclic
// view: some input lies in the output's own SCC yet is not ranked strictly
// earlier. Edges across SCCs can never close a cycle and are always kept.
//
// This mirrors Thunderbolt's `retainsProducibleRoute` and is deliberately
// order-independent. Unlike the DFS back-edge cut it cannot drop every producer
// of a needed cycle member merely because of discovery order.
bool rankPruning(const ReachableView &view, const RankOrder &order, NodeId output,
               std::span<const NodeId> inputs) noexcept;

}  // namespace aw

#endif
