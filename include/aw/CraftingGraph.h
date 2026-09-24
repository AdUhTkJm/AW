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
  double maxSeconds = 2.0;
};

// Add workstation to base crafting graphs.
struct CraftingGraph : BaseCraftingGraph {
  // Maps recipes to workstations on which it can be executed.
  BaseSparseSets workstations;

  // Tag-edge pruning. `tagEdgeDominated[r] == 1` means recipe r is a synthetic
  // tag edge `T <- m` whose member m is cost-dominated by another member of T;
  std::vector<uint8_t> tagEdgeDominated;  // nRecipe entries

  // Real-recipe (composite) pruning. `recipeDominated[r] == 1` means recipe r
  // outputs a real resource and is dominated by a sibling recipe of the same
  // output once every producer of the guard input is inlined.
  // 
  // The dominator must also run on every workstation `r` can run on to preserve
  // reachability. This is taken care already.
  std::vector<uint8_t> recipeDominated;  // nRecipe entries

  // For a dominated recipe, the real input Y that the witness inlined. The
  // recipe is only dropped when inventory[Y] == 0, so stocked Y can still be
  // spent. UINT32_MAX otherwise.
  std::vector<NodeId> recipeGuardInput;  // nRecipe entries

  // Multi-item "wasteful pack" certificates. `packDominated[r] == 1` means
  // recipe r carries the certificate in `packCertificates[r]` and is never
  // executed by an optimal plan, provided the certificate's zero-stock items
  // are indeed out of stock and every support recipe is reachable.
  std::vector<uint8_t> packDominated;              // nRecipe entries
  std::vector<PackCertificate> packCertificates;   // nRecipe entries

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
// `planCrafting`. A dominated tag edge or real recipe is only dropped when the
// player holds none of the member / guard input, so existing stock can still
// be spent.
Subgraph reachableSubgraph(Handle output, std::span<const Handle> workstations,
                           std::span<const Amount> inventory = {}) noexcept;

// Dominance pruning is on by default. Turning a pass off behaves as if nothing
// were dominated and exists for A/B testing.
void setTagPruningEnabled(bool enabled) noexcept;
bool isTagPruningEnabled() noexcept;
void setRecipePruningEnabled(bool enabled) noexcept;
bool isRecipePruningEnabled() noexcept;

// Certificate pruning is on by default. It is computed during
// registerCraftingGraph, so options must be set before registering a graph.
void setPackPruningEnabled(bool enabled) noexcept;
bool isPackPruningEnabled() noexcept;
void setPackPruningOptions(const PackPruneOptions &options) noexcept;
PackPruneOptions getPackPruningOptions() noexcept;

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept;
const char *getCraftingError() noexcept;
void clearCraftingError() noexcept;

// Note that Minecraft is inherently serial, so we won't worry about any races on C++ side.
// This just gives us the singleton graph.
const CraftingGraph &getCraftingGraph() noexcept;

}  // namespace aw

#endif
