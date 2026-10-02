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
#include "aw/utils/LargeStackCall.h"
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
    return g.inputsOf(rhs).size() < g.r2i.targetsOf(lhs).size();
  }
  int64_t lhsMass = 0;
  int64_t rhsMass = 0;
  for (Amount weight : g.inputAmountsOf(lhs))
    lhsMass += weight;
  for (Amount weight : g.inputAmountsOf(rhs))
    rhsMass += weight;
  if (lhsMass != rhsMass)
    return rhsMass < lhsMass;
  return g.outputAmt[rhs] > g.outputAmt[lhs];
}

#define ACYCLIC_ARG_LIST const BaseCraftingGraph &g, ItemId target, const ReachableView *reach, const RankOrder *rank

// The acyclic view of the reachable recipe graph.
struct AcyclicView {
  // Post-order of items visited.
  aw::vector<uint32_t> post;
  // usable[r]: recipe `r` survived the cut.
  aw::vector<uint8_t> usable;

  AcyclicView(ACYCLIC_ARG_LIST) noexcept;
private:
  enum Status : uint8_t {
    UNSEEN = 0, IN_STACK, DONE
  };
  aw::vector<Status> status;
  void initImpl(ACYCLIC_ARG_LIST) noexcept;
};

void AcyclicView::initImpl(ACYCLIC_ARG_LIST) noexcept {
  status[target] = IN_STACK;
  const auto producers = g.producersOf(target);

  for (RecipeId recipe : producers) {
    const auto inputs = g.inputsOf(recipe);

    // Prune.
    if (reach != nullptr && rankPruning(*reach, *rank, target, inputs))
      continue;

    // Cycle check.
    if (std::ranges::any_of(inputs, [&](ItemId input) { return status[input] == IN_STACK; }))
      continue;

    // DFS for unseen items.
    for (ItemId item : inputs) {
      if (status[item] == UNSEEN)
        initImpl(g, item, reach, rank);
    }

    // Everything works well. The recipe is preserved.
    usable[recipe] = 1;
  }

  status[target] = DONE;
  post.push_back_unchecked(target);
}

AcyclicView::AcyclicView(ACYCLIC_ARG_LIST) noexcept: usable(g.nRecipe, 0), status(g.nItem, UNSEEN) {
  post.reserve(g.nItem);
  aw::ls::call([this](auto&&... args) { this->initImpl(args...); }, g, target, reach, rank);
}

// Whether each item is obtainable, ignoring amounts.
aw::vector<uint8_t> computeObtainable(const BaseCraftingGraph &g, const AcyclicView &view,
                                      std::span<const Amount> stock) noexcept {
  aw::vector<uint8_t> obtainable = aw::vector<uint8_t>::zeroes(g.nItem);
  for (uint32_t item : view.post) {
    // Item with stock is of course obtainable.
    if (stock[item] > 0) {
      obtainable[item] = 1;
      continue;
    }

    // If there exists a usable recipe, and every item of it is obtainable,
    // then so is the current one.
    bool ok = false;
    for (RecipeId recipe : g.producersOf(item)) {
      if (!view.usable[recipe])
        continue;
      auto targets = g.inputsOf(recipe);
      if (std::ranges::all_of(targets, [&](ItemId id) { return obtainable[id]; })) {
        ok = true;
        break;
      }
    }
    obtainable[item] = ok;
  }
  return obtainable;
}

// How many units of `item` `recipe` can craft, given `cap`.
int64_t producibleVia(const BaseCraftingGraph &g, uint32_t recipe, ItemId item,
                      std::span<const int64_t> cap) noexcept {
  const auto inputs = g.inputsOf(recipe);
  const auto weights = g.inputAmountsOf(recipe);
  int64_t firings = INT64_MAX;
  for (size_t k = 0; k < inputs.size(); k++) {
    firings = std::min(cap[inputs[k]] / weights[k], firings);
    if (firings == 0)
      return 0;
  }
  return satMul(firings, g.producedAmountOf(recipe, item));
}

// A candidate recipe to craft an item.
// We record `capacity` as an ordering heuristic.
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
  ItemId target;
  Amount amount;

  aw::vector<int64_t> capacityFromOrder() const noexcept;
};

