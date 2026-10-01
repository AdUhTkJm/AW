// The order-independent cycle cut. See RankedCut.h for the contract.
//
// Thunderbolt's `buildDag` cuts every recipe with an input that is still on the
// DFS stack. Which routes that keeps depends on arrival order, so a target
// whose needed SCC member is only reached through a detour can lose every
// producer of that member. The ranked cut below instead orients each SCC once
// from a hyperedge-Kahn order and keeps only inputs that are ranked strictly
// earlier, which is independent of how the DFS happens to walk the graph.

#include "RankedCut.h"
#include "aw/utils/LargeStackCall.h"

#include <cstdint>
#include <queue>
#include <vector>

namespace aw {
namespace {

// A pending item in the ranked Kahn order. Pops are monotone in (tier, level),
// so the last dependency to release a recipe is also the latest one, and its
// tier is inherited by the recipe's output.
struct Ready {
  int32_t tier;
  int32_t level;
  uint32_t item;
};

// Standard Tarjan algorithm.
struct TarjanContext {
  const aw::vector<ItemId> &adjOffsets;
  const aw::vector<ItemId> &adjTargets;
  aw::vector<int32_t> index;
  aw::vector<int32_t> low;
  aw::vector<uint8_t> onStack;
  aw::vector<uint32_t> componentStack;
  aw::vector<int32_t> &component;
  int32_t timer = 0;
  int32_t nComponent = 0;

  TarjanContext(const aw::vector<ItemId> &adjOffsets, const aw::vector<ItemId> &adjTargets, aw::vector<int32_t> &component, uint nItem):
    adjOffsets(adjOffsets), adjTargets(adjTargets), index(nItem, -1), low(nItem, -1), onStack(nItem, 0), component(component)
  {
    componentStack.reserve(nItem);
  }
};

void tarjanDFS(TarjanContext &ctx, uint32_t node) noexcept {
  ctx.index[node] = ctx.low[node] = ctx.timer++;
  ctx.onStack[node] = 1;
  ctx.componentStack.push_back_unchecked(node);

  const ItemId startEdge = ctx.adjOffsets[node];
  const ItemId endEdge = ctx.adjOffsets[node + 1];

  for (ItemId e = startEdge; e < endEdge; ++e) {
    const uint32_t child = ctx.adjTargets[e];

    if (ctx.index[child] < 0) {
      tarjanDFS(ctx, child);
      if (ctx.low[child] < ctx.low[node])
        ctx.low[node] = ctx.low[child];
    } else if (ctx.onStack[child]) {
      if (ctx.index[child] < ctx.low[node])
        ctx.low[node] = ctx.index[child];
    }
  }

  if (ctx.low[node] == ctx.index[node]) {
    for (;;) {
      const uint32_t member = ctx.componentStack.back();
      ctx.componentStack.pop_back();
      ctx.onStack[member] = 0;
      ctx.component[member] = ctx.nComponent;
      if (member == node)
        break;
    }
    ctx.nComponent++;
  }
}

// Visits every root that no earlier root reached.
void tarjanAll(TarjanContext &ctx, std::span<const uint32_t> roots) noexcept {
  for (uint32_t root : roots) {
    if (ctx.index[root] < 0)
      tarjanDFS(ctx, root);
  }
}

// Fills `component` in place; unreached entries keep their -1 value.
void computeComponents(std::span<const uint32_t> roots,
                       const aw::vector<ItemId> &adjOffsets,
                       const aw::vector<ItemId> &adjTargets,
                       aw::vector<int32_t> &component) noexcept {
  TarjanContext ctx(adjOffsets, adjTargets, component, component.size());
  // Entered on the large stack.
  aw::ls::call(tarjanAll, ctx, roots);
}

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

  // First compute offsets.
  size_t nEdge = 0;
  for (uint32_t item : order) {
    uint32_t count = 0;
    for (RecipeId recipe : g.producersOf(item)) {
      const auto inputs = g.inputsOf(recipe);
      count += (uint32_t) inputs.size();
      for (ItemId input : inputs)
        consumerOffsets[input + 1]++;
    }
    adjOffsets[item + 1] = count;
    nEdge += count;
  }
  for (uint32_t item = 1; item <= nItem; item++) {
    adjOffsets[item] += adjOffsets[item - 1];
    consumerOffsets[item] += consumerOffsets[item - 1];
  }

  // Then fill in the data.
  aw::vector<ItemId> adjTargets(nEdge);
  aw::vector<ItemId> consumerTargets(nEdge);
  aw::vector<ItemId> consumerCursor(consumerOffsets);
  for (uint32_t item : order) {
    uint adjCursor = adjOffsets[item];
    for (RecipeId recipe : g.producersOf(item)) {
      for (ItemId input : g.inputsOf(recipe)) {
        adjTargets[adjCursor++] = input;
        consumerTargets[consumerCursor[input]++] = recipe;
      }
    }
  }

  computeComponents(order, adjOffsets, adjTargets, component);

  consumers.offsets = std::move(consumerOffsets);
  consumers.targets = std::move(consumerTargets);
}

RankOrder rankProducible(const BaseCraftingGraph &g, const ReachableView &view,
                         std::span<const Amount> stock, bool stockSeeded) noexcept {
  RankOrder order(g.nItem, -1);
  aw::vector<int32_t> pending(g.nRecipe, 0);

  const aw::vector<int32_t> &distance = view.distance;
  const aw::vector<int32_t> &orderIndex = view.orderIndex;
  // Ascending (tier, level), then farthest from the target, then discovery
  // order. std::priority_queue is a max-heap, so every comparison is inverted.
  const auto compare = [&distance, &orderIndex](const Ready &lhs, const Ready &rhs) {
    if (lhs.tier != rhs.tier)
      return lhs.tier > rhs.tier;
    if (lhs.level != rhs.level)
      return lhs.level > rhs.level;
    if (distance[lhs.item] != distance[rhs.item])
      return distance[lhs.item] < distance[rhs.item];
    return orderIndex[lhs.item] > orderIndex[rhs.item];
  };
  std::priority_queue<Ready, std::vector<Ready>, decltype(compare)> ready(compare);
  const auto push = [&ready](uint32_t item, int32_t tier, int32_t level) {
    ready.push(Ready{tier, level, item});
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
    // recipe is the latest one; the output inherits its tier.
    for (ItemId consumer : view.consumers.targetsOf(entry.item)) {
      if (--pending[consumer] == 0)
        push(g.output[consumer], entry.tier, entry.level + 1);
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
