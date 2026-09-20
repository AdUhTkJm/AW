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

// A compressed sparse row matrix.
// Row `r` owns targets[offsets[r] .. offsets[r+1]).
// In all comments, we denote the number of nodes as V and edges as E.
struct SparseGraph {
  std::vector<NodeId> offsets;  // V + 1
  std::vector<NodeId> targets;  // E
  std::vector<Amount> weights;  // E

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

  [[nodiscard]]
  std::span<const Amount> weightsOf(size_t row) const noexcept {
    return {weights.data() + offsets[row], weights.data() + offsets[row + 1]};
  }
};

struct CraftingGraph {
  // See Java side. This is the number of real resources.
  uint32_t nReal = 0;

  // Number of item nodes is set to the highest handle referenced as an output or input.
  uint32_t nItem = 0;
  uint32_t nRecipe = 0;

  // Items and recipes are bipartite graphs.
  // These graphs are namely item to recipe and recipe to item graphs.
  SparseGraph i2r;
  SparseGraph r2i;

  std::vector<NodeId> output;
  std::vector<Amount> outputAmt;

  [[nodiscard]]
  static NodeId itemNode(Handle handle) noexcept {
    return handle - 1;
  }

  [[nodiscard]]
  static Handle itemHandle(NodeId node) noexcept {
    return node + 1;
  }

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

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept;
const char *getCraftingError() noexcept;
void clearCraftingError() noexcept;

// Note that Minecraft is inherently serial, so we won't worry about any races on C++ side.
// This just gives us the singleton graph.
const CraftingGraph &getCraftingGraph() noexcept;

}  // namespace aw

#endif
