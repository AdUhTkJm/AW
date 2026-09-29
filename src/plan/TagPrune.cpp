#include "Prune.h"
#include "aw/plan/Options.h"

namespace aw::detail {

using PairKey = uint64_t;
using uint = uint32_t;

constexpr PairKey packPair(NodeId m, NodeId w) noexcept {
  return ((PairKey) m << 32) | (PairKey) w;
}

void prefixSum(aw::vector<uint> &v) noexcept {
  v.insert(v.begin(), 0);
  for (uint i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
}

void intersect(const aw::vector<NodeId> &a, const aw::vector<NodeId> &b,
                     aw::vector<NodeId> &out) noexcept {
  out.clear();
  out.reserve(std::min(a.size(), b.size()));
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] < b[j]) {
      i++;
    } else if (b[j] < a[i]) {
      j++;
    } else {
      out.push_back_unchecked(a[i]);
      i++;
      j++;
    }
  }
}

// Merges the (unsorted, possibly duplicated) `cand` into the sorted-unique
// `dst`, keeping `dst` sorted, and returns the keys that were not already in
// it. The closure below inserts one frontier at a time, so appending a sorted
// batch beats probing a hash table and matches the sort/unique style of the
// rest of the pass.
aw::vector<PairKey> mergeNewPairs(aw::vector<PairKey> &dst,
                                   aw::vector<PairKey> &cand) noexcept {
  std::sort(cand.begin(), cand.end());
  cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
  aw::vector<PairKey> added;
  if (cand.empty())
    return added;
  // Every new key comes from `cand`, so size the result up front.
  added.reserve(cand.size());

  aw::vector<PairKey> merged;
  merged.reserve(dst.size() + cand.size());
  size_t i = 0, j = 0;
  while (i < dst.size() && j < cand.size()) {
    if (dst[i] < cand[j]) {
      merged.push_back_unchecked(dst[i++]);
    } else if (cand[j] < dst[i]) {
      added.push_back_unchecked(cand[j]);
      merged.push_back_unchecked(cand[j++]);
    } else {
      merged.push_back_unchecked(dst[i]);
      i++;
      j++;
    }
  }
  while (i < dst.size())
    merged.push_back_unchecked(dst[i++]);
  while (j < cand.size()) {
    added.push_back_unchecked(cand[j]);
    merged.push_back_unchecked(cand[j++]);
  }
  dst.swap(merged);
  return added;
}

