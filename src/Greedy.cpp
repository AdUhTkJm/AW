// Greedy DAG pre-pass.
//
// Builds an acyclic view of the reachable recipe graph by running a DFS from
// the target and cutting every recipe that would close a cycle (an input that
// is still on the DFS stack), then resolves each item once in reverse
// post-order: deduct stock, fire one producing recipe for the remaining
// deficit, and push the inputs' demand down the order.
//
// This mirrors the shape of Thunderbolt's `buildDag` + `linearPass`: it is
// O(V + E) and quantity-independent, and it returns a real firing vector
// whenever the retained acyclic routes already support the request. The
// cutting can delete routes a plan would need, so failure is expected and
// handled by the caller. The result is always re-checked against the exact
// balance `planCrafting` hands the solver, so it can never be a false
// positive.
//
// The caller uses the result as the whole answer in flash mode, or as the
// CP-SAT objective cap plus solution hint in the other modes.

#include "aw/Int128.h"
#include "aw/Plan.h"

#include <cstdint>
#include <span>

namespace aw {
namespace {

int64_t ceilDiv(int64_t numerator, int64_t denominator) {
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// Producer-selection order. The DAG is shared; only this ranking changes.
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

}  // namespace

aw::vector<int64_t> greedyDagPlan(const Subgraph &sub, NodeId target, Amount amount,
                                  std::span<const Amount> invSrc) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  const uint32_t nItem = g.nItem;
  const uint32_t nRecipe = g.nRecipe;
  if (target >= nItem || amount <= 0 || nRecipe == 0)
    return {};

  // Inventory for one subgraph item. The target's stock is deliberately not
  // subtracted: `rhs[target] = amount`, exactly as in planCrafting, so a valid
  // plan must still produce `amount` new units.
  const auto stockOf = [&](NodeId item) -> Amount {
    if (item == target)
      return 0;
    const NodeId source = sub.itemOrigin[item];
    return source < invSrc.size() ? invSrc[source] : 0;
  };

  // ---- 1. DFS from the target, cutting recipes that would close a cycle. ----
  // The stack is kept as three parallel PodVectors because PodVector only holds
  // integral types.
  aw::vector<uint8_t> color = aw::vector<uint8_t>::zeroes(nItem);    // 0 white, 1 gray, 2 black
  aw::vector<uint8_t> usable = aw::vector<uint8_t>::zeroes(nRecipe); // survived the cut
  aw::vector<uint32_t> post;  // finish order: inputs before their consumer
  post.reserve(nItem);
  {
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

  // ---- 2. Which items can bottom out in stock under the retained routes? ----
  // Optimistic: quantities are ignored, so this only guides producer choice.
  aw::vector<uint8_t> obtainable = aw::vector<uint8_t>::zeroes(nItem);
  for (uint32_t item : post) {
    if (stockOf(item) > 0) {
      obtainable[item] = 1;
      continue;
    }
    bool ok = false;
    for (NodeId producerNode : g.i2r.targetsOf(item)) {
      const uint32_t recipe = producerNode - nItem;
      if (!usable[recipe])
        continue;
      bool all = true;
      for (NodeId input : g.r2i.targetsOf(recipe))
        if (!obtainable[input]) {
          all = false;
          break;
        }
      if (all) {
        ok = true;
        break;
      }
    }
    obtainable[item] = ok ? 1 : 0;
  }

  // ---- 3. One demand sweep per strategy, in reverse post-order. ----
  const Strategy strategies[] = {Strategy::LargestBatch, Strategy::LeastInputMass};
  for (Strategy strategy : strategies) {
    aw::vector<int64_t> need = aw::vector<int64_t>::zeroes(nItem);
    aw::vector<int64_t> exec = aw::vector<int64_t>::zeroes(nRecipe);
    need[target] = amount;
    bool failed = false;

    for (size_t index = post.size(); index-- > 0 && !failed;) {
      const uint32_t item = post[index];
      const int64_t deficit = need[item] - stockOf(item);
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
          if (!usable[recipe])
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
      continue;

    // ---- 4. Verify the exact balance the solver will be handed. ----
    aw::vector<aw::int128> balance(nItem, 0);
    for (uint32_t recipe = 0; recipe < nRecipe; recipe++) {
      const int64_t times = exec[recipe];
      if (times == 0)
        continue;
      balance[g.output[recipe]] += (aw::int128) g.outputAmt[recipe] * times;
      const auto inputs = g.r2i.targetsOf(recipe);
      const auto weights = g.r2i.weightsOf(recipe);
      for (size_t k = 0; k < inputs.size(); k++)
        balance[inputs[k]] -= (aw::int128) weights[k] * times;
    }
    bool valid = true;
    for (uint32_t item = 0; item < nItem && valid; item++) {
      const aw::int128 required = item == target ? (aw::int128) amount : -(aw::int128) stockOf(item);
      if (balance[item] < required)
        valid = false;
    }
    if (valid)
      return exec;
  }

  return {};
}

}  // namespace aw
