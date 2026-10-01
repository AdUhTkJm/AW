// The order-independent cycle cut. See RankedCut.h for the contract.
//
// Thunderbolt's `buildDag` cuts every recipe with an input that is still on the
// DFS stack. Which routes that keeps depends on arrival order, so a target
// whose needed SCC member is only reached through a detour can lose every
// producer of that member. The ranked cut below instead orients each SCC once
// from a hyperedge-Kahn order and keeps only inputs that are ranked strictly
// earlier, which is independent of how the DFS happens to walk the graph.

#include "RankedCut.h"

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

// Iterative Tarjan over the CSR adjacency `adjOffsets` / `adjTargets`.
// `component` is filled with the component id of every vertex reachable from a
// root in `roots`; every other entry keeps its -1 value.
void computeComponents(std::span<const uint32_t> roots, const aw::vector<NodeId> &adjOffsets,
                       const aw::vector<NodeId> &adjTargets,
                       aw::vector<int32_t> &component) noexcept {
  const size_t nItem = component.size();
  aw::vector<int32_t> index;
  aw::vector<int32_t> low;
  index.assign(nItem, -1);
  low.assign(nItem, -1);
  aw::vector<uint8_t> onStack = aw::vector<uint8_t>::zeroes(nItem);
  aw::vector<uint32_t> componentStack;
  aw::vector<uint32_t> nodeStack;
  aw::vector<NodeId> edgeStack;
  componentStack.reserve(nItem);
  nodeStack.reserve(nItem);
  edgeStack.reserve(nItem);

  int32_t timer = 0;
  int32_t nComponent = 0;
  for (uint32_t root : roots) {
    if (index[root] >= 0)
      continue;
    index[root] = low[root] = timer++;
    onStack[root] = 1;
    componentStack.push_back(root);
    nodeStack.push_back(root);
    edgeStack.push_back(adjOffsets[root]);
    while (!nodeStack.empty()) {
      const uint32_t node = nodeStack.back();
      if (edgeStack.back() < adjOffsets[node + 1]) {
        const uint32_t child = adjTargets[edgeStack.back()++];
        if (index[child] < 0) {
          index[child] = low[child] = timer++;
          onStack[child] = 1;
          componentStack.push_back(child);
          nodeStack.push_back(child);
          edgeStack.push_back(adjOffsets[child]);
        } else if (onStack[child] && index[child] < low[node]) {
          low[node] = index[child];
        }
        continue;
      }
      nodeStack.pop_back();
      edgeStack.pop_back();
      if (low[node] == index[node]) {
        for (;;) {
          const uint32_t member = componentStack.back();
          componentStack.pop_back();
          onStack[member] = 0;
          component[member] = nComponent;
          if (member == node)
            break;
        }
        nComponent++;
      }
      if (!nodeStack.empty()) {
        const uint32_t parent = nodeStack.back();
        if (low[node] < low[parent])
          low[parent] = low[node];
      }
    }
  }
}

}  // namespace

ReachableView::ReachableView(const BaseCraftingGraph &g, NodeId target) noexcept {
  const uint32_t nItem = g.nItem;
  component.assign(nItem, -1);
  distance.assign(nItem, -1);
  orderIndex.assign(nItem, -1);

  // BFS over the input cone. The first visit of an item is its shortest input
  // distance because every edge costs one hop.
  order.reserve(nItem);
  aw::vector<uint32_t> queue;
  queue.reserve(nItem);
  const auto discover = [&](NodeId item, int32_t dist) {
    if (orderIndex[item] >= 0)
      return;
    orderIndex[item] = (int32_t) order.size();
    distance[item] = dist;
    order.push_back(item);
    queue.push_back(item);
  };
  discover(target, 0);
  for (size_t head = 0; head < queue.size(); head++) {
    const uint32_t item = queue[head];
    const int32_t nextDistance = distance[item] + 1;
    for (NodeId producerNode : g.producersOf(item)) {
      const uint32_t recipe = producerNode - nItem;
      for (NodeId input : g.inputsOf(recipe))
        discover(input, nextDistance);
    }
  }

  // One pass counts the two CSRs so they can be sized exactly: `adjacency`
  // walks from an item to the inputs of its producers (the Tarjan graph), and
  // `consumers` walks from an item to the recipes that consume it (the Kahn
  // reverse graph). Only reachable items contribute.
  aw::vector<NodeId> adjOffsets = aw::vector<NodeId>::zeroes(nItem + 1);
  aw::vector<NodeId> consumerOffsets = aw::vector<NodeId>::zeroes(nItem + 1);
  size_t nEdge = 0;
  for (uint32_t item : order) {
    uint32_t count = 0;
    for (NodeId producerNode : g.producersOf(item)) {
      const auto inputs = g.inputsOf(producerNode - nItem);
      count += (uint32_t) inputs.size();
      for (NodeId input : inputs)
        consumerOffsets[input + 1]++;
    }
    adjOffsets[item + 1] = count;
    nEdge += count;
  }
  for (uint32_t item = 1; item <= nItem; item++) {
    adjOffsets[item] += adjOffsets[item - 1];
    consumerOffsets[item] += consumerOffsets[item - 1];
  }

  aw::vector<NodeId> adjTargets(nEdge);
  aw::vector<NodeId> consumerTargets(nEdge);
  aw::vector<NodeId> adjCursor(adjOffsets);
  aw::vector<NodeId> consumerCursor(consumerOffsets);
  for (uint32_t item : order) {
    for (NodeId producerNode : g.producersOf(item)) {
      const uint32_t recipe = producerNode - nItem;
      for (NodeId input : g.inputsOf(recipe)) {
        adjTargets[adjCursor[item]++] = input;
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
  RankOrder order;
  order.assign(g.nItem, -1);
  aw::vector<int32_t> pending;
  pending.assign(g.nRecipe, 0);

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
    for (NodeId producerNode : producers) {
      const uint32_t recipe = producerNode - g.nItem;
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
    for (NodeId consumer : view.consumers.targetsOf(entry.item)) {
      if (--pending[consumer] == 0)
        push(g.output[consumer], entry.tier, entry.level + 1);
    }
  }
  return order;
}

bool rankPruning(const ReachableView &view, const RankOrder &order, NodeId output,
               std::span<const NodeId> inputs) noexcept {
  const int32_t component = view.component[output];
  if (component < 0)
    return false;
  const int32_t own = order[output];
  for (NodeId input : inputs) {
    if (view.component[input] != component)
      continue;
    const int32_t dependency = order[input];
    if (own < 0 || dependency < 0 || dependency >= own)
      return true;
  }
  return false;
}

}  // namespace aw
