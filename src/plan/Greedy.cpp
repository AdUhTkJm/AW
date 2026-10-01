// Greedy DAG pre-pass.
//
// Builds an acyclic view of the reachable recipe graph by running a DFS from
// the target and cutting every recipe that would close a cycle (an input that
// is still on the DFS stack), then resolves each item once in reverse
// post-order: deduct stock, choose producing recipes for the remaining deficit,
// and push the inputs' demand down the order.
//
// This mirrors the shape of Thunderbolt's `buildDag` + `linearPass`: it is
// O(V + E) and quantity-independent, and it returns a real firing vector
// whenever the retained acyclic routes already support the request. The
// cutting can delete routes a plan would need, so failure is expected and
// handled by the caller. The result is always re-checked against the exact
// balance `planCrafting` hands the solver, so it can never be a false
// positive.
//
// Two mechanisms are ported from Thunderbolt's recent planner updates:
//
//   * The capacity-reserved sweep (`capacitySweep`) replaces "one best recipe
//     per deficit" with a global reservation. A route only fires while the
//     inputs it needs still have capacity (`stock + producible`), so a single
//     lossy conversion can no longer multiply demand by orders of magnitude
//     across a long chain; the sweep declines instead.
//   * The ranked cut (`RankedCut.h`) rebuilds the cycle cut from an
//     order-independent hyperedge-Kahn order. The plain DFS cut depends on
//     arrival order and can drop every producer of a needed cycle member.
//
// The caller uses the result as the whole answer in flash mode, or as the
// CP-SAT objective cap plus solution hint in the other modes.

#include "aw/plan/Plan.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

#include "aw/utils/Helpers.h"
#include "aw/utils/Int128.h"
#include "RankedCut.h"

