#ifndef CRAFTING_GRAPH_H
#define CRAFTING_GRAPH_H

#include <span>
#include "aw/utils/PodVector.h"

namespace aw {

// The resource handle used by the Java side. 0 for no resource.
using Handle = uint32_t;
// Indices into the unified node space. Items first, recipes next.
using NodeId = uint32_t;
using Amount = int64_t;
using uint = uint32_t;

// A compressed sparse row matrix, unweighted.
// Row `r` owns targets[offsets[r] .. offsets[r+1]).
struct BaseSparseSets {
  aw::vector<NodeId> offsets;  // V + 1
  aw::vector<NodeId> targets;  // E

  [[nodiscard]]
  size_t numVertices() const noexcept {
    return offsets.size() - 1;
  }

  [[nodiscard]]
  size_t numEdges() const noexcept {
    return targets.size();
  }

  [[nodiscard]]
  std::span<const NodeId> targetsOf(uint row) const noexcept {
    return {targets.data() + offsets[row], targets.data() + offsets[row + 1]};
  }
};

// Add weights to SparseSets.
struct SparseGraph : BaseSparseSets {
  aw::vector<Amount> weights;  // E

  [[nodiscard]]
  size_t numEdges() const noexcept {
    return targets.size();
  }

  [[nodiscard]]
  std::span<const Amount> weightsOf(uint row) const noexcept {
    return {weights.data() + offsets[row], weights.data() + offsets[row + 1]};
  }
};

struct BaseCraftingGraph {
  // See Java side. This is the number of real resources.
  uint32_t nReal = 0;

  // Number of item nodes: the highest handle referenced as an output, input or
  // workstation.
  uint32_t nItem = 0;
  uint32_t nRecipe = 0;

  // Items and recipes are bipartite graphs.
  // These graphs are namely item to recipe and recipe to item graphs.

  // Maps item to all recipes that produce it.
  SparseGraph i2r;
  // Maps recipe to all items that it needs as input.
  SparseGraph r2i;

  // Output item node per recipe. Non-decreasing: the decoder emits recipes in
  // ascending output-handle order, and every path that rewrites the recipe list
  // (canonicalizeRecipes, rebuildFromRecipes, inlineSingleUseTagsCore,
  // assembleSubgraph) preserves or restores that order instead of sorting, so
  // the real recipes (output < nReal) are a prefix and the synthetic tag edges
  // a suffix. A loop that only handles real recipes may therefore `break` at
  // the first recipe with `output[r] >= nReal`, and one that only handles tag
  // edges may start at that boundary.
  aw::vector<NodeId> output;
  aw::vector<Amount> outputAmt;

  [[nodiscard]]
  NodeId recipeNode(uint32_t recipe) const noexcept {
    return nItem + recipe;
  }

  [[nodiscard]]
  bool isRecipeNode(NodeId node) const noexcept {
    return node >= nItem;
  }

  [[nodiscard]]
  bool isRealItem(NodeId node) const noexcept {
    return node < nReal;
  }

  [[nodiscard]]
  std::span<const NodeId> producersOf(NodeId item) const noexcept {
    return i2r.targetsOf(item);
  }

  [[nodiscard]]
  std::span<const NodeId> inputsOf(NodeId recipe) const noexcept {
    return r2i.targetsOf(recipe);
  }

  [[nodiscard]]
  std::span<const Amount> producedAmountsOf(NodeId item) const noexcept {
    return i2r.weightsOf(item);
  }

  [[nodiscard]]
  std::span<const Amount> inputAmountsOf(NodeId recipe) const noexcept {
    return r2i.weightsOf(recipe);
  }
};

// A precomputed "wasteful pack" certificate for one recipe.
//
// `support` / `count` describe a non-negative integer pack z: if a feasible
// plan executes the certified recipe at least once, and none of `zeroStock`
// holds stock, then that plan executes every support recipe at least `count`
// times. Those executions sum to a net loss (their combined column is <= 0),
// so removing the pack keeps the plan feasible and strictly cheaper. Hence no
// optimal plan executes the certified recipe.
struct PackCertificate {
  // Source recipe ids with z_r > 0, ascending.
  aw::vector<uint32_t> support;
  // Parallel to `support`; every count is at least 1.
  aw::vector<Amount> count;
  // Real items whose balance was used as "no stock" (b = 0), ascending.
  aw::vector<NodeId> zeroStock;
};

// Add workstation to base crafting graphs.
struct CraftingGraph : BaseCraftingGraph {
  // Maps recipes to workstations on which it can be executed.
  BaseSparseSets workstations;

  // Tag-edge pruning. `tagEdgeDominated[r] == 1` means recipe r is a synthetic
  // tag edge `T <- m` whose member m is cost-dominated by another member of T.
  // A member that any recipe can produce in a batch (more than one unit per
  // execution) is never marked: the batch surplus is a free way to satisfy T,
  // so dropping the edge can cost real steps. See `Batching guard` in Prune.cpp.
  aw::vector<uint8_t> tagEdgeDominated;  // nRecipe entries

