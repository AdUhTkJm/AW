#ifndef AW_PRUNE_H
#define AW_PRUNE_H

#include "aw/plan/CraftingGraph.h"

namespace aw {

// Precomputes for every pruning availble. Does not prune on itself.
void prune(CraftingGraph &graph) noexcept;

// Satellite elimination. Note `inventory` is indexed in source node.
//
// This is called in `reachableSubgraph` per-query, so it doesn't lie in aw::detail.
bool computeSatellitePruning(const CraftingGraph &graph, NodeId target,
                             std::span<const uint8_t> items,
                             std::span<const uint8_t> recipes,
                             std::span<const Amount> inventory,
                             aw::vector<uint8_t> &drop) noexcept;

}

namespace aw::detail {

// A CSR matrix storing all recipes.
struct RecipeVectors {
  aw::vector<uint> offsets;  // nRecipe + 1
  aw::vector<NodeId> items;  // nnz
  aw::vector<Amount> coeffs; // nnz

  [[nodiscard]]
  std::span<const NodeId> itemsOf(uint r) const noexcept {
    return {items.data() + offsets[r], items.data() + offsets[r + 1]};
  }

  [[nodiscard]]
  std::span<const Amount> coeffsOf(uint r) const noexcept {
    return {coeffs.data() + offsets[r], coeffs.data() + offsets[r + 1]};
  }

  explicit RecipeVectors(const BaseCraftingGraph &graph) noexcept;
};

void buildAdjacency(const aw::vector<aw::vector<uint32_t>> &adj,
                    aw::vector<uint> &adjOffsets,
                    aw::vector<uint32_t> &adjTargets) noexcept;

// Marks one representative per sink SCC of a directed graph on `k` nodes.
// `adjOffsets` has k + 1 entries and `adjTargets` has adjOffsets[k] entries.
// `comp` is filled with the component id of every node and `repOfComp` with the
// representative of every sink component (UINT32_MAX for a non-sink one).
// Components are numbered in reverse topological order: an edge to a different
// component always points at a smaller id. `keep[a] == 1` exactly for the
// representatives. Every node with no outgoing edge is its own sink SCC, so the
// result is never empty for k > 0. Returns the number of components.
uint32_t markSinkRepresentatives(uint k, const aw::vector<uint> &adjOffsets,
                                 const aw::vector<uint32_t> &adjTargets,
                                 aw::vector<int32_t> &comp,
                                 aw::vector<uint32_t> &repOfComp,
                                 aw::vector<uint8_t> &keep) noexcept;
                                 
// Fills `compWs[c]` with the union of the workstations of every sink-SCC
// representative reachable from component c, assuming Tarjan numbered the
// components in reverse topological order (an edge a -> b across components
// has comp[b] < comp[a]). `compWs` must already be sized to the component
// count. Those representatives are the replacements a dropped recipe can fall
// back on, so the query keeps it only when one of their stations is available.
void compositeWorkstations(const CraftingGraph &graph,
                          const aw::vector<aw::vector<uint32_t>> &adj,
                          const aw::vector<int32_t> &comp,
                          const aw::vector<uint32_t> &repOfComp,
                          const aw::vector<uint32_t> &recs,
                          aw::vector<aw::vector<NodeId>> &compWs) noexcept;


void computeTagPruning(CraftingGraph &graph) noexcept;
void computePackPruning(CraftingGraph& graph) noexcept;
void computeSubstitutionPruning(CraftingGraph &graph, const RecipeVectors &vec) noexcept;

}  // namespace aw

#endif
