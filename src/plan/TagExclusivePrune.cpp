// Per-item tag-exclusive producer elimination.
//
// docs/algorithm.typ ("tag-exclusive promotion") states the idea, as a per-item
// promotion of the class lemma there. This pass keeps only the part of that
// argument that is local to one item. Call a real item u
// tag-exclusive for a tag T when every consumer of u is a member edge of
// T. Suppose every output of a real recipe r is tag-exclusive for one and the
// same T, and r does not produce more M(T)-capacity than it consumes, where a
// member input and a tag input whose members all lie in M(T) both pay.
// Then r is never needed.
//
// To see it, take any plan x, let z = x restricted to the dropped recipes, and
// route T's member edges in x - z. Item supply outside M(T) only improves,
// because a dropped recipe outputs only M(T) members and its other inputs are
// freed. Summed over M(T),
//
//   delta = sum_inputs(M and tags inside M) - sum_outputs = -C_M(r) >= 0,
//   r in z
//
// so the total member supply available to T cannot fall; every fixed demand is
// still met, because a tag-exclusive item has no consumer other than a T member
// edge. Since T accepts any member, the member edges can be re-routed. Cost
// weakly drops, because only firings are removed.
//
// The pass is per item, not per class: it never needs the rest of M(T) to be
// closed under its own conversions. That is exactly the case a tag whose other
// members leak into real recipes leaves to the class fixpoint, and it is what
// the per-member input credit buys. All outputs have to be exclusive, not just
// the anchor: a byproduct with a real consumer would be lost. The target is
// excluded; the inventory needs no guard, because a stocked member feeds T with
// no conversion.

#include "Prune.h"

#include <algorithm>
#include <chrono>

#include "aw/plan/Options.h"

namespace aw {

namespace {

using uint = uint32_t;

constexpr ItemId kNoTag = UINT32_MAX;

// One query's tag index plus the per-item exclusivity flags. Everything is
// built once, then every real recipe is tested against it.
struct TagExclusiveRun {
  const BaseCraftingGraph &g;
  const ItemId target;
  const uint nReal;
  const uint nItem;
  const uint nRecipe;
  // Real recipes are a prefix of the recipe list, tag edges the suffix.
  const uint nRealRecipe;

  aw::vector<uint8_t> simple;                 // nItem
  aw::vector<uint8_t> tagHasRealConsumer;     // nItem
  aw::vector<aw::vector<ItemId>> members;     // nItem, tags only
  aw::vector<aw::vector<ItemId>> memberTags;  // nReal, tags containing
  aw::vector<ItemId> exclusiveTag;            // nReal, kNoTag when not exclusive

  const std::chrono::steady_clock::time_point started;

  TagExclusiveRun(const Subgraph &sub, ItemId target,
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
    if (options.tagExclusive.maxSeconds <= 0)
      return true;
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    return elapsed < options.tagExclusive.maxSeconds;
  }

  // A tag is simple when every recipe producing it is a single-member edge.
  void buildTagIndex() noexcept {
    simple.assign(nItem, 0);
    tagHasRealConsumer.assign(nItem, 0);
    members.assign(nItem, {});
    memberTags.assign(nReal, {});

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
      for (ItemId in : g.inputsOf(r))
        if (in >= nReal && simple[in])
          tagHasRealConsumer[in] = 1;
  }

  // exclusiveTag[v] is the single tag whose member edges consume v, when
  // that is the only way v is consumed. A real consumer or a second tag clears
  // it.
  void computeExclusive() noexcept {
    exclusiveTag.assign(nReal, kNoTag);
    aw::vector<uint8_t> bad(nReal, 0);

    for (uint r = 0; r < nRecipe; r++) {
      const ItemId out = g.output[r];
      const bool tagEdge = out >= nReal;
      for (ItemId in : g.inputsOf(r)) {
        if (in >= nReal)
          continue;
        if (!tagEdge || !simple[out]) {
          bad[in] = 1;
          continue;
        }
        if (exclusiveTag[in] == kNoTag)
          exclusiveTag[in] = out;
        else if (exclusiveTag[in] != out)
          bad[in] = 1;
      }
    }
    for (ItemId v = 0; v < nReal; v++)
      if (bad[v])
        exclusiveTag[v] = kNoTag;
  }

  [[nodiscard]]
  bool isMemberOf(ItemId v, ItemId t) const noexcept {
    for (ItemId u : memberTags[v])
      if (u == t)
        return true;
    return false;
  }

  // True when every member of the tag t2 is also a member of t. Consuming
  // t2 then always spends an item of M(t).
  [[nodiscard]]
  bool tagInside(ItemId t2, ItemId t) const noexcept {
    if (!simple[t2] || members[t2].empty())
      return false;
    for (ItemId m : members[t2])
      if (!isMemberOf(m, t))
        return false;
    return true;
  }

  // M(t)-capacity consumed by recipe r: the member inputs plus the tag
  // inputs whose members all lie in M(t).
  [[nodiscard]]
  Amount paidCapacity(uint r, ItemId t) const noexcept {
    Amount paid = 0;
    const auto ins = g.inputsOf(r);
    const auto amts = g.inputAmountsOf(r);
    for (size_t k = 0; k < ins.size(); k++) {
      const ItemId in = ins[k];
      if (in < nReal) {
        if (isMemberOf(in, t))
          paid += amts[k];
      } else if (tagInside(in, t)) {
        paid += amts[k];
      }
    }
    return paid;
  }

  // Marks r when every output is tag-exclusive for one simple, really consumed
  // tag, the target is not among them, and r is not a net source of M(t).
  bool considerRecipe(uint r, aw::vector<uint8_t> &drop) const noexcept {
    ItemId t = kNoTag;
    Amount outCapacity = 0;
    for (size_t k = g.r2o.offsets[r]; k < g.r2o.offsets[r + 1]; k++) {
      const ItemId out = g.r2o.targets[k];
      if (out >= nReal || out == target)
        return false;
      const ItemId et = exclusiveTag[out];
      if (et == kNoTag)
        return false;
      if (t == kNoTag)
        t = et;
      else if (t != et)
        return false;
      outCapacity += g.r2o.weights[k];
    }
    if (t == kNoTag || !simple[t] || !tagHasRealConsumer[t])
      return false;
    if (outCapacity > paidCapacity(r, t))
      return false;
    drop[r] = 1;
    return true;
  }
};

}  // namespace

// Per-query tag-exclusive producer elimination. See the file comment. `target`
// is a subgraph item and `drop` is indexed by subgraph recipe, sized to
// `sub.graph.nRecipe`. Returns true when something was marked.
bool computeTagExclusivePruning(QUERY_PRUNE_PARAM_LIST) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  if (!options.tagExclusive.enabled)
    return false;
  if (target >= g.nReal || drop.size() != g.nRecipe || g.nItem == g.nReal)
    return false;

  const auto started = std::chrono::steady_clock::now();
  TagExclusiveRun run(sub, target, started);
  run.buildTagIndex();
  run.computeExclusive();

  bool any = false;
  for (uint r = 0; r < run.nRealRecipe; r++) {
    if (!run.timeLeft())
      break;
    if (run.considerRecipe(r, drop))
      any = true;
  }
  return any;
}

}  // namespace aw