  // Real-recipe (composite) pruning. `recipeDominated[r] == 1` means recipe r
  // outputs a real resource and is dominated by a sibling recipe of the same
  // output once every producer of the guard input is inlined.
  //
  // Dominance is a pure cost comparison, so the dominator may need a
  // workstation r does not. The availability condition is therefore deferred to
  // query time: r is only dropped when one of the dominators listed in
  // `recipeDominatorWorkstations[r]` is available (and the guard has no stock).
  aw::vector<uint8_t> recipeDominated;  // nRecipe entries

  // For a dominated recipe, the real input Y that the witness inlined. The
  // recipe is only dropped when inventory[Y] == 0, so stocked Y can still be
  // spent. UINT32_MAX otherwise.
  aw::vector<NodeId> recipeGuardInput;  // nRecipe entries

  // For a dominated recipe r, the union of the workstations of every kept
  // sibling that dominates r (transitively). r may be dropped when one of them
  // is available and the guard has no stock, because that sibling can then
  // replace r. Empty for an undominated recipe, and also for a dominated recipe
  // whose only replacements cannot be run at all, which is then never dropped.
  aw::vector<aw::vector<NodeId>> recipeDominatorWorkstations;  // nRecipe entries

  // Direct (column) dominance. `recipeDirectDominated[r] == 1` means recipe r
  // outputs a real resource and a sibling recipe of the same output has a
  // componentwise larger column vector: it produces at least as much of the
  // output and consumes no more of every input. Replacing one execution of r by
  // one of the dominator keeps every balance at least as high and the step
  // count unchanged, so no optimal plan needs r.
  //
  // Unlike the composite relation nothing is inlined, so there is no guard
  // input and stock never makes r preferable. The dominator may still need a
  // workstation r does not, so -- exactly as for the composite pass -- the
  // drop is deferred to query time. `recipeDirectDominatorWorkstations[r]` is
  // the union of the workstations of every maximal-column sibling that
  // dominates r; r is dropped when one of them is available.
  aw::vector<uint8_t> recipeDirectDominated;  // nRecipe entries
  aw::vector<aw::vector<NodeId>> recipeDirectDominatorWorkstations;  // nRecipe entries

  // Multi-item "wasteful pack" certificates. `packDominated[r] == 1` means
  // recipe r carries the certificate in `packCertificates[r]` and is never
  // executed by an optimal plan, provided the certificate's zero-stock items
  // are indeed out of stock and every support recipe is reachable.
  aw::vector<uint8_t> packDominated;              // nRecipe entries
  aw::vector<PackCertificate> packCertificates;   // nRecipe entries

  // Dominated-input substitution. `recipeSubstituted[r] == 1` means recipe r
  // outputs a real resource and loses to a sibling once one or more of its
  // inputs is replaced by a resource that cost-dominates it: for every producer
  // of the input Y, the execution consumes at least as much of w as it yields
  // of Y, so `q` units of Y can be paid for with `q` units of w.
  //
  // Dropping r is only valid when none of the collapsed inputs can be spent
  // from stock, so `recipeSubstitutedGuards[r]` lists the real items that must
  // be out of stock (a collapsed tag expands into all of its members), and the
  // replacement may need a workstation r does not, so
  // `recipeSubstitutedDominatorWorkstations[r]` is the union of the kept
  // replacements' workstations. See docs/algorithm.typ, section
  // "基于支配的剪枝：支配输入的替换".
  aw::vector<uint8_t> recipeSubstituted;  // nRecipe entries
  aw::vector<aw::vector<NodeId>> recipeSubstitutedGuards;  // nRecipe entries
  aw::vector<aw::vector<NodeId>>
      recipeSubstitutedDominatorWorkstations;  // nRecipe entries

  [[nodiscard]]
  static NodeId itemNode(Handle handle) noexcept {
    return handle - 1;
  }

  [[nodiscard]]
  static Handle itemHandle(NodeId node) noexcept {
    return node + 1;
  }
};

struct Subgraph {
  // No need to care about workstations for planning on this subgraph.
  BaseCraftingGraph graph;

  // Remappers.
  // graph item node -> source item node, ascending.
  aw::vector<NodeId> itemOrigin;
  // graph recipe id -> source recipe id, ascending.
  aw::vector<NodeId> recipeOrigin;

  // Translates source-graph item id to id in this subgraph.
  // UINT32_MAX on failure.
  NodeId translate(NodeId source) const noexcept;
};

// Computes a subgraph reachable from `output` with available `workstations`.
//
// `output` must be a real resource, and an invalid one means an empty subgrap.
// Handles in `workstations` outside the item range are ignored.
//
// `inventory` is indexed by source item node (handle - 1), matching
// `planCrafting`. A dominated tag edge or composite-dominated real recipe is
// only dropped when the player holds none of the member / guard input, so
// existing stock can still be spent; both it and a directly dominated recipe
// additionally need one of their recorded dominator workstations to be
// available. A directly dominated recipe is dropped without a stock check:
// nothing is inlined, so the dominator replaces it whatever the inventory.
Subgraph reachableSubgraph(Handle output, std::span<const Handle> workstations,
                           std::span<const Amount> inventory = {}) noexcept;

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept;
const char *getCraftingError() noexcept;
void clearCraftingError() noexcept;

// Note that Minecraft is inherently serial, so we won't worry about any races on C++ side.
// This just gives us the singleton graph.
const CraftingGraph &getCraftingGraph() noexcept;

}  // namespace aw

#endif
