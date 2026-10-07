// Interchangeable-variant (tag-orbit) elimination.
//
// docs/algorithm.typ ("变体类消除") has the statement. In one line: a tag T
// whose members V are mutually interchangeable at every external interface
// makes the conversions inside V useless, because any plan already directly
// produces at least as many members as it spends through T's member edges.
//
// Concretely, let V be a set of real items and call a tag a V-tag when all of
// its members lie in V. Suppose
//
//   1. every consumer of a member v of V is either an internal recipe (a
//      V-tag's member edge, or a real recipe whose outputs all lie in V and
//      whose V plus V-tag consumption pays for its V production), or a member
//      edge into a tag that still contains all of V (an exit tag);
//   2. every internal real recipe produces at most as much of V as it consumes
//      of V plus V-tags (it is not a net source of variants).
//
// Then every internal recipe can be dropped. The plan that used them already
// contains at least as many direct productions of V as it has exit firings (an
// internal recipe conserves or shrinks V), and every exit tag accepts every
// member, so those direct productions can simply be routed to the exits.
//
// The pass runs per query on the assembled subgraph, where the target and the
// inventory are known. The target is excluded: a query for a variant names it
// directly and its own producers have to stay. Stock needs no guard, because a
// stocked member can feed an exit tag without a conversion.
//
// The class V is found from a candidate exit tag t0: start from members(t0)
// and repeatedly remove a member that breaks (1) or (2), until nothing is
// removed. The result is the largest subset of members(t0) that survives, and
// every candidate is re-checked before any recipe is marked.

#include "Prune.h"

#include <algorithm>
#include <chrono>
#include <span>

#include "aw/plan/Options.h"

namespace aw {

namespace {

using uint = uint32_t;

// One query's worth of scratch and the per-candidate fixpoint. Members, member
// edges and real consumers are built once; the per-candidate flags are reused.
struct VariantClassRun {
  const BaseCraftingGraph &g;
  const ItemId target;
  const uint nReal;
  const uint nItem;
  const uint nRecipe;
  // Real recipes are a prefix of the recipe list, tag edges the suffix.
  const uint nRealRecipe;

  // Tag structure, indexed by pseudo item for `members` and by real item for
  // the two back indices.
  aw::vector<aw::vector<ItemId>> members;          // nItem, pseudo entries only
  aw::vector<uint8_t> simple;                      // nItem
  aw::vector<uint8_t> tagHasRealConsumer;          // nItem
  aw::vector<aw::vector<ItemId>> memberTags;       // nReal
  aw::vector<aw::vector<RecipeId>> realConsumers;  // nReal

  // Per-candidate flags.
  aw::vector<uint8_t> inV;        // nReal
  aw::vector<uint8_t> vtag;       // nItem
  aw::vector<uint8_t> internalR;  // nRecipe
  aw::vector<uint8_t> dead;       // nItem
  aw::vector<ItemId> classItems;
  uint classSize = 0;

  const std::chrono::steady_clock::time_point started;

  VariantClassRun(const Subgraph &sub, ItemId target,
                  std::chrono::steady_clock::time_point started) noexcept
      : g(sub.graph), target(target), nReal(sub.graph.nReal),
        nItem(sub.graph.nItem), nRecipe(sub.graph.nRecipe),
        nRealRecipe(firstTagEdge(sub.graph)), started(started) {}

  // Index of the first synthetic tag edge, i.e. the count of real recipes.
  static uint firstTagEdge(const BaseCraftingGraph &graph) noexcept {
    uint r = 0;
    while (r < graph.nRecipe && graph.output[r] < graph.nReal)
      r++;
    return r;
  }

  [[nodiscard]]
  bool timeLeft() const noexcept {
    if (options.variantClass.maxSeconds <= 0)
      return true;
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    return elapsed < options.variantClass.maxSeconds;
  }

  // A tag is simple when every recipe producing it is a single-member edge.
  void buildTagIndex() noexcept {
    members.assign(nItem, {});
    simple.assign(nItem, 0);
    tagHasRealConsumer.assign(nItem, 0);
    memberTags.assign(nReal, {});
    realConsumers.assign(nReal, {});

    aw::vector<ItemId> scratch;
    for (ItemId t = nReal; t < nItem; t++) {
      const auto recipes = g.producersOf(t);
      if (recipes.empty())
        continue;
      scratch.clear();
      bool ok = true;
      for (RecipeId r : recipes) {
        const auto outs = g.outputsOf(r);
        const auto ins = g.inputsOf(r);
        if (outs.size() != 1 || ins.size() != 1 || ins[0] >= nReal) {
          ok = false;
          break;
        }
        scratch.push_back(ins[0]);
      }
      if (!ok)
        continue;
      std::sort(scratch.begin(), scratch.end());
      scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
      simple[t] = 1;
      for (ItemId m : scratch)
        memberTags[m].push_back(t);
      members[t].assign(scratch.begin(), scratch.end());
    }

    for (uint r = 0; r < nRealRecipe; r++)
      for (ItemId in : g.inputsOf(r)) {
        if (in < nReal)
          realConsumers[in].push_back(r);
        else if (simple[in])
          tagHasRealConsumer[in] = 1;
      }
  }

  // vtag[t] = every member of t is currently in V.
  void computeTags() noexcept {
    for (ItemId t = nReal; t < nItem; t++) {
      if (!simple[t] || members[t].empty()) {
        vtag[t] = 0;
        continue;
      }
      bool all = true;
      for (ItemId m : members[t])
        if (!inV[m]) {
          all = false;
          break;
        }
      vtag[t] = all ? 1 : 0;
    }
  }

