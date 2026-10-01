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

// Per-recipe item deltas as a CSR: recipe r owns
// entries[offsets[r] .. offsets[r + 1]).
struct RecipeDeltas {
  aw::vector<uint> offsets;      // nRecipe + 1
  aw::vector<DeltaEntry> entries;
};

// The "r consumes an item p produces" graph over the executed recipes, as a
// CSR: consumer r owns producers targets[offsets[r] .. offsets[r + 1]).
struct ProducerGraph {
  aw::vector<uint> offsets;      // nRecipe + 1
  aw::vector<uint32_t> targets;
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

// Aggregate recipe `r`'s output and inputs into per-item net changes in `acc`,
// appending the touched items to `touched`. A recipe may list an item more
// than once, so the entries are merged with the per-recipe `stamp` before the
// SCC test reads them.
void aggregateRecipe(const BaseCraftingGraph &g, uint32_t r, aw::vector<Amount> &acc,
                     aw::vector<int32_t> &stamp, aw::vector<NodeId> &touched) noexcept {
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
}

// Aggregate every executed recipe's inputs into per-item deltas. A recipe may
// list an item more than once, so the entries are merged before the SCC test
// reads them.
RecipeDeltas buildRecipeDeltas(const BaseCraftingGraph &g,
                               std::span<const int64_t> exec) noexcept {
  const uint32_t n = g.nRecipe;
  const uint32_t m = g.nItem;
  RecipeDeltas deltas;
  deltas.offsets.assign(n + 1, 0);

  // Scratch space for the per-recipe aggregation, reused across both passes.
  aw::vector<Amount> acc(m, 0);
  aw::vector<int32_t> stamp(m, -1);
  aw::vector<NodeId> touched;
  touched.reserve(m < 64 ? m : 64);

  // Size pass.
  for (uint32_t r = 0; r < n; r++) {
    if (exec[r] <= 0) {
      deltas.offsets[r + 1] = deltas.offsets[r];
      continue;
    }
    aggregateRecipe(g, r, acc, stamp, touched);
    deltas.offsets[r + 1] = deltas.offsets[r] + (uint) touched.size();
  }

  // Fill pass.
  deltas.entries.resize(deltas.offsets[n]);
  for (uint32_t r = 0; r < n; r++) {
    if (exec[r] <= 0)
      continue;
    aggregateRecipe(g, r, acc, stamp, touched);
    uint cursor = deltas.offsets[r];
    for (NodeId item : touched) {
      const Amount produced = item == g.output[r] ? g.outputAmt[r] : 0;
      deltas.entries[cursor].item = item;
      deltas.entries[cursor].delta = acc[item];
      deltas.entries[cursor].consume = produced - acc[item];
      cursor++;
    }
  }
  return deltas;
}

// Build the "r consumes an item p produces" graph, deduplicated with a
// per-recipe stamp: an item with many producers would otherwise add one edge
// per (recipe, producer) pair.
ProducerGraph buildProducerGraph(const BaseCraftingGraph &g,
                                 std::span<const int64_t> exec) noexcept {
  const uint32_t n = g.nRecipe;
  ProducerGraph adj;
  adj.offsets.assign(n + 1, 0);

  // Size pass.
  aw::vector<int32_t> edgeStamp(n, -1);
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
    adj.offsets[r + 1] = adj.offsets[r] + count;
  }

  // Fill pass.
  adj.targets.resize(adj.offsets[n]);
  aw::vector<uint> cursor(adj.offsets.begin(), adj.offsets.end() - 1);
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
        adj.targets[cursor[r]++] = p;
      }
    }
  }
  return adj;
}

// The first recipe of `recipes` that still has executions left and can afford
// one firing, or UINT32_MAX. Sets `anyLeft` when any recipe remains.
uint32_t firstEnabled(const aw::vector<uint32_t> &recipes,
                      const aw::vector<int64_t> &remaining, const RecipeDeltas &deltas,
                      const aw::vector<Amount> &avail, bool &anyLeft) noexcept {
  anyLeft = false;
  for (uint32_t r : recipes) {
    if (remaining[r] <= 0)
      continue;
    anyLeft = true;
    bool enabled = true;
    for (uint e = deltas.offsets[r]; e < deltas.offsets[r + 1]; e++) {
      if (deltas.entries[e].consume > 0 && avail[deltas.entries[e].item] < deltas.entries[e].consume) {
        enabled = false;
        break;
      }
    }
    if (enabled)
      return r;
  }
  return UINT32_MAX;
}

