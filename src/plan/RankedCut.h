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
};

// One BFS over the input cone computes reachability, shortest input distances
// and SCC membership. O(V + E) on that cone.
ReachableView analyzeReachable(const BaseCraftingGraph &g, NodeId target) noexcept;

// Hyperedge-Kahn order over the reachable items: a recipe only becomes ready
// once every input it consumes has been ranked, so the ordinal grows from
// producible sources toward the target.
//
// This mirrors Thunderbolt's `rankProducibleRoutes`. `stock` is indexed by
// subgraph item and the target's own stock must be 0. `stockSeeded` selects one
// of the two ranked passes: it prefers stocked cycle members as cut points,
// while the stock-free pass is the plain zero-inventory DAG in which any route
// producible from raw sources survives.
struct RankOrder {
  // Ordinal per item, -1 when the order could not place it (a cycle with no
  // producible entry point).
  aw::vector<int32_t> ordinal;
};

RankOrder rankProducible(const BaseCraftingGraph &g, const ReachableView &view,
                         std::span<const Amount> stock, bool stockSeeded) noexcept;

// True when the recipe producing `output` must be dropped from the acyclic
// view: some input lies in the output's own SCC yet is not ranked strictly
// earlier. Edges across SCCs can never close a cycle and are always kept.
//
// This mirrors Thunderbolt's `retainsProducibleRoute` and is deliberately
// order-independent. Unlike the DFS back-edge cut it cannot drop every producer
// of a needed cycle member merely because of discovery order.
bool cutByRank(const ReachableView &view, const RankOrder &order, NodeId output,
               std::span<const NodeId> inputs) noexcept;

}  // namespace aw

#endif
