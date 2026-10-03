// The order-independent cycle cut. See RankedCut.h for the contract.
//
// Thunderbolt's `buildDag` cuts every recipe with an input that is still on the
// DFS stack. Which routes that keeps depends on arrival order, so a target
// whose needed SCC member is only reached through a detour can lose every
// producer of that member. The ranked cut below instead orients each SCC once
// from a hyperedge-Kahn order and keeps only inputs that are ranked strictly
// earlier, which is independent of how the DFS happens to walk the graph.

#include "RankedCut.h"
#include "aw/utils/Scc.h"

#include <cstdint>
#include <queue>
#include <vector>

namespace aw {
namespace {

// A pending item in the ranked Kahn order. Pops are monotone in (tier, level),
// so the last dependency to release a recipe is also the latest one, and its
// tier is inherited by the recipe's output.
struct Ready {
  uint32_t item;
  int32_t tier;
  int32_t level;
  uint64_t sortKey;

  // std::priority_queue is a max-heap, so every comparison is inverted.
  bool operator<(const Ready &other) const noexcept { return sortKey > other.sortKey; }
};

}  // namespace

ReachableView::ReachableView(const BaseCraftingGraph &g, ItemId target) noexcept:
  component(g.nItem, -1), distance(g.nItem, -1), orderIndex(g.nItem, -1)
{
  const uint32_t nItem = g.nItem;
  aw::vector<uint32_t> queue;
  // Initialize `distance`: shortest distance from `target` based on BFS.
  order.reserve(nItem);
  queue.reserve(nItem);
  const auto discover = [&](ItemId item, int32_t dist) {
    if (orderIndex[item] >= 0)
      return;
    orderIndex[item] = order.size();
    distance[item] = dist;
    order.push_back_unchecked(item);
    queue.push_back_unchecked(item);
  };
  discover(target, 0);
  for (size_t head = 0; head < queue.size(); head++) {
    const uint32_t item = queue[head];
    const int32_t nextDistance = distance[item] + 1;
    for (RecipeId recipe : g.producersOf(item)) {
      for (ItemId input : g.inputsOf(recipe))
        discover(input, nextDistance);
    }
  }

  // This is formed by projecting items out of `g`.
  // In other words, `i -> j` means item `i` needs item `j` in one of its recipes.
  aw::vector<ItemId> adjOffsets(nItem + 1, 0);
  // This is a reversed graph of `g.r2i`.
  // `i -> r` means recipe `r` needs item `i` as input.
  aw::vector<ItemId> consumerOffsets(nItem + 1, 0);

  // First compute offsets. `adj` stays item-level (one entry per output row of
  // a recipe), which is what the item SCC test wants. `consumers` counts each
  // recipe once, so a multi-output recipe is released once when the last of its
  // dependencies is ranked.
  size_t nEdge = 0;
  for (uint32_t item : order) {
    uint32_t count = 0;
    for (RecipeId recipe : g.producersOf(item)) {
      const auto inputs = g.inputsOf(recipe);
      count += (uint32_t) inputs.size();
    }
    adjOffsets[item + 1] = count;
    nEdge += count;
  }
  for (uint32_t item = 1; item <= nItem; item++)
    adjOffsets[item] += adjOffsets[item - 1];

  aw::vector<uint8_t> recipeSeen(g.nRecipe, 0);
  for (uint32_t item : order) {
    for (RecipeId recipe : g.producersOf(item)) {
      if (recipeSeen[recipe])
        continue;
      recipeSeen[recipe] = 1;
      for (ItemId input : g.inputsOf(recipe))
        consumerOffsets[input + 1]++;
    }
  }
  for (uint32_t item = 1; item <= nItem; item++)
    consumerOffsets[item] += consumerOffsets[item - 1];

  // Then fill in the data.
  aw::vector<ItemId> adjTargets(nEdge);
  aw::vector<ItemId> consumerTargets(consumerOffsets.back());
  aw::vector<ItemId> consumerCursor(consumerOffsets);
  for (uint32_t item : order) {
    uint adjCursor = adjOffsets[item];
    for (RecipeId recipe : g.producersOf(item))
      for (ItemId input : g.inputsOf(recipe))
        adjTargets[adjCursor++] = input;
  }
  std::fill(recipeSeen.begin(), recipeSeen.end(), 0);
  for (uint32_t item : order) {
    for (RecipeId recipe : g.producersOf(item)) {
      if (recipeSeen[recipe])
        continue;
      recipeSeen[recipe] = 1;
      for (ItemId input : g.inputsOf(recipe))
        consumerTargets[consumerCursor[input]++] = recipe;
    }
  }

  aw::computeSccs(adjOffsets, adjTargets, order, component);

  consumers.offsets = std::move(consumerOffsets);
  consumers.targets = std::move(consumerTargets);
}

RankOrder rankProducible(const BaseCraftingGraph &g, const ReachableView &view,
                         std::span<const Amount> stock, bool stockSeeded) noexcept {
  RankOrder order(g.nItem, -1);
  aw::vector<int32_t> pending(g.nRecipe, 0);

  const aw::vector<int32_t> &distance = view.distance;
  const aw::vector<int32_t> &orderIndex = view.orderIndex;
  // Ascending (tier, level), then farthest from the target, then discovery order.
  // Tier is determined by:
  //   Tier 2. generated from nothing.
  //   Tier 1. stocked.
  //   Tier 0. others.
  const auto makeSortKey = [&](uint32_t item, uint64_t tier, uint64_t level) noexcept -> uint64_t {
    uint64_t dist = distance[item];
    uint64_t idx  = orderIndex[item];
    // Larger `dist` means better.
    uint64_t inv_dist = (~dist) & 0x3FFFFF; // 22 bit
    return (tier << 62) |
           ((level & 0x3FFFF) << 44) |
           (inv_dist << 22) |
           (idx & 0x3FFFFF);
  };
  std::priority_queue<Ready, std::vector<Ready>> ready;
  const auto push = [&](uint32_t item, int32_t tier, int32_t level) {
    ready.push(Ready{item, tier, level, makeSortKey(item, tier, level)});
  };

  // Seed the ready set. A raw leaf is fertile immediately. A stocked item is a
  // cut point of its own SCC in the stock-seeded pass, and an input-free recipe
  // is a producible entry point in both.
  for (uint32_t item : view.order) {
    const auto producers = g.producersOf(item);
    if (producers.empty()) {
      push(item, stockSeeded && stock[item] > 0 ? 0 : 2, 0);
      continue;
    }
    if (stockSeeded && stock[item] > 0)
      push(item, 1, 0);
    for (RecipeId recipe : producers) {
      const size_t dependencies = g.inputsOf(recipe).size();
      if (dependencies == 0)
        push(item, 0, 1);
      else
        pending[recipe] = (int32_t) dependencies;
    }
  }

  int32_t next = 0;
  while (!ready.empty()) {
    const Ready entry = ready.top();
    ready.pop();
    if (order[entry.item] >= 0)
      continue;
    order[entry.item] = next++;
    // Pops are monotone in (tier, level), so the last dependency to release a
    // recipe is the latest one; every output of the recipe inherits its tier.
    // A byproduct that is not itself in the target's input cone is skipped: it
    // has no rank of its own and nothing reads it through this view.
    for (ItemId consumer : view.consumers.targetsOf(entry.item)) {
      if (--pending[consumer] != 0)
        continue;
      for (ItemId out : g.outputsOf(consumer))
        if (view.orderIndex[out] >= 0)
          push(out, entry.tier, entry.level + 1);
    }
  }
  return order;
}

bool rankPruning(const ReachableView &view, const RankOrder &order, ItemId output,
               std::span<const ItemId> inputs) noexcept {
  const int32_t component = view.component[output];
  if (component < 0)
    return false;
  const int32_t own = order[output];
  for (ItemId input : inputs) {
    if (view.component[input] != component)
      continue;
    const int32_t dependency = order[input];
    if (own < 0 || dependency < 0 || dependency >= own)
      return true;
  }
  return false;
}

}  // namespace aw
