// Post-solve fireability check.
//
// The balance the solver enforces, `produced(i) - consumed(i) >= b_i`, is a net
// condition over the whole plan. It is satisfied by plans that can never be
// executed, because it can cancel a consumption that has to happen first
// against a production that only happens later. The classic shape is a cycle
// that needs a seed:
//
//   item1 <- r0 (input item2 x1)
//   item2 <- r1 (output item2 x2, input item1 x1)
//
// The loop's net output pays for its own seed on paper, so `r0:8, r1:4`
// balances a request of 4 item1, but nothing can fire from an empty stock.
//
// The support of a plan is almost always acyclic; only its cycles can have an
// ordering problem, and those are the only places this pass inspects. Condense
// the executed recipes by the edge "r consumes an item that p produces" into
// SCCs. A valid firing sequence can always be reordered so that an upstream SCC
// finishes before a downstream one starts -- upstream never reads a downstream
// output -- so the plan is fireable exactly when every SCC, in dependency
// order, can fire its own execution counts given the stock plus the net output
// of the SCCs already processed.
//
// The per-SCC test is a batched greedy: the first recipe that is enabled fires
// as many times in a row as its inputs allow, capped at its remaining count.
// A batch is never undone, and each batch removes at least one execution, so
// the loop terminates. A DAG costs one batch per recipe. The greedy order is
// not complete inside a strongly connected group -- two recipes can share a
// scarce seed, and firing the wrong one first can stall -- so a `false` means
// "do not trust this plan", which is exactly the CYCLE_UNFULFILLED contract.

#include "aw/plan/Plan.h"

#include <algorithm>
#include <cstdint>
#include <span>

#include "Prune.h"
#include "aw/utils/Int128.h"

namespace aw {
namespace {

// Total number of batches over every SCC. A DAG uses one per recipe; this only
// bounds a pathological cyclic core. Running out reports the plan as unproven,
// never as proven.
constexpr uint64_t FIRE_BUDGET = 1'000'000;

// One item's role inside one recipe: `consume` is how much the recipe eats and
// `delta` is its net change (output minus consumption).
struct DeltaEntry {
  NodeId item;
  Amount consume;
  Amount delta;
};

// `avail += change`, saturating at the int64 range. Everything the batch
// arithmetic needs fits in int64 except this accumulation, which a huge count
// times a huge amount can push past it; a saturated value is still "enough".
void applyDelta(Amount &avail, aw::int128 change) noexcept {
  const aw::int128 next = (aw::int128) avail + change;
  if (next > (aw::int128) INT64_MAX)
    avail = INT64_MAX;
  else if (next < (aw::int128) INT64_MIN)
    avail = INT64_MIN;
  else
    avail = (Amount) next;
}

}  // namespace

bool planIsFireable(const Subgraph &sub, std::span<const Amount> invSrc,
                    std::span<const int64_t> exec) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  const uint32_t n = g.nRecipe;
  const uint32_t m = g.nItem;
  if (exec.size() != n)
    return true;
  if (n == 0)
    return true;

  // ---- 1. Aggregate each recipe's inputs into per-item deltas. ----
  //
  // A recipe may list an item more than once, so the entries are merged before
  // the SCC test reads them.
  aw::vector<Amount> acc(m, 0);
  aw::vector<int32_t> stamp(m, -1);
  aw::vector<NodeId> touched;
  touched.reserve(m < 64 ? m : 64);

