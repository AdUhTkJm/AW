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
using Amount = uint64_t;

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

// Add workstation to base crafting graphs.
struct CraftingGraph : BaseCraftingGraph {
  // Maps recipes to workstations on which it can be executed.
  BaseSparseSets workstations;

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
};

// Computes a subgraph reachable from `output` with available `workstations`.
//
// `output` must be a real resource, and an invalid one means an empty subgrap.
// Handles in `workstations` outside the item range are ignored.
Subgraph reachableSubgraph(Handle output, std::span<const Handle> workstations) noexcept;

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept;
const char *getCraftingError() noexcept;
void clearCraftingError() noexcept;

// Note that Minecraft is inherently serial, so we won't worry about any races on C++ side.
// This just gives us the singleton graph.
const CraftingGraph &getCraftingGraph() noexcept;

}  // namespace aw

#endif