  // A real recipe is internal when all of its outputs lie in V and its V plus
  // V-tag inputs pay for its V output.
  void computeInternal() noexcept {
    std::fill(internalR.begin(), internalR.begin() + nRealRecipe, 0);
    const aw::vector<ItemId> &outsAll = g.r2o.targets;
    const aw::vector<Amount> &outAmtsAll = g.r2o.weights;
    for (uint r = 0; r < nRealRecipe; r++) {
      Amount outV = 0;
      bool all = true;
      for (size_t k = g.r2o.offsets[r]; k < g.r2o.offsets[r + 1]; k++) {
        if (!inV[outsAll[k]]) {
          all = false;
          break;
        }
        outV += outAmtsAll[k];
      }
      if (!all)
        continue;
      Amount paid = 0;
      for (size_t k = g.r2i.offsets[r]; k < g.r2i.offsets[r + 1]; k++) {
        const ItemId in = g.r2i.targets[k];
        if (in < nReal) {
          if (inV[in])
            paid += g.r2i.weights[k];
        } else if (vtag[in]) {
          paid += g.r2i.weights[k];
        }
      }
      if (outV <= paid)
        internalR[r] = 1;
    }
  }

  // A V-tag is dead when every real recipe consuming it is internal, so its
  // member edges feed nothing that survives. A V-tag no recipe consumes is
  // dead too.
  void computeDead() noexcept {
    for (ItemId t = nReal; t < nItem; t++)
      dead[t] = vtag[t];
    for (uint r = 0; r < nRealRecipe; r++) {
      if (internalR[r])
        continue;
      for (size_t k = g.r2i.offsets[r]; k < g.r2i.offsets[r + 1]; k++) {
        const ItemId in = g.r2i.targets[k];
        if (in >= nReal && vtag[in])
          dead[in] = 0;
      }
    }
  }

  // True when V is a subset of the members of `t`.
  [[nodiscard]]
  bool classInsideTag(ItemId t) const noexcept {
    if (members[t].size() < classSize)
      return false;
    uint hit = 0;
    for (ItemId m : members[t])
      if (inV[m])
        hit++;
    return hit == classSize;
  }

  // Removes every member that breaks condition (1) or (2). Returns true when
  // anything was removed.
  bool shrinkRound() noexcept {
    bool changed = false;
    for (ItemId v : classItems) {
      if (!inV[v])
        continue;
      bool bad = false;
      for (RecipeId r : realConsumers[v]) {
        if (!internalR[r]) {
          bad = true;
          break;
        }
      }
      if (!bad) {
        for (ItemId t : memberTags[v]) {
          if (vtag[t] && dead[t])
            continue;
          if (classInsideTag(t))
            continue;
          bad = true;
          break;
        }
      }
      if (bad) {
        inV[v] = 0;
        classSize--;
        changed = true;
      }
    }
    return changed;
  }

  // Runs the fixpoint from members(t0). Returns false when the class is empty,
  // contains the target, or runs out of budget.
  bool collectCandidate(ItemId t0) noexcept {
    if (members[t0].empty() ||
        members[t0].size() > options.variantClass.maxClassNodes)
      return false;
    std::fill(inV.begin(), inV.end(), 0);
    classItems.clear();
    classSize = 0;
    for (ItemId m : members[t0]) {
      if (m == target)
        return false;
      inV[m] = 1;
      classItems.push_back(m);
      classSize++;
    }

    const uint maxRounds = (uint) classItems.size() + 1;
    for (uint round = 0; round < maxRounds; round++) {
      computeTags();
      computeInternal();
      computeDead();
      if (!shrinkRound())
        return classSize != 0;
      if (classSize == 0)
        return false;
      if (!timeLeft())
        return false;
    }
    return false;
  }

  // Marks the internal recipes and the member edges of every dead V-tag.
  void markDrops(aw::vector<uint8_t> &drop) noexcept {
    for (uint r = 0; r < nRealRecipe; r++)
      if (internalR[r])
        drop[r] = 1;
    for (ItemId t = nReal; t < nItem; t++) {
      if (!vtag[t] || !dead[t])
        continue;
      for (RecipeId r : g.producersOf(t))
        drop[r] = 1;
    }
  }
};

}  // namespace

// Per-query interchangeable-variant elimination. See the file comment and
// docs/algorithm.typ. `target` is a subgraph item and `drop` is indexed by
// subgraph recipe, sized to `sub.graph.nRecipe`. Returns true when something
// was marked.
bool computeVariantClassPruning(QUERY_PRUNE_PARAM_LIST) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  if (!options.variantClass.enabled)
    return false;
  if (target >= g.nReal || drop.size() != g.nRecipe || g.nItem == g.nReal)
    return false;

  const auto started = std::chrono::steady_clock::now();
  VariantClassRun run(sub, target, started);
  run.buildTagIndex();
  run.inV.assign(g.nReal, 0);
  run.vtag.assign(g.nItem, 0);
  run.internalR.assign(g.nRecipe, 0);
  run.dead.assign(g.nItem, 0);

  bool any = false;
  uint candidates = 0;
  for (ItemId t0 = g.nReal; t0 < g.nItem; t0++) {
    if (!run.simple[t0] || !run.tagHasRealConsumer[t0])
      continue;
    if (candidates++ >= options.variantClass.maxCandidates || !run.timeLeft())
      break;
    if (!run.collectCandidate(t0))
      continue;
    run.markDrops(drop);
    any = true;
  }
  return any;
}

}  // namespace aw