// Returns a vector `cap`, where cap[x] is an upper bound of needed amount of `x`.
aw::vector<int64_t> SweepContext::capacityFromOrder() const noexcept {
  aw::vector<int64_t> cap(g.nItem, 0);
  for (uint32_t item : view.post) {
    int64_t best = 0;
    for (RecipeId recipe : g.producersOf(item)) {
      if (!view.usable[recipe])
        continue;
      best = std::max(best, producibleVia(g, recipe, item, cap));
    }
    cap[item] = satAdd(stock[item], best);
  }
  return cap;
}

// RPO propagation on the DAG. Returns the plan. 
aw::vector<int64_t> capacitySweep(const SweepContext &ctx, Strategy strategy) noexcept {
  const BaseCraftingGraph &g = ctx.g;
  const AcyclicView &view = ctx.view;
  const std::span<const Amount> stock = ctx.stock;
  aw::vector<int64_t> cap = ctx.capacityFromOrder();
  aw::vector<int64_t> need(g.nItem, 0);
  aw::vector<int64_t> exec(g.nRecipe, 0);
  need[ctx.target] = ctx.amount;
  aw::vector<Route> routes;

  for (uint index = view.post.size(); index-- > 0;) {
    const uint32_t item = view.post[index];
    int64_t deficit = need[item] - stock[item];
    if (deficit <= 0)
      continue;

    // Collect usable recipes to craft `item`.
    routes.clear();
    for (RecipeId recipe : g.producersOf(item)) {
      if (!view.usable[recipe])
        continue;
      routes.push_back(Route{recipe, producibleVia(g, recipe, item, cap)});
    }
    
    // Sort them under heuristic.
    std::sort(routes.begin(), routes.end(), [&](const Route &lhs, const Route &rhs) {
      if (lhs.capacity != rhs.capacity)
        return lhs.capacity > rhs.capacity;
      return better(g, strategy, rhs.recipe, lhs.recipe);
    });

    for (const Route &route : routes) {
      if (deficit <= 0)
        break;
      const int64_t craftable = producibleVia(g, route.recipe, item, cap);
      if (craftable <= 0)
        continue;
      const int64_t batch = g.producedAmountOf(route.recipe, item);
      const int64_t make = std::min(deficit, craftable);
      const int64_t times = ceilDiv(make, batch);
      // When the plan is saturated, we cannot tell.
      [[unlikely]]
      if (exec[route.recipe] > INT64_MAX - times)
        return {};
      exec[route.recipe] += times;

      // Mark the inputs of this recipe as needed.
      const auto inputs = g.inputsOf(route.recipe);
      const auto weights = g.inputAmountsOf(route.recipe);
      for (size_t k = 0; k < inputs.size(); k++) {
        const int64_t additional = satMul(weights[k], times);
        [[unlikely]]
        if (need[inputs[k]] > INT64_MAX - additional)
          return {};
        need[inputs[k]] += additional;
        cap[inputs[k]] = std::max<int64_t>(0, cap[inputs[k]] - additional);
      }
      deficit -= std::min(deficit, satMul(times, batch));
    }
    if (deficit > 0)
      return {};
  }
  return exec;
}

// RPO propagation, but of a different form. Returns the plan.
//
// This time we try to identify a single best recipe and go all the way through it.
aw::vector<int64_t> demandSweep(const SweepContext &ctx, const aw::vector<uint8_t> &obtainable, Strategy strategy) noexcept {
  const BaseCraftingGraph &g = ctx.g;
  const AcyclicView &view = ctx.view;
  const std::span<const Amount> stock = ctx.stock;
  const uint32_t nItem = g.nItem;
  aw::vector<int64_t> need(nItem, 0);
  aw::vector<int64_t> exec(g.nRecipe, 0);
  need[ctx.target] = ctx.amount;

  for (size_t index = view.post.size(); index-- > 0;) {
    const uint32_t item = view.post[index];
    const int64_t deficit = need[item] - stock[item];
    if (deficit <= 0)
      continue;

    // Prefer a producer whose inputs are all obtainable.
    uint32_t best = UINT32_MAX;
    for (RecipeId recipe : g.producersOf(item)) {
      if (!view.usable[recipe])
        continue;
      if (!std::ranges::all_of(g.inputsOf(recipe), [&](ItemId input) { return obtainable[input]; }))
        continue;
      if (best == UINT32_MAX || better(g, strategy, best, recipe))
        best = recipe;
    }

    // No recipe available. Give up.
    if (best == UINT32_MAX)
      return {};
    
    const int64_t times = ceilDiv(deficit, g.producedAmountOf(best, item));
    exec[best] += times;
    const auto inputs = g.inputsOf(best);
    const auto weights = g.inputAmountsOf(best);
    for (size_t k = 0; k < inputs.size(); k++) {
      const aw::int128 next = (aw::int128) need[inputs[k]] + (aw::int128) weights[k] * times;
      if (next > (aw::int128) INT64_MAX)
        return {};

      need[inputs[k]] = (int64_t) next;
    }
  }
  return exec;
}