// The most firings of `chosen` that can happen back to back. A net-consuming
// input sets the batch; a net-producing one cannot run out inside the batch.
int64_t maxBatch(uint32_t chosen, int64_t remaining, const RecipeDeltas &deltas,
                 const aw::vector<Amount> &avail) noexcept {
  int64_t batch = remaining;
  for (uint e = deltas.offsets[chosen]; e < deltas.offsets[chosen + 1]; e++) {
    const Amount consume = deltas.entries[e].consume;
    const Amount delta = deltas.entries[e].delta;
    if (consume <= 0 || delta >= 0)
      continue;
    const Amount extra = (avail[deltas.entries[e].item] - consume) / (-delta);
    const int64_t bound = extra >= batch - 1 ? batch : extra + 1;
    if (bound < batch)
      batch = bound;
  }
  return batch < 1 ? 1 : batch;
}

// Everything one firing pass needs. `witness`, when set, collects the blockers
// of the first deadlock instead of just reporting it.
struct FireInput {
  const Subgraph &sub;
  std::span<const Amount> invSrc;
  std::span<const int64_t> exec;
  const RecipeDeltas &deltas;
  const aw::vector<int32_t> &comp;
  uint32_t nComp;
  FireabilityWitness *witness = nullptr;
};

// Fire every SCC in dependency order with the batched greedy from the file
// header. False means a group deadlocked or the batch budget ran out.
bool fireComponents(const FireInput &in) noexcept {
  const Subgraph &sub = in.sub;
  const RecipeDeltas &deltas = in.deltas;
  const aw::vector<int32_t> &comp = in.comp;
  const uint32_t nComp = in.nComp;
  const BaseCraftingGraph &g = sub.graph;
  const uint32_t n = g.nRecipe;
  const uint32_t m = g.nItem;

  aw::vector<aw::vector<uint32_t>> compRecipes(nComp);
  for (uint32_t r = 0; r < n; r++)
    if (in.exec[r] > 0)
      compRecipes[comp[r]].push_back(r);

  aw::vector<Amount> avail(m, 0);
  for (uint32_t item = 0; item < m; item++) {
    const NodeId source = sub.itemOrigin[item];
    avail[item] = source < in.invSrc.size() ? in.invSrc[source] : 0;
  }

  aw::vector<int64_t> remaining(n, 0);
  uint64_t batchCount = 0;
  for (uint32_t c = 0; c < nComp; c++) {
    const aw::vector<uint32_t> &recipes = compRecipes[c];
    if (recipes.empty())
      continue;
    for (uint32_t r : recipes)
      remaining[r] = in.exec[r];

    while (true) {
      bool anyLeft;
      const uint32_t chosen = firstEnabled(recipes, remaining, deltas, avail, anyLeft);
      if (!anyLeft)
        break;
      // Every remaining recipe needs more than the stock and the already-fired
      // output can supply: this group is a deadlock.
      if (chosen == UINT32_MAX) {
        if (in.witness != nullptr) {
          for (uint32_t r : recipes) {
            if (remaining[r] <= 0)
              continue;
            // One blocker is enough to describe the recipe: the first input it
            // is short of.
            for (uint e = deltas.offsets[r]; e < deltas.offsets[r + 1]; e++) {
              const DeltaEntry &d = deltas.entries[e];
              if (d.consume > 0 && avail[d.item] < d.consume) {
                in.witness->recipe.push_back(r);
                in.witness->item.push_back(d.item);
                in.witness->need.push_back(d.consume);
                break;
              }
            }
          }
        }
        return false;
      }

      const int64_t batch = maxBatch(chosen, remaining[chosen], deltas, avail);
      for (uint e = deltas.offsets[chosen]; e < deltas.offsets[chosen + 1]; e++)
        applyDelta(avail[deltas.entries[e].item],
                   (aw::int128) deltas.entries[e].delta * (aw::int128) batch);
      remaining[chosen] -= batch;
      if (++batchCount > FIRE_BUDGET)
        return false;
    }
  }
  return true;
}

}  // namespace

bool planIsFireable(const Subgraph &sub, std::span<const Amount> invSrc,
                    std::span<const int64_t> exec, FireabilityWitness *witness) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  const uint32_t n = g.nRecipe;
  if (exec.size() != n || n == 0)
    return true;

  const RecipeDeltas deltas = buildRecipeDeltas(g, exec);
  const ProducerGraph adj = buildProducerGraph(g, exec);

  // Tarjan numbers the components in reverse topological order: a cross edge
  // a -> b has comp[b] < comp[a]. Our edges point from a consumer to its
  // producers, so ascending component order processes producers first.
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  const uint32_t nComp = detail::markSinkRepresentatives(
      n, adj.offsets, adj.targets, comp, repOfComp, keep);
  if (nComp == 0)
    return true;

  return fireComponents(FireInput{sub, invSrc, exec, deltas, comp, nComp, witness});
}

}  // namespace aw
