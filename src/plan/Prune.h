#ifndef AW_PRUNE_H
#define AW_PRUNE_H

#include "aw/plan/CraftingGraph.h"

namespace aw {

// Precomputes for every pruning availble. Does not prune on itself.
void prune(CraftingGraph &graph) noexcept;

// Satellite elimination. Note `inventory` is indexed in source node.
//
// This is called in `reachableSubgraph` per-query, so it doesn't lie in aw::detail.
bool computeSatellitePruning(const CraftingGraph &graph, ItemId target,
                             std::span<const uint8_t> items,
                             std::span<const uint8_t> recipes,
                             std::span<const Amount> inventory,
                             aw::vector<uint8_t> &drop) noexcept;

// Escape-free satellite elimination, on an already assembled subgraph. Drops
// every subgraph recipe that produces or consumes an item of the largest set of
// items that no recipe leaks out of, excluding the target and the stock. It has
// to run after the query-time re-pruning, which can be the step that cuts the
// island off from the needed items.
//
// `target` is a subgraph item, `sourceInventory` is indexed in source node, and
// `drop` is indexed by subgraph recipe, sized to `sub.graph.nRecipe`.
bool computeClosedIslandPruning(const Subgraph &sub, ItemId target,
                                std::span<const Amount> sourceInventory,
                                aw::vector<uint8_t> &drop) noexcept;

// Interchangeable-variant (tag-orbit) elimination, on an already assembled
// subgraph. Drops the conversions inside a class of items that every external
// recipe reaches through the same tag: a plan that used a conversion already
// directly produces at least as many class members as it spends on that tag,
// and the tag accepts every member, so the conversions are never needed.
//
// `target` is a subgraph item and is excluded from every class. `drop` is
// indexed by subgraph recipe, sized to `sub.graph.nRecipe`.
bool computeVariantClassPruning(const Subgraph &sub, ItemId target,
                                aw::vector<uint8_t> &drop) noexcept;

// Variant-folding elimination, on an already assembled subgraph. A decorative
// twin of a base is an item that a tag, or a chain of "same recipe, other
// item" copies, makes interchangeable with the base everywhere it is demanded
// by name. The pass folds the twins onto the base and drops every recipe that
// mentions a folded item, but only after a signature check proves every such
// recipe can be replayed by recipes that survive. See docs/algorithm.typ
// (@multi-exit) and VariantFoldPrune.cpp.
//
// `target` is a subgraph item and is never folded. `sourceInventory` is indexed
// in source node; a stocked decorative item is never folded. `drop` is indexed
// by subgraph recipe, sized to `sub.graph.nRecipe`.
bool computeVariantFoldPruning(const Subgraph &sub, ItemId target,
                               std::span<const Amount> sourceInventory,
                               aw::vector<uint8_t> &drop) noexcept;

// Tag-exclusive producer elimination, on an already assembled subgraph. A real
// item is tag-exclusive for a tag T when every consumer of it is a member
// edge of T. A real recipe whose outputs are all exclusive for the same T is
// dropped when it produces no more M(T)-capacity than it consumes, because a
// plan that fired it already holds at least as much M(T)-capacity and T accepts
// any member, so the freed inputs can be routed into T instead.
//
// `target` is a subgraph item and is excluded: a query that names an output of
// the recipe has to keep it. `drop` is indexed by subgraph recipe, sized to
// `sub.graph.nRecipe`.
bool computeTagExclusivePruning(const Subgraph &sub, ItemId target,
                                aw::vector<uint8_t> &drop) noexcept;

}

namespace aw::detail {

// A CSR matrix storing all recipes.
struct RecipeVectors {
  aw::vector<uint> offsets;  // nRecipe + 1
  aw::vector<ItemId> items;  // nnz
  aw::vector<Amount> coeffs; // nnz

  [[nodiscard]]
  std::span<const ItemId> itemsOf(uint r) const noexcept {
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
                          aw::vector<aw::vector<ItemId>> &compWs) noexcept;


// True when the sparse column (ci, cc) is componentwise <= (si, sc). Positive
// coefficients are consumption and negative ones production, so an item only
// the right side names is fatal when that side produces it. Both runs must be
// ascending and duplicate-free; the caller owns that invariant. Raw pointers
// rather than spans because every caller is an inner loop that already holds
// the rows.
[[gnu::always_inline]]
inline bool leVectorRaw(const ItemId *ci, const Amount *cc, size_t cSize,
                        const ItemId *si, const Amount *sc, size_t sSize) noexcept {
  size_t i = 0, j = 0;

  // Shared part, where both sides have elements.
  while (i < cSize && j < sSize) {
    const ItemId cid = ci[i];
    const ItemId sid = si[j];

    if (cid == sid) {
      if (cc[i] > sc[j])
        return false;
      ++i;
      ++j;
    } else if (cid < sid) {
      if (cc[i] > 0)
        return false;
      ++i;
    } else {
      if (sc[j] < 0)
        return false;
      ++j;
    }
  }

  // Now the remaining items for c and s.
  while (i < cSize) {
    if (cc[i] > 0)
      return false;
    ++i;
  }
  while (j < sSize) {
    if (sc[j] < 0)
      return false;
    ++j;
  }

  return true;
}

bool leVector(std::span<const ItemId> ci, std::span<const Amount> cc,
              std::span<const ItemId> si, std::span<const Amount> sc) noexcept;

void computeTagPruning(CraftingGraph &graph) noexcept;
void computePackPruning(CraftingGraph& graph) noexcept;
void computeSubstitutionPruning(CraftingGraph &graph, const RecipeVectors &vec) noexcept;
void computeRecipePruning(CraftingGraph &graph, const RecipeVectors &vec) noexcept;

// Query-time re-pruning of a reachable subgraph. Runs the composite, direct and
// (optionally) pack passes on the workstation-free subgraph and returns one byte
// per subgraph recipe, 1 meaning "drop". `sourceInventory` is indexed by source
// item node, exactly as `planCrafting` receives it; the subgraph's remapping is
// undone through `sub.itemOrigin` for the stock guards. Nothing happens unless
// `options.reprune.enabled` is set.
aw::vector<uint8_t> repruneSubgraph(const Subgraph &sub,
                                    std::span<const Amount> sourceInventory) noexcept;

}  // namespace aw

#endif
