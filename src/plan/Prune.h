#ifndef AW_PRUNE_H
#define AW_PRUNE_H

#include "aw/plan/CraftingGraph.h"
#define QUERY_PRUNE_PARAM_LIST const Subgraph &sub, ItemId target, std::span<const Amount> sourceInventory, aw::vector<uint8_t> &drop

namespace aw {

// Precomputes for every pruning availble. Does not prune on itself.
void prune(CraftingGraph &graph) noexcept;

bool computeSatellitePruning(const CraftingGraph &graph, ItemId target,
                             std::span<const uint8_t> items,
                             std::span<const uint8_t> recipes,
                             std::span<const Amount> inventory,
                             aw::vector<uint8_t> &drop) noexcept;

bool computeClosedIslandPruning(QUERY_PRUNE_PARAM_LIST) noexcept;
bool computeVariantClassPruning(QUERY_PRUNE_PARAM_LIST) noexcept;
bool computeVariantFoldPruning(QUERY_PRUNE_PARAM_LIST) noexcept;
bool computeTagExclusivePruning(QUERY_PRUNE_PARAM_LIST) noexcept;

// A pair of ItemId and Amount. Better readability than std::pair.
// Moreover, this is now an element of PodVector rather than std::vector.
struct ItemEntry {
  ItemId item;
  Amount amt;

  ItemEntry() = default;
  ItemEntry(ItemId item, Amount amt): item(item), amt(amt) {}

  bool operator<(const ItemEntry &other) const {
    return item == other.item ? amt < other.amt : item < other.item;
  }
};

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
