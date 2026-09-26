#ifndef CRAFTING_GRAPH_H
#define CRAFTING_GRAPH_H
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace aw {

// The resource handle used by the Java side. 0 for no resource.
using Handle = uint32_t;
// Indices into the unified node space. Items first, recipes next.
using NodeId = uint32_t;
using Amount = int64_t;

// A compressed sparse row matrix, unweighted.
// Row `r` owns targets[offsets[r] .. offsets[r+1]).
struct BaseSparseSets {
  std::vector<NodeId> offsets;  // V + 1
  std::vector<NodeId> targets;  // E

  [[nodiscard]]
  size_t numVertices() const noexcept {
    return offsets.size() - 1;
  }

  [[nodiscard]]
  size_t numEdges() const noexcept {
    return targets.size();
  }

  [[nodiscard]]
  std::span<const NodeId> targetsOf(size_t row) const noexcept {
    return {targets.data() + offsets[row], targets.data() + offsets[row + 1]};
  }
};

// Add weights to SparseSets.
struct SparseGraph : BaseSparseSets {
  std::vector<Amount> weights;  // E

  [[nodiscard]]
  size_t numEdges() const noexcept {
    return targets.size();
  }

  [[nodiscard]]
  std::span<const Amount> weightsOf(size_t row) const noexcept {
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
  SparseGraph i2r;
  SparseGraph r2i;

  std::vector<NodeId> output;
  std::vector<Amount> outputAmt;

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
    return node < nItem && node < nReal;
  }
};

// A precomputed "wasteful pack" certificate for one recipe.
//
// `support` / `count` describe a non-negative integer pack z: if a feasible
// plan executes the certified recipe at least once, and none of `zeroStock`
// holds stock, then that plan executes every support recipe at least `count`
// times. Those executions sum to a net loss (their combined column is <= 0),
// so removing the pack keeps the plan feasible and strictly cheaper. Hence no
// optimal plan executes the certified recipe. See docs/algorithm.typ, section
// "针对多个零库存物品的推广".
struct PackCertificate {
  // Source recipe ids with z_r > 0, ascending.
  std::vector<uint32_t> support;
  // Parallel to `support`; every count is at least 1.
  std::vector<Amount> count;
  // Real items whose balance was used as "no stock" (b = 0), ascending.
  std::vector<NodeId> zeroStock;
};

// Budget for the multi-item "wasteful pack" certificates. The defaults keep
// registration bounded on the full 12k-item graphs; running out of budget only
// turns a search branch into a leaf, which weakens the pack but never makes it
// unsound.
struct PackPruneOptions {
  bool enabled = true;

  // Pack size: at most this many distinct recipes, and at most this total
  // count across the pack.
  uint32_t maxPackRecipes = 128;
  int64_t maxPackValue = 1024;

  // R3 search budget. Exceeding either turns the node into a leaf.
  uint32_t maxBranchDepth = 8;
  uint32_t maxBranchNodes = 4096;

  // Cap on the pack's zero-stock set. A derivation that needs more is dropped.
  uint32_t maxZeroStockItems = 32;

  // Wall-clock budget for the whole pass. <= 0 means no limit.
  double maxSeconds = 60.0;
};

// Budget for the satellite-elimination pass (docs/algorithm.typ, "孤岛消除").
//
// The pass runs inside reachableSubgraph, where the target and the inventory
// are known: it looks for groups of recipes whose only net output to the rest
// of the subgraph is one item, hold no stock inside, and cannot repay that
// item, and drops them. The undirected articulation components and the wider
// "input-leaking" islands found by the escape enumeration are two ways to find
// such groups. Everything else is a budget, and running out of one only means a
// group is left alone.
struct SatellitePruneOptions {
  bool enabled = true;

  // A component with more nodes than this is skipped. A wide component is
  // never a "variants hanging off a basic form" island, and its LP would
  // dominate the pass.
  uint32_t maxComponentNodes = 512;

  // Budget for the generalized escape enumeration. The island of a single
  // escape can be much wider than an undirected component (the maximal closed
  // set is taken), so it has its own, larger bound; `maxComponentNodes` still
  // bounds the undirected pass. The count is items plus recipes.
  uint32_t maxIslandNodes = 4096;

  // Wall-clock budget for one round of the pass, on top of the reachability
  // walk. reachableSubgraph runs at most four rounds. <= 0 means no limit.
  double maxSeconds = 0.05;
};

// Where single-use tag inlining runs. Off by default; it is an experiment.
//
// A tag T with member edges `T <- m1 .. T <- mn` and exactly one real consumer
// `R: a <- T, ...` (with the tag consumed once at amount 1) is replaced by the
// n real recipes `a <- m1, ...`. The tag node and its n edges disappear and the
// recipe count drops by one. A larger consumed amount is left alone: it could
// be served by mixed members, which a homogeneous expansion cannot express.
//
// Inlining changes the objective, because the removed tag edges were charged 1
// each in the solver model. Tag edges are conceptually free; the unit cost is
// only there to stop the solver turning arbitrary amounts of items into tags.
// So an inlined optimum is meant to be lower, and the two optima are not
// compared for equality.
enum class TagInlineMode : uint8_t {
  // No inlining.
  OFF = 0,
  // Flatten every single-use tag at registration time, using every member
  // edge, before the dominance passes run. This can re-introduce members that
  // tag pruning would have dropped, so it can grow the query-time subgraph, but
  // it gives the passes the flattest graph to reason about.
  PRE_PRUNE = 1,
  // Flatten single-use tags at query time, inside reachableSubgraph, after the
  // walk and every pruning pass. Only recipes that survived are inlined, so a
  // tag's surviving member edges -- which include a dominated member the player
  // holds stock of -- are exactly the members that can still be spent. This
  // keeps the subgraph from growing and keeps stock usable.
  QUERY_TIME = 2,
  // PRE_PRUNE followed by QUERY_TIME: persistent single-use tags are flattened
  // once for the whole corpus, and the subgraph-specific ones are flattened per
  // query.
  BOTH = 3,
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
  std::vector<uint8_t> tagEdgeDominated;  // nRecipe entries

  // Real-recipe (composite) pruning. `recipeDominated[r] == 1` means recipe r
  // outputs a real resource and is dominated by a sibling recipe of the same
  // output once every producer of the guard input is inlined.
  //
  // Dominance is a pure cost comparison, so the dominator may need a
  // workstation r does not. The availability condition is therefore deferred to
  // query time: r is only dropped when one of the dominators listed in
  // `recipeDominatorWorkstations[r]` is available (and the guard has no stock).
  std::vector<uint8_t> recipeDominated;  // nRecipe entries

  // For a dominated recipe, the real input Y that the witness inlined. The
  // recipe is only dropped when inventory[Y] == 0, so stocked Y can still be
  // spent. UINT32_MAX otherwise.
  std::vector<NodeId> recipeGuardInput;  // nRecipe entries

  // For a dominated recipe r, the union of the workstations of every kept
  // sibling that dominates r (transitively). r may be dropped when one of them
  // is available and the guard has no stock, because that sibling can then
  // replace r. Empty for an undominated recipe, and also for a dominated recipe
  // whose only replacements cannot be run at all, which is then never dropped.
  std::vector<std::vector<NodeId>> recipeDominatorWorkstations;  // nRecipe entries

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
  std::vector<uint8_t> recipeDirectDominated;  // nRecipe entries
  std::vector<std::vector<NodeId>> recipeDirectDominatorWorkstations;  // nRecipe entries

  // Multi-item "wasteful pack" certificates. `packDominated[r] == 1` means
  // recipe r carries the certificate in `packCertificates[r]` and is never
  // executed by an optimal plan, provided the certificate's zero-stock items
  // are indeed out of stock and every support recipe is reachable.
  std::vector<uint8_t> packDominated;              // nRecipe entries
  std::vector<PackCertificate> packCertificates;   // nRecipe entries

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
  std::vector<uint8_t> recipeSubstituted;  // nRecipe entries
  std::vector<std::vector<NodeId>> recipeSubstitutedGuards;  // nRecipe entries
  std::vector<std::vector<NodeId>>
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
  std::vector<NodeId> itemOrigin;
  // graph recipe id -> source recipe id, ascending.
  std::vector<NodeId> recipeOrigin;

  // Translates source-graph item id to id in this subgraph.
  // UINT32_MAX on failure.
  NodeId translate(NodeId source) const noexcept;

  // Translates an entire inventory.
  std::vector<Amount> translateInv(std::span<const Amount> src) const noexcept;
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

// Dominance pruning is on by default. Turning a pass off behaves as if nothing
// were dominated and exists for A/B testing.
void setTagPruningEnabled(bool enabled) noexcept;
bool isTagPruningEnabled() noexcept;
void setRecipePruningEnabled(bool enabled) noexcept;
bool isRecipePruningEnabled() noexcept;

// Direct (column) dominance pruning is independent of the composite pass and
// also on by default. It is a query-time pass over registration-time flags, so
// it can be switched off on its own.
void setDirectDominancePruningEnabled(bool enabled) noexcept;
bool isDirectDominancePruningEnabled() noexcept;

// Dominated-input substitution is independent of the other recipe passes and
// also on by default. It computes its own recursive cost relation at
// registration time, so it can be switched off on its own.
void setSubstitutionPruningEnabled(bool enabled) noexcept;
bool isSubstitutionPruningEnabled() noexcept;

// Integrality relaxation for the dominance passes now lives in Options.h as
// `options().nonoptimal`. It is on by default, and read at registration time,
// so it must be set before registerCraftingGraph. See the field's comment for
// exactly which guards it drops and which relation it computes instead.

// Certificate pruning is on by default. It is computed during
// registerCraftingGraph, so options must be set before registering a graph.
void setPackPruningEnabled(bool enabled) noexcept;
bool isPackPruningEnabled() noexcept;
void setPackPruningOptions(const PackPruneOptions &options) noexcept;
PackPruneOptions getPackPruningOptions() noexcept;

// Satellite elimination is on by default. It is a query-time pass, so it can be
// switched off independently of the registration-time passes above.
void setSatellitePruningEnabled(bool enabled) noexcept;
bool isSatellitePruningEnabled() noexcept;
void setSatellitePruningOptions(const SatellitePruneOptions &options) noexcept;
SatellitePruneOptions getSatellitePruningOptions() noexcept;

// Dead-node cleanup is on by default. Like satellite elimination it is a
// query-time pass, so it is switched off independently of the registration-time
// passes above.
//
// Reachability keeps an item when some surviving recipe consumes it, even if no
// surviving recipe can produce it. When the player holds none of that item and
// the source graph does have a producer, the item can only appear as a zero
// column: every recipe that consumes it is dead too, and dropping those can
// expose further such items. This pass removes that closure and re-runs the
// walk, so only the parts that can actually be reached remain. Items with no
// producer anywhere in the source graph are raw materials the player is meant
// to gather, so they are never touched.
void setDeadNodePruningEnabled(bool enabled) noexcept;
bool isDeadNodePruningEnabled() noexcept;

// Single-use tag inlining. Like the pack options it is read at registration
// time, so it must be set before registerCraftingGraph.
void setTagInliningMode(TagInlineMode mode) noexcept;
TagInlineMode getTagInliningMode() noexcept;

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept;
const char *getCraftingError() noexcept;
void clearCraftingError() noexcept;

// Note that Minecraft is inherently serial, so we won't worry about any races on C++ side.
// This just gives us the singleton graph.
const CraftingGraph &getCraftingGraph() noexcept;

}  // namespace aw

#endif