  aw::vector<uint> deltaOffsets(n + 1, 0);
  for (uint32_t r = 0; r < n; r++) {
    if (exec[r] <= 0) {
      deltaOffsets[r + 1] = deltaOffsets[r];
      continue;
    }
    touched.clear();
    const NodeId out = g.output[r];
    stamp[out] = (int32_t) r;
    acc[out] = g.outputAmt[r];
    touched.push_back(out);
    const auto inputs = g.r2i.targetsOf(r);
    const auto amounts = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++) {
      const NodeId item = inputs[k];
      if (stamp[item] != (int32_t) r) {
        stamp[item] = (int32_t) r;
        acc[item] = 0;
        touched.push_back(item);
      }
      acc[item] -= amounts[k];
    }
    deltaOffsets[r + 1] = deltaOffsets[r] + (uint) touched.size();
  }

  aw::vector<DeltaEntry> deltas(deltaOffsets[n]);
  for (uint32_t r = 0; r < n; r++) {
    if (exec[r] <= 0)
      continue;
    touched.clear();
    const NodeId out = g.output[r];
    stamp[out] = (int32_t) r;
    acc[out] = g.outputAmt[r];
    touched.push_back(out);
    const auto inputs = g.r2i.targetsOf(r);
    const auto amounts = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++) {
      const NodeId item = inputs[k];
      if (stamp[item] != (int32_t) r) {
        stamp[item] = (int32_t) r;
        acc[item] = 0;
        touched.push_back(item);
      }
      acc[item] -= amounts[k];
    }
    uint cursor = deltaOffsets[r];
    for (NodeId item : touched) {
      const Amount produced = item == out ? g.outputAmt[r] : 0;
      deltas[cursor].item = item;
      deltas[cursor].delta = acc[item];
      deltas[cursor].consume = produced - acc[item];
      cursor++;
    }
  }

  // ---- 2. Build the "r consumes an item p produces" graph. ----
  //
  // Deduplicated with a per-recipe stamp: an item with many producers would
  // otherwise add one edge per (recipe, producer) pair.
  aw::vector<int32_t> edgeStamp(n, -1);
  aw::vector<uint> adjOffsets(n + 1, 0);
  for (uint32_t r = 0; r < n; r++) {
    uint count = 0;
    if (exec[r] > 0) {
      for (NodeId item : g.r2i.targetsOf(r)) {
        for (NodeId producerNode : g.i2r.targetsOf(item)) {
          const uint32_t p = producerNode - g.nItem;
          if (exec[p] <= 0 || edgeStamp[p] == (int32_t) r)
            continue;
          edgeStamp[p] = (int32_t) r;
          count++;
        }
      }
    }
    adjOffsets[r + 1] = adjOffsets[r] + count;
  }

  aw::vector<uint32_t> adjTargets(adjOffsets[n]);
  aw::vector<uint> cursor(adjOffsets.begin(), adjOffsets.end() - 1);
  std::fill(edgeStamp.begin(), edgeStamp.end(), -1);
  for (uint32_t r = 0; r < n; r++) {
    if (exec[r] <= 0)
      continue;
    for (NodeId item : g.r2i.targetsOf(r)) {
      for (NodeId producerNode : g.i2r.targetsOf(item)) {
        const uint32_t p = producerNode - g.nItem;
        if (exec[p] <= 0 || edgeStamp[p] == (int32_t) r)
          continue;
        edgeStamp[p] = (int32_t) r;
        adjTargets[cursor[r]++] = p;
      }
    }
  }

  // Tarjan numbers the components in reverse topological order: a cross edge
  // a -> b has comp[b] < comp[a]. Our edges point from a consumer to its
  // producers, so ascending component order processes producers first.
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  const uint32_t nComp = detail::markSinkRepresentatives(
      n, adjOffsets, adjTargets, comp, repOfComp, keep);
  if (nComp == 0)
    return true;

  aw::vector<aw::vector<uint32_t>> compRecipes(nComp);
  for (uint32_t r = 0; r < n; r++)
    if (exec[r] > 0)
      compRecipes[comp[r]].push_back(r);

  // ---- 3. Fire every SCC in dependency order. ----
  aw::vector<Amount> avail(m, 0);
  for (uint32_t item = 0; item < m; item++) {
    const NodeId source = sub.itemOrigin[item];
    avail[item] = source < invSrc.size() ? invSrc[source] : 0;
  }

  aw::vector<int64_t> remaining(n, 0);
  uint64_t batchCount = 0;
  for (uint32_t c = 0; c < nComp; c++) {
    const aw::vector<uint32_t> &recipes = compRecipes[c];
    if (recipes.empty())
      continue;
    for (uint32_t r : recipes)
      remaining[r] = exec[r];

    while (true) {
      uint32_t chosen = UINT32_MAX;
      bool anyLeft = false;
      for (uint32_t r : recipes) {
        if (remaining[r] <= 0)
          continue;
        anyLeft = true;
        bool enabled = true;
        for (uint e = deltaOffsets[r]; e < deltaOffsets[r + 1]; e++) {
          if (deltas[e].consume > 0 && avail[deltas[e].item] < deltas[e].consume) {
            enabled = false;
            break;
          }
        }
        if (enabled) {
          chosen = r;
          break;
        }
      }
      if (!anyLeft)
        break;
      // Every remaining recipe needs more than the stock and the already-fired
      // output can supply: this group is a deadlock.
      if (chosen == UINT32_MAX)
        return false;

      // The most firings that can happen back to back. A net-consuming input
      // sets the batch; a net-producing one cannot run out inside the batch.
      int64_t batch = remaining[chosen];
      for (uint e = deltaOffsets[chosen]; e < deltaOffsets[chosen + 1]; e++) {
        const Amount consume = deltas[e].consume;
        const Amount delta = deltas[e].delta;
        if (consume <= 0 || delta >= 0)
          continue;
        const Amount extra = (avail[deltas[e].item] - consume) / (-delta);
        const int64_t bound = extra >= batch - 1 ? batch : extra + 1;
        if (bound < batch)
          batch = bound;
      }
      if (batch < 1)
        batch = 1;

      for (uint e = deltaOffsets[chosen]; e < deltaOffsets[chosen + 1]; e++)
        applyDelta(avail[deltas[e].item], (aw::int128) deltas[e].delta * (aw::int128) batch);
      remaining[chosen] -= batch;
      if (++batchCount > FIRE_BUDGET)
        return false;
    }
  }
  return true;
}

}  // namespace aw