namespace aw {
namespace {

int64_t ceilDiv(int64_t numerator, int64_t denominator) {
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// Saturating arithmetic for the optimistic capacity bound.
// Everything is non-negative.
int64_t satAdd(int64_t lhs, int64_t rhs) noexcept {
  int64_t result = 0;
  return addOverflow(lhs, rhs, result) ? INT64_MAX : result;
}

int64_t satMul(int64_t lhs, int64_t rhs) noexcept {
  int64_t result = 0;
  return mulOverflow(lhs, rhs, result) ? INT64_MAX : result;
}

// Producer-selection tie-break. The DAG is shared; only this ranking changes.
enum class Strategy : uint8_t {
  // Fewest executions: prefer the largest output batch.
  LargestBatch = 0,
  // Least input mass: prefer the smallest total consumed amount.
  LeastInputMass = 1,
};

// True when producer `rhs` is strictly better than producer `lhs` under
// `strategy`.
bool better(const BaseCraftingGraph &g, Strategy strategy, uint32_t lhs, uint32_t rhs) {
  if (strategy == Strategy::LargestBatch) {
    if (g.outputAmt[rhs] != g.outputAmt[lhs])
      return g.outputAmt[rhs] > g.outputAmt[lhs];
    return g.r2i.targetsOf(rhs).size() < g.r2i.targetsOf(lhs).size();
  }
  int64_t lhsMass = 0;
  int64_t rhsMass = 0;
  for (Amount weight : g.r2i.weightsOf(lhs))
    lhsMass += weight;
  for (Amount weight : g.r2i.weightsOf(rhs))
    rhsMass += weight;
  if (lhsMass != rhsMass)
    return rhsMass < lhsMass;
  return g.outputAmt[rhs] > g.outputAmt[lhs];
}

// The acyclic view of the reachable recipe graph.
struct AcyclicView {
  // Post-order of items visited.
  aw::vector<uint32_t> post;
  // usable[r]: recipe `r` survived the cut.
  aw::vector<uint8_t> usable;

  AcyclicView(const BaseCraftingGraph &g, NodeId target, const ReachableView *reach, const RankOrder *rank) noexcept;
};

// DFS from `target`, cutting every recipe that would close a cycle (an input
// that is still on the DFS stack) and, when a ranked cut is supplied, every
// recipe whose SCC-internal inputs are not ranked strictly earlier.
AcyclicView::AcyclicView(const BaseCraftingGraph &g, NodeId target,
                         const ReachableView *reach, const RankOrder *rank) noexcept {
  const uint32_t nItem = g.nItem;
  const uint32_t nRecipe = g.nRecipe;
  usable = aw::vector<uint8_t>::zeroes(nRecipe);
  post.reserve(nItem);

  aw::vector<uint8_t> color = aw::vector<uint8_t>::zeroes(nItem);  // 0 white, 1 gray, 2 black
  aw::vector<uint32_t> stackItem;
  aw::vector<uint32_t> stackRecipe;
  aw::vector<uint32_t> stackInput;
  stackItem.reserve(nItem);
  stackRecipe.reserve(nItem);
  stackInput.reserve(nItem);
  color[target] = 1;
  stackItem.push_back(target);
  stackRecipe.push_back(0);
  stackInput.push_back(0);
  while (!stackItem.empty()) {
    const uint32_t item = stackItem.back();
    const auto producers = g.i2r.targetsOf(item);
    if (stackRecipe.back() >= producers.size()) {
      color[item] = 2;
      post.push_back(item);
      stackItem.pop_back();
      stackRecipe.pop_back();
      stackInput.pop_back();
      continue;
    }
    const uint32_t recipe = producers[stackRecipe.back()] - nItem;
    const auto inputs = g.r2i.targetsOf(recipe);
    // The ranked cut is applied before the back-edge test: dropping a recipe
    // also drops its children from the DFS frontier, so the traversal (and not
    // just the retained set) changes, exactly as in Thunderbolt's frameFor.
    if (reach != nullptr && cutByRank(*reach, *rank, item, inputs)) {
      stackRecipe.back()++;
      stackInput.back() = 0;
      continue;
    }
    bool closesCycle = false;
    for (NodeId input : inputs)
      if (color[input] == 1) {
        closesCycle = true;
        break;
      }
    if (closesCycle) {
      stackRecipe.back()++;
      stackInput.back() = 0;
      continue;
    }
    if (stackInput.back() < inputs.size()) {
      const NodeId input = inputs[stackInput.back()++];
      if (color[input] == 0) {
        color[input] = 1;
        stackItem.push_back(input);
        stackRecipe.push_back(0);
        stackInput.push_back(0);
      }
      continue;
    }
    usable[recipe] = 1;
    stackRecipe.back()++;
    stackInput.back() = 0;
  }
}

// Which items can bottom out in stock under the retained routes? Optimistic:
// quantities are ignored, so this only guides producer choice.
aw::vector<uint8_t> computeObtainable(const BaseCraftingGraph &g, const AcyclicView &view,
                                      std::span<const Amount> stock) noexcept {
  aw::vector<uint8_t> obtainable = aw::vector<uint8_t>::zeroes(g.nItem);
  for (uint32_t item : view.post) {
    if (stock[item] > 0) {
      obtainable[item] = 1;
      continue;
    }
    bool ok = false;
    for (NodeId producerNode : g.i2r.targetsOf(item)) {
      const uint32_t recipe = producerNode - g.nItem;
      if (!view.usable[recipe])
        continue;
      auto targets = g.r2i.targetsOf(recipe);
      if (std::ranges::all_of(targets, [&](NodeId id) { return obtainable[id]; })) {
        ok = true;
        break;
      }
    }
    obtainable[item] = ok;
  }
  return obtainable;
}

// How much of a recipe's output can be made from the optimistic capacity left
// in its inputs. INT64_MAX means "unbounded", which only an input-free recipe
// reaches.
int64_t producibleVia(const BaseCraftingGraph &g, uint32_t recipe,
                      std::span<const int64_t> cap) noexcept {
  const auto inputs = g.r2i.targetsOf(recipe);
  const auto weights = g.r2i.weightsOf(recipe);
  int64_t firings = INT64_MAX;
  for (size_t k = 0; k < inputs.size(); k++) {
    const int64_t weight = weights[k];
    if (weight <= 0)
      continue;
    const int64_t possible = cap[inputs[k]] / weight;
    if (possible < firings)
      firings = possible;
    if (firings == 0)
      return 0;
  }
  if (firings == INT64_MAX)
    return INT64_MAX;
  return satMul(firings, g.outputAmt[recipe]);
}

// Remaining capacity of a recipe: the same combination as `producibleVia`, but
// against `cap - need`, so `need` doubles as the global reservation.
int64_t capRemainingVia(const BaseCraftingGraph &g, std::span<const int64_t> cap,
                        std::span<const int64_t> need, uint32_t recipe) noexcept {
  const auto inputs = g.r2i.targetsOf(recipe);
  const auto weights = g.r2i.weightsOf(recipe);
  int64_t firings = INT64_MAX;
  for (size_t k = 0; k < inputs.size(); k++) {
    const int64_t weight = weights[k];
    if (weight <= 0)
      continue;
    const int64_t remaining = cap[inputs[k]] - need[inputs[k]];
    const int64_t possible = remaining > 0 ? remaining / weight : 0;
    if (possible < firings)
      firings = possible;
    if (firings == 0)
      return 0;
  }
  if (firings == INT64_MAX)
    return INT64_MAX;
  return satMul(firings, g.outputAmt[recipe]);
}

// A candidate route during one item's allocation, with the capacity score it
// was ordered by.
struct Route {
  uint32_t recipe;
  int64_t capacity;
};

// The fixed inputs of one demand sweep, bundled so both sweep shapes stay
// within the argument budget and share one stock snapshot.
struct SweepContext {
  const BaseCraftingGraph &g;
  const AcyclicView &view;
  std::span<const Amount> stock;
  NodeId target;
  Amount amount;

  aw::vector<int64_t> capacityFromOrder() const noexcept;
};

// Returns a vector `cap`, where cap[x] is an upper bound of needed amount of `x`.
aw::vector<int64_t> SweepContext::capacityFromOrder() const noexcept {
  aw::vector<int64_t> cap(g.nItem, 0);
  for (uint32_t item : view.post) {
    int64_t best = 0;
    for (NodeId producerNode : g.i2r.targetsOf(item)) {
      const uint32_t recipe = producerNode - g.nItem;
      if (!view.usable[recipe])
        continue;
      const int64_t amount = producibleVia(g, recipe, cap);
      if (amount > best)
        best = amount;
    }
    cap[item] = satAdd(stock[item], best);
  }
  return cap;
}

// RPO demand sweep under `strategy`. Returns the plan.
aw::vector<int64_t> capacitySweep(const SweepContext &ctx, Strategy strategy) noexcept {
  const BaseCraftingGraph &g = ctx.g;
  const AcyclicView &view = ctx.view;
  const std::span<const Amount> stock = ctx.stock;
  const aw::vector<int64_t> cap = ctx.capacityFromOrder();
  aw::vector<int64_t> need(g.nItem, 0);
  aw::vector<int64_t> exec(g.nRecipe, 0);
  need[ctx.target] = ctx.amount;
  aw::vector<Route> routes;

  for (size_t index = view.post.size(); index-- > 0;) {
    const uint32_t item = view.post[index];
    int64_t deficit = need[item];
    if (deficit <= 0)
      continue;
    deficit -= stock[item];
    if (deficit <= 0)
      continue;

    routes.clear();
    for (NodeId producerNode : g.i2r.targetsOf(item)) {
      const uint32_t recipe = producerNode - g.nItem;
      if (!view.usable[recipe] || g.outputAmt[recipe] <= 0)
        continue;
      routes.push_back(Route{recipe, capRemainingVia(g, cap, need, recipe)});
    }
    // Most remaining capacity first. Splitting a deficit only where capacity
    // exists is what keeps a lossy route from exploding the demand downstream.
    std::sort(routes.begin(), routes.end(), [&](const Route &lhs, const Route &rhs) {
      if (lhs.capacity != rhs.capacity)
        return lhs.capacity > rhs.capacity;
      return better(g, strategy, rhs.recipe, lhs.recipe);
    });

    for (const Route &route : routes) {
      if (deficit <= 0)
        break;
      const int64_t available = capRemainingVia(g, cap, need, route.recipe);
      if (available <= 0)
        continue;
      const int64_t batch = g.outputAmt[route.recipe];
      const int64_t make = std::min(deficit, available);
      const int64_t times = ceilDiv(make, batch);
      // A clamped total is not a plan: decline rather than hand back a
      // saturated firing vector the balance check cannot judge.
      if (exec[route.recipe] > INT64_MAX - times)
        return {};
      exec[route.recipe] += times;
      const auto inputs = g.r2i.targetsOf(route.recipe);
      const auto weights = g.r2i.weightsOf(route.recipe);
      for (size_t k = 0; k < inputs.size(); k++) {
        const int64_t additional = satMul(weights[k], times);
        if (need[inputs[k]] > INT64_MAX - additional)
          return {};
        need[inputs[k]] += additional;
      }
      deficit -= std::min(deficit, satMul(times, batch));
    }
    if (deficit > 0)
      return {};
  }
  return exec;
}

// One demand sweep in reverse post-order under `strategy`: deduct stock, fire
// one producing recipe for the remaining deficit, and push the inputs' demand
// down the order. Returns the firing vector, or an empty vector on failure.
// Kept as the fallback the ranked cut and capacity sweep are compared against.
aw::vector<int64_t> demandSweep(const SweepContext &ctx,
                                const aw::vector<uint8_t> &obtainable,
                                Strategy strategy) noexcept {
  const BaseCraftingGraph &g = ctx.g;
  const AcyclicView &view = ctx.view;
  const std::span<const Amount> stock = ctx.stock;
  const uint32_t nItem = g.nItem;
  aw::vector<int64_t> need = aw::vector<int64_t>::zeroes(nItem);
  aw::vector<int64_t> exec = aw::vector<int64_t>::zeroes(g.nRecipe);
  need[ctx.target] = ctx.amount;
  bool failed = false;

  for (size_t index = view.post.size(); index-- > 0 && !failed;) {
    const uint32_t item = view.post[index];
    const int64_t deficit = need[item] - stock[item];
    if (deficit <= 0)
      continue;

    // Prefer a producer whose inputs are all obtainable. Fall back to any
    // usable producer so an over-pessimistic obtainability guess cannot hide
    // a route; the balance check below still rejects a dead end.
    uint32_t best = UINT32_MAX;
    for (int pass = 0; pass < 2 && best == UINT32_MAX; pass++) {
      const bool requireObtainable = pass == 0;
      for (NodeId producerNode : g.i2r.targetsOf(item)) {
        const uint32_t recipe = producerNode - nItem;
        if (!view.usable[recipe])
          continue;
        if (requireObtainable) {
          bool all = true;
          for (NodeId input : g.r2i.targetsOf(recipe))
            if (!obtainable[input]) {
              all = false;
              break;
            }
          if (!all)
            continue;
        }
        if (best == UINT32_MAX || better(g, strategy, best, recipe))
          best = recipe;
      }
    }
    if (best == UINT32_MAX) {
      failed = true;
      break;
    }
    const int64_t batch = g.outputAmt[best];
    if (batch <= 0) {
      failed = true;
      break;
    }
    const int64_t times = ceilDiv(deficit, batch);
    exec[best] += times;
    const auto inputs = g.r2i.targetsOf(best);
    const auto weights = g.r2i.weightsOf(best);
    for (size_t k = 0; k < inputs.size(); k++) {
      const aw::int128 next = (aw::int128) need[inputs[k]] + (aw::int128) weights[k] * times;
      if (next > (aw::int128) INT64_MAX) {
        failed = true;
        break;
      }
      need[inputs[k]] = (int64_t) next;
    }
  }
  if (failed)
    return {};
  return exec;
}

// Verify the exact balance the solver will be handed: `exec` must cover the
// requested amount of `target` plus every other item's stock deficit.
bool satisfiesBalance(const BaseCraftingGraph &g, const aw::vector<int64_t> &exec,
                      std::span<const Amount> stock, NodeId target, Amount amount) noexcept {
  const uint32_t nItem = g.nItem;
  aw::vector<aw::int128> balance(nItem, 0);
  for (uint32_t recipe = 0; recipe < g.nRecipe; recipe++) {
    const int64_t times = exec[recipe];
    if (times == 0)
      continue;
    balance[g.output[recipe]] += (aw::int128) g.outputAmt[recipe] * times;
    const auto inputs = g.r2i.targetsOf(recipe);
    const auto weights = g.r2i.weightsOf(recipe);
    for (size_t k = 0; k < inputs.size(); k++)
      balance[inputs[k]] -= (aw::int128) weights[k] * times;
  }
  for (uint32_t item = 0; item < nItem; item++) {
    const aw::int128 required = item == target ? (aw::int128) amount : -(aw::int128) stock[item];
    if (balance[item] < required)
      return false;
  }
  return true;
}

// The solver's objective, as a hint on search direction.
aw::int128 executionCost(const BaseCraftingGraph &g, std::span<const int64_t> exec) noexcept {
  aw::int128 cost = 0;
  // The real recipes are a prefix of the recipe list, so the first synthetic
  // tag edge ends the sum. Tag edges are not charged a step. See the ordering
  // note on BaseCraftingGraph::output.
  for (uint32_t recipe = 0; recipe < g.nRecipe && g.output[recipe] < g.nReal; recipe++)
    cost += exec[recipe];
  return cost;
}

// The cheapest balance-checked plan seen so far. Every candidate is a complete
// firing vector over the same subgraph, so their costs are comparable even when
// they come from different acyclic views.
struct BestPlan {
  aw::vector<int64_t> exec;
  aw::int128 cost = 0;
  bool found = false;

  void consider(const BaseCraftingGraph &g, aw::vector<int64_t> &&candidate,
                std::span<const Amount> stock, NodeId target, Amount amount) {
    if (candidate.empty())
      return;
    if (!satisfiesBalance(g, candidate, stock, target, amount))
      return;
    const aw::int128 candidateCost = executionCost(g, candidate);
    if (found && candidateCost >= cost)
      return;
    exec = std::move(candidate);
    cost = candidateCost;
    found = true;
  }
};

}  // namespace

aw::vector<int64_t> greedyDagPlan(const Subgraph &sub, NodeId target, Amount amount,
                                  std::span<const Amount> invSrc) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  if (target >= g.nItem || amount <= 0 || g.nRecipe == 0)
    return {};