// Fills graph.tagEdgeDominated. Never fails; on malformed input it simply
// prunes less.
void computeTagPruning(CraftingGraph &graph) noexcept {
  graph.tagEdgeDominated.assign(graph.nRecipe, 0);

  const uint nItem = graph.nItem;
  const uint nReal = graph.nReal;
  const uint nRecipe = graph.nRecipe;
  if (nItem == 0 || nReal == 0 || nRecipe == 0)
    return;

  // ---- Simple tags and their members ------------------------------------
  aw::vector<uint8_t> simpleTag(nItem, 0);
  aw::vector<aw::vector<NodeId>> members(nItem);
  for (NodeId t = nReal; t < nItem; t++) {
    const auto recipes = graph.i2r.targetsOf(t);
    if (recipes.empty())
      continue;

    aw::vector<NodeId> ms;
    ms.reserve(recipes.size());
    bool simple = true;
    for (NodeId recipeNode : recipes) {
      const uint r = recipeNode - nItem;
      const auto inputs = graph.r2i.targetsOf(r);
      if (!graph.workstations.targetsOf(r).empty() || inputs.size() != 1 ||
          inputs[0] >= nReal) {
        simple = false;
        break;
      }
      ms.push_back_unchecked(inputs[0]);
    }
    if (!simple)
      continue;

    std::sort(ms.begin(), ms.end());
    ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
    simpleTag[t] = 1;
    members[t] = std::move(ms);
  }

  aw::vector<uint8_t> producible(nItem, 0);
  for (NodeId m = 0; m < nItem; m++)
    if (!graph.i2r.targetsOf(m).empty())
      producible[m] = 1;

  // ---- Budget the tag universe ------------------------------------------
  // The support pairs below are the cross product of a tag with itself and the
  // SCC stage scans the same square, so both are quadratic in the member count.
  // Admit tags smallest first, up to the budget; one that does not fit is
  // dropped from the relation. Dropping a tag only removes justifications, so
  // `valid` fires less often and fewer tag edges are ever marked dominated. It
  // weakens the pass but cannot make it unsound.
  {
    aw::vector<uint32_t> order;
    order.reserve(nItem);
    for (NodeId t = nReal; t < nItem; t++)
      if (simpleTag[t])
        order.push_back_unchecked(t);
    std::sort(order.begin(), order.end(), [&members](uint32_t a, uint32_t b) noexcept {
      if (members[a].size() != members[b].size())
        return members[a].size() < members[b].size();
      return a < b;
    });

    uint64_t used = 0;
    for (uint32_t t : order) {
      const uint64_t k = members[t].size();
      const uint64_t cost = k * k;
      if (k > options.maxTagMembers || cost > options.maxTagPairs - used) {
        simpleTag[t] = 0;
        continue;
      }
      used += cost;
    }
  }

  // member -> tags containing it, ascending.
  aw::vector<uint> itemTagOffsets(nReal, 0);
  for (NodeId t = nReal; t < nItem; t++)
    if (simpleTag[t])
      for (NodeId m : members[t])
        itemTagOffsets[m]++;
  prefixSum(itemTagOffsets);
  aw::vector<NodeId> itemTagTargets(itemTagOffsets.back());
  {
    aw::vector<uint> cursor(itemTagOffsets.begin(), itemTagOffsets.end() - 1);
    for (NodeId t = nReal; t < nItem; t++)
      if (simpleTag[t])
        for (NodeId m : members[t])
          itemTagTargets[cursor[m]++] = t;
  }

  // tag -> real items that consume it in a real recipe. Duplicates are kept;
  // the worklist dedups them.
  aw::vector<uint> tagConsumerOffsets(nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.output[r] >= nReal)
      continue;
    for (NodeId j : graph.r2i.targetsOf(r))
      if (j >= nReal && j < nItem && simpleTag[j])
        tagConsumerOffsets[j]++;
  }
  prefixSum(tagConsumerOffsets);
  aw::vector<NodeId> tagConsumerTargets(tagConsumerOffsets.back());
  {
    aw::vector<uint> cursor(tagConsumerOffsets.begin(), tagConsumerOffsets.end() - 1);
    for (uint r = 0; r < nRecipe; r++) {
      if (graph.output[r] >= nReal)
        continue;
      const NodeId m = graph.output[r];
      for (NodeId j : graph.r2i.targetsOf(r))
        if (j >= nReal && j < nItem && simpleTag[j])
          tagConsumerTargets[cursor[j]++] = m;
    }
  }

  const bool nonoptimal = options.nonoptimal;

  // ---- Amount-qualified inputs ------------------------------------------
  // Per real item: the real items and simple tags that some recipe of it
  // consumes in an amount at least equal to that recipe's output. Those are the
  // inputs a single execution can rely on; `validItem` repeats the same scan
  // recipe by recipe, and the closure below expands the whole relation.
  aw::vector<aw::vector<NodeId>> qualReal(nReal), qualTags(nReal);
  for (NodeId m = 0; m < nReal; m++) {
    for (NodeId recipeNode : graph.i2r.targetsOf(m)) {
      const uint r = recipeNode - nItem;
      const Amount out = graph.outputAmt[r];
      const auto inputs = graph.r2i.targetsOf(r);
      const auto weights = graph.r2i.weightsOf(r);
      for (size_t k = 0; k < inputs.size(); k++) {
        if (weights[k] < out)
          continue;
        const NodeId j = inputs[k];
        if (j < nReal) {
          // A raw input is gathered, not crafted, so it never requires w.
          if (producible[j])
            qualReal[m].push_back(j);
        } else if (j < nItem && simpleTag[j]) {
          qualTags[m].push_back(j);
        }
      }
    }
    std::sort(qualReal[m].begin(), qualReal[m].end());
    qualReal[m].erase(std::unique(qualReal[m].begin(), qualReal[m].end()),
                      qualReal[m].end());
    std::sort(qualTags[m].begin(), qualTags[m].end());
    qualTags[m].erase(std::unique(qualTags[m].begin(), qualTags[m].end()),
                      qualTags[m].end());
  }

  // ---- The universe of candidate pairs ----------------------------------
  // A pair (x, w) reads "x is dominated by w". x is a real item or a simple
  // tag, w is always a real item. For an item pair `alive(x, w)` is the old
  // one-step relation; for a tag pair it is the conjunction over the members,
  // which is the general form of the tag rule: every member of the consumed
  // tag has to require w, not merely contain it.
  //
  // (a) witness pairs: w is an amount-qualified input source of every recipe of
  //     m, with tag inputs expanded to their members. These seed the relation.
  // (b) support pairs (z, w) for members of a common tag.
  // The closure then alternates: an item pair contributes a tag pair for each
  // of its amount-qualified tag inputs, a tag pair contributes an item pair for
  // each of its producible members, and an item pair contributes an item pair
  // for each of its amount-qualified real inputs. The last two steps make the
  // relation transitive; they only run in nonoptimal mode, and the exact pass
  // only builds the tag pairs whose w really is a member.
  aw::vector<PairKey> itemPairs;
  {
    aw::vector<NodeId> acc, next, scratch;
    uint64_t witnessPairs = 0;
    for (NodeId m = 0; m < nReal && witnessPairs < options.maxWitnessPairs; m++) {
      const auto recipes = graph.i2r.targetsOf(m);
      if (recipes.empty())
        continue;

      bool first = true;
      acc.clear();
      for (NodeId recipeNode : recipes) {
        const uint r = recipeNode - nItem;
        const Amount out = graph.outputAmt[r];
        scratch.clear();
        const auto inputs = graph.r2i.targetsOf(r);
        const auto weights = graph.r2i.weightsOf(r);
        for (size_t k = 0; k < inputs.size(); k++) {
          if (weights[k] < out)
            continue;
          const NodeId j = inputs[k];
          if (j < nReal) {
            scratch.push_back(j);
          } else if (j < nItem && simpleTag[j]) {
            scratch.insert(scratch.end(), members[j].begin(), members[j].end());
          }
        }
        std::sort(scratch.begin(), scratch.end());
        scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
        if (first) {
          acc.swap(scratch);
          first = false;
        } else {
          intersect(acc, scratch, next);
          acc.swap(next);
        }
        if (acc.empty())
          break;
      }
      for (NodeId w : acc) {
        if (w == m)
          continue;
        itemPairs.push_back(packPair(m, w));
        if (++witnessPairs >= options.maxWitnessPairs)
          break;
      }
    }
  }
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const aw::vector<NodeId> &ms = members[t];
    for (NodeId z : ms) {
      if (!producible[z])
        continue;
      for (NodeId w : ms)
        if (z != w)
          itemPairs.push_back(packPair(z, w));
    }
  }

  std::sort(itemPairs.begin(), itemPairs.end());
  itemPairs.erase(std::unique(itemPairs.begin(), itemPairs.end()), itemPairs.end());

  // ---- Tag pairs ---------------------------------------------------------
  // An item pair contributes a tag pair (j, w) for every amount-qualified tag
  // input j of some recipe of m. In exact mode only the tags that literally
  // contain w are kept, which is the old one-step gate; the tag-pair lookup in
  // validItem then enforces the member restriction for free.
  aw::vector<PairKey> tagPairs;
  {
    aw::vector<PairKey> candTags;
    for (PairKey key : itemPairs) {
      const NodeId m = (NodeId) (key >> 32);
      const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
      for (NodeId j : qualTags[m]) {
        if (!nonoptimal &&
            !std::binary_search(members[j].begin(), members[j].end(), w))
          continue;
        candTags.push_back(packPair(j, w));
      }
    }
    mergeNewPairs(tagPairs, candTags);
  }

  // ---- Transitive closure -----------------------------------------------
  // Only the frontier is expanded, so every pair is processed once and the
  // closure costs one pass over the pairs it reaches. options.maxPrunePairs caps what
  // the closure adds; running out leaves pairs out of the relation, never in
  // it. The tag pairs above are deliberately outside the budget: the exact mode
  // gates through them, so they must survive however large the seeds are.
  if (nonoptimal) {
    uint64_t total = itemPairs.size() + tagPairs.size();
    aw::vector<PairKey> frontierItems = itemPairs;
    aw::vector<PairKey> frontierTags = tagPairs;
    while ((!frontierItems.empty() || !frontierTags.empty()) &&
           total < options.maxPrunePairs) {
      aw::vector<PairKey> candTags;
      for (PairKey key : frontierItems) {
        const NodeId m = (NodeId) (key >> 32);
        const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
        for (NodeId j : qualTags[m]) {
          if (total + candTags.size() >= options.maxPrunePairs)
            break;
          candTags.push_back(packPair(j, w));
        }
      }
      const aw::vector<PairKey> newTags = mergeNewPairs(tagPairs, candTags);
      total += newTags.size();

      aw::vector<PairKey> candItems;
      // A tag has to dominate w through all of its producible members.
      for (PairKey key : frontierTags) {
        const NodeId j = (NodeId) (key >> 32);
        const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
        for (NodeId z : members[j]) {
          if (z == w || !producible[z])
            continue;
          if (total + candItems.size() >= options.maxPrunePairs)
            break;
          candItems.push_back(packPair(z, w));
        }
      }
      // A real item m is dominated by w when its input j is; a raw input is
      // gathered rather than crafted, so it never requires w and is skipped by
      // the `producible` precomputation above.
      for (PairKey key : frontierItems) {
        const NodeId m = (NodeId) (key >> 32);
        const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
        for (NodeId j : qualReal[m]) {
          if (j == w)
            continue;
          if (total + candItems.size() >= options.maxPrunePairs)
            break;
          candItems.push_back(packPair(j, w));
        }
      }
      const aw::vector<PairKey> newItems = mergeNewPairs(itemPairs, candItems);
      total += newItems.size();
      frontierItems = newItems;
      frontierTags = newTags;
    }
  }

  // ---- Final universe ---------------------------------------------------
  aw::vector<PairKey> pairs;
  pairs.reserve(itemPairs.size() + tagPairs.size());
  pairs.insert(pairs.end(), itemPairs.begin(), itemPairs.end());
  pairs.insert(pairs.end(), tagPairs.begin(), tagPairs.end());
  itemPairs.clear();
  itemPairs.shrink_to_fit();
  tagPairs.clear();
  tagPairs.shrink_to_fit();

  const uint32_t universe = (uint32_t) pairs.size();
  if (universe == 0)
    return;

  // Group the universe by witness. Sorting by (x, w) means a single scan fills
  // each witness row in ascending x, so idOf can binary search the row.
  aw::vector<uint> byWOffsets(nItem, 0);
  for (PairKey key : pairs)
    byWOffsets[(uint32_t) (key & 0xFFFFFFFFu)]++;
  prefixSum(byWOffsets);
  aw::vector<NodeId> byWTargets(universe);
  aw::vector<NodeId> pairW(universe);
  {
    aw::vector<uint> cursor(byWOffsets.begin(), byWOffsets.end() - 1);
    for (PairKey key : pairs) {
      const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
      const NodeId x = (NodeId) (key >> 32);
      const uint slot = cursor[w]++;
      byWTargets[slot] = x;
      pairW[slot] = w;
    }
  }
  pairs.clear();
  pairs.shrink_to_fit();

  auto idOf = [&](NodeId x, NodeId w) -> uint32_t {
    const uint lo = byWOffsets[w], hi = byWOffsets[w + 1];
    const auto begin = byWTargets.begin() + lo;
    const auto end = byWTargets.begin() + hi;
    const auto it = std::lower_bound(begin, end, x);
    if (it == end || *it != x)
      return UINT32_MAX;
    return (uint32_t) (it - byWTargets.begin());
  };

  // Declared before the validation lambdas, which read it; filled in below.
  aw::vector<uint8_t> alive(universe, 1);

  // ---- Batching guard ----------------------------------------------------
  // Dropping `T <- m` is only safe when the plan can give up the m it consumes.
  // Every justification below is a per-execution swap, which stops being
  // equivalent once a producer of m emits several units at a time: a plan that
  // runs `m x8 <- w x8` to satisfy some other consumer of m gets 7 units of m
  // for free, and it spends one of them on `T <- m`. Deleting that edge then
  // has to make the 7 units (and the one spent) up with a real w craft while
  // the batch keeps running, so the optimum rises even though every recipe of
  // m does consume w at an equal rate.
  //
  // Real witness in the ATM10 graph: `enderio:fused_quartz_d_black x3` costs 26
  // real steps with tag pruning off and 27 with it, because
  // `#12695 <- fused_quartz_d_black` is dropped while the batch
  // `fused_quartz_d_black x8 <- black_dye + #12695 x8` still runs for the
  // target's three units. `minecraft:stick x7` (3 vs 4) is the same effect on
  // the column-cover branch, through `#12650 <- demonic_wooden_stairs` and
  // `demonic_wooden_stairs x4 <- demonic_planks x6`.
  //
  // Requiring every producer of m to emit exactly one unit restores the
  // per-execution argument: freeing k units of m demand then removes exactly k
  // producer executions, and each of them consumed at least one unit of the
  // dominator (or was covered by a single dominator execution), which pays for
  // the k new `T <- w` edges. A member that does not qualify is simply kept, so
  // the guard can only ever prune less.
  //
  // Nonoptimal mode skips the guard, so a batched member can be marked
  // dominated: dropping its tag edge then relies on "m is a better co-member
  // than w" even though a plan may have to keep a batch running. Skipping the
  // guard also means the scan below is not needed.
  aw::vector<uint8_t> unitOutput;
  if (!nonoptimal) {
    unitOutput.assign(nItem, 1);
    for (NodeId m = 0; m < nReal; m++)
      for (NodeId recipeNode : graph.i2r.targetsOf(m))
        if (graph.outputAmt[recipeNode - nItem] != 1) {
          unitOutput[m] = 0;
          break;
        }
  }

  // ---- Column cover ------------------------------------------------------
  // Besides consuming a dominator (directly or through a tag), a recipe of m
  // can also be replaced by a recipe of the candidate dominator: it must yield
  // at least as much and consume no more of every item, with a simple tag it
  // consumes allowed to pick any member. Swapping the one execution that fed
  // the dropped tag edge is free; the batching guard above is what keeps that
  // from being read as "m can be dropped wholesale". It catches members whose
  // concrete recipes have a dominator-free route (e.g.
  // `_d <- amethyst + quartz_block`) but still consume at least what some
  // dominator route does. The check is budgeted, because the fixpoint may ask
  // for the same pair repeatedly.
  uint64_t coverBudget = options.maxTagCoverWork;
  aw::vector<std::pair<NodeId, Amount>> cap;
  auto covers = [&](uint s, uint r) -> bool {
    if (coverBudget == 0)
      return false;
    coverBudget--;
    if (graph.outputAmt[s] < graph.outputAmt[r])
      return false;

    cap.clear();
    const auto ri = graph.r2i.targetsOf(r);
    const auto rw = graph.r2i.weightsOf(r);
    for (size_t j = 0; j < ri.size(); j++)
      cap.emplace_back(ri[j], rw[j]);
    std::sort(cap.begin(), cap.end(),
              [](const auto &a, const auto &b) noexcept { return a.first < b.first; });

    const auto si = graph.r2i.targetsOf(s);
    const auto sw = graph.r2i.weightsOf(s);
    for (size_t j = 0; j < si.size(); j++) {
      const NodeId h = si[j];
      Amount need = sw[j];

      // An exact row first, so a tag consumed atomically matches itself.
      auto it = std::lower_bound(
          cap.begin(), cap.end(), h,
          [](const auto &p, NodeId v) noexcept { return p.first < v; });
      if (it != cap.end() && it->first == h) {
        const Amount take = std::min(need, it->second);
        it->second -= take;
        need -= take;
      }
      if (need > 0 && h >= nReal && h < nItem && simpleTag[h]) {
        for (NodeId z : members[h]) {
          if (need <= 0)
            break;
          auto jt = std::lower_bound(
              cap.begin(), cap.end(), z,
              [](const auto &p, NodeId v) noexcept { return p.first < v; });
          if (jt != cap.end() && jt->first == z) {
            const Amount take = std::min(need, jt->second);
            jt->second -= take;
            need -= take;
          }
        }
      }
      if (need > 0)
        return false;
    }
    return true;
  };
  auto colCover = [&](uint r, NodeId w) -> bool {
    for (NodeId sNode : graph.i2r.targetsOf(w))
      if (covers(sNode - nItem, r))
        return true;
    return false;
  };

  // A tag pair (t, w) holds when every producible member of t is dominated by
  // w. A member with no recipe is gathered directly, so it never requires w and
  // it blocks the gate: it has no pair, so the lookup fails.
  auto validTag = [&](NodeId t, NodeId w) -> bool {
    for (NodeId z : members[t]) {
      if (z == w)
        continue;
      const uint32_t q = idOf(z, w);
      if (q == UINT32_MAX || !alive[q])
        return false;
    }
    return true;
  };

  // A real item m is dominated by w when every recipe of m has an
  // amount-qualified input that is w itself, an item pair in turn dominated by
  // w, or a tag pair dominated by w. The column-cover rule is the same escape
  // hatch as before.
  auto validItem = [&](NodeId m, NodeId w) -> bool {
    for (NodeId recipeNode : graph.i2r.targetsOf(m)) {
      const uint r = recipeNode - nItem;
      const Amount out = graph.outputAmt[r];
      const auto inputs = graph.r2i.targetsOf(r);
      const auto weights = graph.r2i.weightsOf(r);
      bool ok = false;
      for (size_t k = 0; k < inputs.size(); k++) {
        if (weights[k] < out)
          continue;
        const NodeId j = inputs[k];
        if (j == w) {
          ok = true;
          break;
        }
        if (j < nReal) {
          // Chaining real inputs is the transitive closure; exact mode only
          // accepts the direct w above. A raw input has no pair, so it fails
          // the lookup below, which is what we want.
          if (!nonoptimal)
            continue;
        } else if (j >= nItem || !simpleTag[j]) {
          continue;
        }
        const uint32_t q = idOf(j, w);
        if (q != UINT32_MAX && alive[q]) {
          ok = true;
          break;
        }
      }
      if (!ok && !colCover(r, w))
        return false;
    }
    return true;
  };

  // ---- Greatest fixpoint ------------------------------------------------
  // Every pair starts alive and is killed once when it loses its justification.
  // Killing an item pair can invalidate the tag pairs that contain it as a
  // member, and killing a tag pair invalidates the recipes that gated through
  // it; both are re-queued, which yields the greatest fixpoint.
  aw::vector<uint32_t> queue(universe);
  aw::vector<uint8_t> queued(universe, 0);
  size_t head = 0, tail = 0, pending = 0;
  auto push = [&](uint32_t id) noexcept {
    if (queued[id])
      return;
    queued[id] = 1;
    queue[tail] = id;
    tail++;
    if (tail == universe)
      tail = 0;
    pending++;
  };
  for (uint32_t id = 0; id < universe; id++)
    push(id);

  while (pending != 0) {
    const uint32_t id = queue[head];
    head++;
    if (head == universe)
      head = 0;
    pending--;
    queued[id] = 0;
    if (!alive[id])
      continue;

    const NodeId x = byWTargets[id];
    const NodeId w = pairW[id];
    // A first component at or above nReal is a simple tag: its pair is the AND
    // over its members, not a recipe scan.
    const bool tagPair = x >= nReal;
    if (tagPair ? validTag(x, w) : validItem(x, w))
      continue;

    alive[id] = 0;
    if (tagPair) {
      // The tag lost a member, so every real recipe that consumed it may no
      // longer gate through it.
      for (uint e = tagConsumerOffsets[x]; e < tagConsumerOffsets[x + 1]; e++) {
        const uint32_t q = idOf(tagConsumerTargets[e], w);
        if (q != UINT32_MAX)
          push(q);
      }
    } else {
      // The item lost, so every tag that contains it lost a supporting member.
      for (uint e = itemTagOffsets[x]; e < itemTagOffsets[x + 1]; e++) {
        const uint32_t q = idOf(itemTagTargets[e], w);
        if (q != UINT32_MAX)
          push(q);
      }
    }
  }

  // ---- Keep one representative per sink SCC -----------------------------
  // On the alive edges inside a tag, every member reaches some sink SCC, and a
  // sink representative is substitutable for everything that reaches it. This
  // also keeps at least one member of every tag, including mutual-requirement
  // cycles, so no tag becomes unsatisfiable.
  aw::vector<uint32_t> adjOffsets, adjTargets, cursor;
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const aw::vector<NodeId> &ms = members[t];
    const size_t k = ms.size();
    if (k < 2)
      continue;

    adjOffsets.assign(k + 1, 0);
    for (size_t a = 0; a < k; a++) {
      uint32_t degree = 0;
      for (size_t b = 0; b < k; b++) {
        if (a == b)
          continue;
        const uint32_t id = idOf(ms[a], ms[b]);
        if (id != UINT32_MAX && alive[id])
          degree++;
      }
      adjOffsets[a + 1] = degree;
    }
    for (size_t a = 0; a + 1 <= k; a++)
      adjOffsets[a + 1] += adjOffsets[a];
    adjTargets.assign(adjOffsets[k], 0);
    cursor.assign(adjOffsets.begin(), adjOffsets.end() - 1);
    for (size_t a = 0; a < k; a++)
      for (size_t b = 0; b < k; b++) {
        if (a == b)
          continue;
        const uint32_t id = idOf(ms[a], ms[b]);
        if (id != UINT32_MAX && alive[id])
          adjTargets[cursor[a]++] = (uint32_t) b;
      }

    markSinkRepresentatives((uint) k, adjOffsets, adjTargets, comp, repOfComp, keep);

    for (uint32_t a = 0; a < k; a++) {
      if (keep[a])
        continue;
      const NodeId m = ms[a];
      // A batched member keeps its tag edge: the batch surplus would otherwise
      // be a free way to satisfy T, and dropping the edge costs real steps.
      // Nonoptimal mode waives the guard, see above.
      if (!nonoptimal && !unitOutput[m])
        continue;
      for (NodeId recipeNode : graph.i2r.targetsOf(t)) {
        const uint r = recipeNode - nItem;
        const auto inputs = graph.r2i.targetsOf(r);
        if (inputs.size() == 1 && inputs[0] == m)
          graph.tagEdgeDominated[r] = 1;
      }
    }
  }
}

}