// The solver's objective, as a hint on search direction.
aw::int128 executionCost(const BaseCraftingGraph &g, std::span<const int64_t> exec) noexcept {
  aw::int128 cost = 0;
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
  const BaseCraftingGraph &g;
  const std::span<const Amount> stock;
  const Amount amount;
  const ItemId target;
  bool found = false;

  BestPlan(const BaseCraftingGraph &g, std::span<const Amount> stock, ItemId target, Amount amount):
    g(g), stock(stock), amount(amount), target(target) {}

  void consider(aw::vector<int64_t> &&candidate) noexcept;
};

void BestPlan::consider(aw::vector<int64_t> &&candidate) noexcept {
  if (candidate.empty())
    return;
  const aw::int128 candidateCost = executionCost(g, candidate);
  if (found && candidateCost >= cost)
    return;
  exec = std::move(candidate);
  cost = candidateCost;
  found = true;
}

}  // namespace

aw::vector<int64_t> greedyDagPlan(const Subgraph &sub, ItemId target, Amount amount,
                                  std::span<const Amount> invSrc) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  if (target >= g.nItem || amount <= 0 || g.nRecipe == 0)
    return {};

  // Inventory for one subgraph item. The target's stock is deliberately not
  // subtracted: `rhs[target] = amount`, exactly as in planCrafting, so a valid
  // plan must still produce `amount` new units.
  aw::vector<Amount> stk(g.nItem, 0);
  for (uint32_t item = 0; item < g.nReal; item++) {
    const ItemId source = sub.itemOrigin[item];
    stk[item] = source < invSrc.size() ? invSrc[source] : 0;
  }
  stk[target] = 0;
  
  const std::span<const Amount> stock(stk.data(), stk.size());

  // First try basic DAG cut with normal DFS.
  const AcyclicView plain(g, target, nullptr, nullptr);
  const aw::vector<uint8_t> plainObtainable = computeObtainable(g, plain, stock);
  const SweepContext plainSweep{g, plain, stock, target, amount};
  BestPlan best(g, stock, target, amount);
  best.consider(capacitySweep(plainSweep, Strategy::LargestBatch));
  best.consider(demandSweep(plainSweep, plainObtainable, Strategy::LargestBatch));
  best.consider(demandSweep(plainSweep, plainObtainable, Strategy::LeastInputMass));

  // `rankProducible` needs this to be safe, since it's packing bits into a `uint64_t`.
  // In real world we probably won't hit this.
  [[unlikely]]
  if (g.nItem >= 262144)
    return std::move(best.exec);

  // Then try a more sophisticated cut.
  if (!best.found) {
    const ReachableView reachable(g, target);
    const bool stockSeededModes[] = {true, false};
    for (bool stockSeeded : stockSeededModes) {
      const RankOrder order = rankProducible(g, reachable, stock, stockSeeded);
      const AcyclicView ranked(g, target, &reachable, &order);
      const SweepContext rankedSweep{g, ranked, stock, target, amount};
      best.consider(capacitySweep(rankedSweep, Strategy::LargestBatch));
      const aw::vector<uint8_t> obtainable = computeObtainable(g, ranked, stock);
      best.consider(demandSweep(rankedSweep, obtainable, Strategy::LargestBatch));
    }
  }

  // NRVO doesn't work here, we're not returning the whole `best`.
  return std::move(best.exec);
}

}  // namespace aw
