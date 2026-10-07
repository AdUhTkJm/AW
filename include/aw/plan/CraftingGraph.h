#ifndef CRAFTING_GRAPH_H
#define CRAFTING_GRAPH_H

#include <algorithm>
#include <span>
#include "aw/utils/PodVector.h"

namespace aw {

// The resource handle used by the Java side. 0 for no resource.
using Handle = uint32_t;
// Item index, `handle - 1`. Items and recipes live in two separate id spaces,
// so a recipe id can index `r2i`, `output` and `workstations` directly.
using ItemId = uint32_t;
// Recipe index, `[0, nRecipe)`. The same ids are the targets of `i2r` and the
// rows of `r2i`, `output` and `workstations`.
using RecipeId = uint32_t;
using Amount = int64_t;
using uint = uint32_t;

// A compressed sparse row matrix, unweighted.
// Row `r` owns targets[offsets[r] .. offsets[r+1]).
// The id space of `targets` depends on the matrix: `i2r` holds recipe ids,
// `r2i` and `workstations` hold item ids. Both types alias uint32_t, so one
// representation covers all of them.
struct BaseSparseSets {
  aw::vector<ItemId> offsets;  // V + 1
  aw::vector<ItemId> targets;  // E

  [[nodiscard]]
  size_t numVertices() const noexcept {
    return offsets.size() - 1;
  }

  [[nodiscard]]
  size_t numEdges() const noexcept {
    return targets.size();
  }

  [[nodiscard]]
  std::span<const ItemId> targetsOf(uint row) const noexcept {
    return {targets.data() + offsets[row], targets.data() + offsets[row + 1]};
  }

