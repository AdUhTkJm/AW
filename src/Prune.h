#ifndef AW_PRUNE_H
#define AW_PRUNE_H

#include "aw/CraftingGraph.h"

namespace aw {

// Fills the dominance flags on the canonical graph:
//
//   * tagEdgeDominated for the synthetic `T <- m` tag edges, and
//   * recipeDominated / recipeGuardInput for the real recipes.
//
// Called once by registerCraftingGraph, after canonicalizeRecipes(). Never
// fails; malformed input just prunes less. The implementation lives in
// Prune.cpp so both passes can share the sparse-vector and SCC machinery.
void computePruning(CraftingGraph& graph) noexcept;

// Fills packDominated / packCertificates with the multi-item "wasteful pack"
// certificates. Called by computePruning after the tag and composite passes,
// so recipes those passes already drop are skipped. The implementation lives in
// PackPrune.cpp.
void computePackPruning(CraftingGraph& graph) noexcept;

// Satellite elimination. `itemSeen` / `recipeSeen` describe the subgraph the
// reachability walk kept, `target` is the item being planned for and
// `inventory` is indexed by source item node. Recipes that provably cannot
// appear in an optimal plan of that subgraph are marked in `drop`; the return
// value says whether anything was marked. The implementation lives in
// SatellitePrune.cpp, together with the LP and the exact certificate that back
// every mark.
bool computeSatellitePruning(const CraftingGraph& graph, NodeId target,
                             std::span<const uint8_t> itemSeen,
                             std::span<const uint8_t> recipeSeen,
                             std::span<const Amount> inventory,
                             std::vector<uint8_t>& drop) noexcept;

}  // namespace aw

#endif
