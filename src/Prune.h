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

}  // namespace aw

#endif