  [[nodiscard]]
  ItemId firstTargetOf(uint row) const noexcept {
    return targets[offsets[row]];
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

  Amount firstWeightOf(uint row) const noexcept {
    return weights[offsets[row]];
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

  // Maps item to all recipes that produce it. Targets are recipe ids.
  SparseGraph i2r;
  // Maps recipe to all items that it needs as input. Targets are item ids,
  // ascending and duplicate-free, and every amount is positive: the decoder
  // delta-codes each list, every later rewrite of the lists preserves that
  // encoding, and dropNonPositiveInputs clears the remaining amounts before any
  // pass runs. The sparse column comparisons rely on that order.
  SparseGraph r2i;

  // Anchor output item id per recipe. Non-decreasing: the decoder emits recipes
  // in ascending anchor-handle order, and every path that rewrites the recipe
  // list (canonicalizeRecipes, rebuildFromRecipes, inlineSingleUseTagsCore,
  // assembleSubgraph) preserves or restores that order instead of sorting, so
  // the real recipes (output < nReal) are a prefix and the synthetic tag edges
  // a suffix. A loop that only handles real recipes may therefore `break` at
  // the first recipe with `output[r] >= nReal`, and one that only handles tag
  // edges may start at that boundary.
  //
  // The anchor is the recipe's identity, its sort key and the row the plan
  // response names it under. It is not necessarily the only output; see r2o.
  aw::vector<ItemId> output;
  aw::vector<Amount> outputAmt;

  // What one execution of the recipe costs the planner, counted in executions
  // of the underlying Minecraft recipe. It is 1 for an ordinary real recipe, 0
  // for a synthetic tag edge (picking a member is free in game), and the batch
  // size for a level of a chanced recipe: the Java side folds `batch`
  // executions into one deterministic recipe that consumes `batch` input sets
  // (see ChanceBatching), so one execution of that column *is* `batch` real
  // executions and has to be charged as many steps. A real recipe is never
  // free -- the reader clamps it to at least 1 -- because a zero-cost real
  // column is unbounded under the objective cap.
  //
  // Same order as `output`: real recipes are a prefix and tag edges the suffix,
  // so a loop that only handles real recipes may `break` here as well.
  aw::vector<Amount> cost;  // nRecipe entries

  // Every gross output per recipe, CSR over recipes: recipe `r` outputs
  // `r2o.targets[offsets[r] .. offsets[r + 1])` at the parallel amounts in
  // `weights`. Entries are sorted ascending by item and deduplicated. The
  // anchor is always present; the remaining entries are byproducts. A recipe
  // that consumes an item it also outputs keeps both rows here and in `r2i`,
  // and only the net column merges them.
  //
  // `i2r` carries one edge per output, so a recipe appears in the producer list
  // of every item it outputs, not just its anchor.
  SparseGraph r2o;

  [[nodiscard]]
  std::span<const ItemId> outputsOf(RecipeId recipe) const noexcept {
    return r2o.targetsOf(recipe);
  }

  [[nodiscard]]
  ItemId firstOutputOf(RecipeId recipe) const noexcept {
    return r2o.firstTargetOf(recipe);
  }

  [[nodiscard]]
  std::span<const Amount> outputAmountsOf(RecipeId recipe) const noexcept {
    return r2o.weightsOf(recipe);
  }

  [[nodiscard]]
  Amount firstOutputAmountOf(RecipeId recipe) const noexcept {
    return r2o.firstWeightOf(recipe);
  }

  [[nodiscard]]
  bool hasByproducts(RecipeId recipe) const noexcept {
    return r2o.offsets[recipe + 1] - r2o.offsets[recipe] > 1;
  }

  // Gross amount of `item` one execution outputs, 0 when it outputs none.
  // Binary search over the sorted output list.
  [[nodiscard]]
  Amount producedAmountOf(RecipeId recipe, ItemId item) const noexcept {
    const ItemId *begin = r2o.targets.data() + r2o.offsets[recipe];
    const ItemId *end = r2o.targets.data() + r2o.offsets[recipe + 1];
    const ItemId *it = std::lower_bound(begin, end, item);
    return it != end && *it == item ? r2o.weights[(size_t) (it - r2o.targets.data())] : 0;
  }

  [[nodiscard]]
  bool isRealItem(ItemId node) const noexcept {
    return node < nReal;
  }

  [[nodiscard]]
  std::span<const RecipeId> producersOf(ItemId item) const noexcept {
    return i2r.targetsOf(item);
  }

  [[nodiscard]]
  std::span<const ItemId> inputsOf(RecipeId recipe) const noexcept {
    return r2i.targetsOf(recipe);
  }

  [[nodiscard]]
  ItemId firstInputOf(RecipeId recipe) const noexcept {
    return r2i.firstTargetOf(recipe);
  }

  [[nodiscard]]
  std::span<const Amount> producedAmountsOf(ItemId item) const noexcept {
    return i2r.weightsOf(item);
  }

  [[nodiscard]]
  std::span<const Amount> inputAmountsOf(RecipeId recipe) const noexcept {
    return r2i.weightsOf(recipe);
  }

  Amount firstInputAmountOf(RecipeId recipe) const noexcept {
    return r2i.firstWeightOf(recipe);
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
  aw::vector<ItemId> zeroStock;
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
  aw::vector<ItemId> recipeGuardInput;  // nRecipe entries

  // For a dominated recipe r, the union of the workstations of every kept
  // sibling that dominates r (transitively). r may be dropped when one of them
  // is available and the guard has no stock, because that sibling can then
  // replace r. Empty for an undominated recipe, and also for a dominated recipe
  // whose only replacements cannot be run at all, which is then never dropped.
  aw::vector<aw::vector<ItemId>> recipeDominatorWorkstations;  // nRecipe entries

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
  aw::vector<aw::vector<ItemId>> recipeDirectDominatorWorkstations;  // nRecipe entries

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
  aw::vector<aw::vector<ItemId>> recipeSubstitutedGuards;  // nRecipe entries
  aw::vector<aw::vector<ItemId>>
      recipeSubstitutedDominatorWorkstations;  // nRecipe entries

  [[nodiscard]]
  static ItemId itemNode(Handle handle) noexcept {
    return handle - 1;
  }

  [[nodiscard]]
  static Handle itemHandle(ItemId node) noexcept {
    return node + 1;
  }
};

struct Subgraph {
  // No need to care about workstations for planning on this subgraph.
  BaseCraftingGraph graph;

  // Remappers.
  // graph item id -> source item id, ascending.
  aw::vector<ItemId> itemOrigin;
  // graph recipe id -> source recipe id, ascending.
  aw::vector<RecipeId> recipeOrigin;

  // Translates source-graph item id to id in this subgraph.
  // UINT32_MAX on failure.
  ItemId translate(ItemId source) const noexcept;
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