  // Inventory for one subgraph item. The target's stock is deliberately not
  // subtracted: `rhs[target] = amount`, exactly as in planCrafting, so a valid
  // plan must still produce `amount` new units.
  aw::vector<Amount> stock(g.nItem, 0);
  for (uint32_t item = 0; item < g.nItem; item++) {
    if (item == target)
      continue;
    const NodeId source = sub.itemOrigin[item];
    stock[item] = source < invSrc.size() ? invSrc[source] : 0;
  }
  const std::span<const Amount> stockSpan(stock.data(), stock.size());

  // Evaluate every plain-view pass and keep the cheapest balance-checked plan.
  // The capacity-reserved sweep is what stops a lossy conversion from
  // multiplying demand, while the two plain sweeps can be cheaper when the
  // reservation over-commits; taking the minimum keeps both properties.
  // Flash mode returns this plan directly, so cost is not cosmetic here.
  const AcyclicView plain(g, target, nullptr, nullptr);
  const aw::vector<uint8_t> plainObtainable = computeObtainable(g, plain, stockSpan);
  const SweepContext plainSweep{g, plain, stockSpan, target, amount};
  BestPlan best;
  best.consider(g, capacitySweep(plainSweep, Strategy::LargestBatch), stockSpan, target, amount);
  best.consider(g, demandSweep(plainSweep, plainObtainable, Strategy::LargestBatch), stockSpan,
                target, amount);
  best.consider(g, demandSweep(plainSweep, plainObtainable, Strategy::LeastInputMass), stockSpan,
                target, amount);

  // The plain DFS can drop every producer of a needed cycle member. Rebuild the
  // cut from an order-independent ranking and retry only when the plain view
  // found nothing: ranked plans keep coverage, not cost.
  if (!best.found) {
    const ReachableView reachable = analyzeReachable(g, target);
    const bool stockSeededModes[] = {true, false};
    for (bool stockSeeded : stockSeededModes) {
      const RankOrder order = rankProducible(g, reachable, stockSpan, stockSeeded);
      const AcyclicView ranked(g, target, &reachable, &order);
      const SweepContext rankedSweep{g, ranked, stockSpan, target, amount};
      best.consider(g, capacitySweep(rankedSweep, Strategy::LargestBatch), stockSpan, target,
                    amount);
      const aw::vector<uint8_t> obtainable = computeObtainable(g, ranked, stockSpan);
      best.consider(g, demandSweep(rankedSweep, obtainable, Strategy::LargestBatch), stockSpan,
                    target, amount);
    }
  }
  return std::move(best.exec);
}

}  // namespace aw
