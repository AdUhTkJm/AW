// Tag-edge dominance pruning.
//
// A tag is a pseudo-item with one synthetic recipe `T <- m` per member m. When
// another member w cost-dominates m -- every way of crafting m either consumes
// at least as much w, or is replaceable by a recipe of w -- the edge `T <- m`
// is never part of an optimal plan, so it is marked dominated here and the
// query-time subgraph drops it. See docs/algorithm.typ, "基于支配的剪枝".
//
// The pass computes the greatest fixpoint of a candidate relation over pairs
// (x, w), read "x is dominated by w", where x is a real item or a simple tag
// and w is always a real item. `TagPruner` holds the pass state; each stage is
// one member function, called by run() top to bottom.

#include "Prune.h"
#include "aw/plan/Options.h"

namespace aw::detail {

using PairKey = uint64_t;
using uint = uint32_t;

namespace {

constexpr PairKey packPair(NodeId m, NodeId w) noexcept {
  return ((PairKey) m << 32) | (PairKey) w;
}

NodeId pairFirst(PairKey key) noexcept {
  return (NodeId) (key >> 32);
}

NodeId pairSecond(PairKey key) noexcept {
  return (NodeId) (key & 0xFFFFFFFFu);
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

// State of one tag-pruning pass over the crafting graph. run() calls the
// stages in this order:
//
//   findSimpleTags          which pseudo-items are simple tags, and their members
//   budgetTagUniverse       admit tags into the relation within the pair budget
//   buildMemberIndex        member -> tags containing it (CSR)
//   buildConsumerIndex      tag -> real items consuming it (CSR)
//   computeQualifiedInputs  per-item inputs consumed in at least the output amount
//   seedItemPairs           witness and support pairs
//   seedTagPairs            tag pairs contributed by the item pairs
//   transitiveClosure       nonoptimal mode only
//   indexUniverse           group the pairs by witness for binary search
//   computeUnitOutputs      batching guard, exact mode only
//   greatestFixpoint        kill unjustified pairs until convergence
//   pruneDominatedEdges     keep one sink-SCC representative per tag
//
// Every budget overflow only drops candidates or justifications, so a stage
// that runs out of budget weakens the pass but cannot make it unsound.
struct TagPruner {
  explicit TagPruner(CraftingGraph &graph) noexcept
      : graph(graph),
        nItem(graph.nItem),
        nReal(graph.nReal),
        nRecipe(graph.nRecipe),
        nonoptimal(options.nonoptimal) {}

  void run() noexcept;

  CraftingGraph &graph;
  const uint nItem;
  const uint nReal;
  const uint nRecipe;
  const bool nonoptimal;

  // A simple tag is a pseudo-item whose recipes are all workstation-free
  // `T <- m` with a single real member m; only those participate in the pass.
  // members[t] is sorted and unique.
  aw::vector<uint8_t> simpleTag;           // nItem
  aw::vector<aw::vector<NodeId>> members;  // nItem
  // producible[m]: m has at least one recipe (it is crafted, not gathered).
  aw::vector<uint8_t> producible;          // nItem

  // member -> tags containing it, ascending.
  aw::vector<uint> itemTagOffsets;         // nReal + 1 after prefixSum
  aw::vector<NodeId> itemTagTargets;
  // tag -> real items that consume it in a real recipe. Duplicates are kept;
  // the worklist dedups them.
  aw::vector<uint> tagConsumerOffsets;     // nItem + 1 after prefixSum
  aw::vector<NodeId> tagConsumerTargets;

  // Amount-qualified inputs, see computeQualifiedInputs().
  aw::vector<aw::vector<NodeId>> qualReal, qualTags;  // nReal each

  // The candidate relation, see seedItemPairs().
  aw::vector<PairKey> itemPairs;
  aw::vector<PairKey> tagPairs;

  // The final universe grouped by witness: pairs with witness w occupy
  // [byWOffsets[w], byWOffsets[w + 1]) of byWTargets (first components, in
  // ascending order). A pair's global id is its byWTargets index. Fixpoint
  // propagation never changes the witness, so the pass runs one witness row at
  // a time; rowIdOf answers (x, w) with a row-local hash of x, while idOf keeps
  // the binary search for the cross-witness SCC stage.
  aw::vector<uint> byWOffsets;             // nItem + 1 after prefixSum
  aw::vector<NodeId> byWTargets;           // universe entries
  uint32_t universe = 0;
  aw::vector<uint8_t> alive;               // universe entries

  // Scratch for the witness row currently being fixed: an open-addressed
  // x -> global id + 1 map (0 is empty), sized to at least twice the row.
  aw::vector<uint32_t> rowSlots;
  uint rowMask = 0;
  uint rowShift = 0;

  // Batching guard, see computeUnitOutputs(). Empty in nonoptimal mode.
  aw::vector<uint8_t> unitOutput;          // nItem

  // Column-cover work budget and scratch, see covers().
  uint64_t coverBudget = 0;
  aw::vector<std::pair<NodeId, Amount>> cap;

  void findSimpleTags() noexcept;
  void budgetTagUniverse() noexcept;
  void buildMemberIndex() noexcept;
  void buildConsumerIndex() noexcept;
  void computeQualifiedInputs() noexcept;
  void seedItemPairs() noexcept;
  void seedTagPairs() noexcept;
  void transitiveClosure() noexcept;
  // False when the universe is empty and there is nothing to do.
  bool indexUniverse() noexcept;
  // The pair id of (x, w) in the universe, or UINT32_MAX when absent.
  uint32_t idOf(NodeId x, NodeId w) const noexcept;
  // Prepares the row-local lookup for witness w.
  void beginRow(uint w) noexcept;
  // The global id of (x, w) for the row prepared by beginRow(w), or
  // UINT32_MAX. Only valid until the next beginRow().
  uint32_t rowIdOf(NodeId x) const noexcept;
  void computeUnitOutputs() noexcept;
  // True when one execution of recipe s can replace one of r, see the
  // column-cover comment above covers().
  bool covers(uint s, uint r) noexcept;
  bool colCover(uint r, NodeId w) noexcept;
  bool validTag(NodeId t, NodeId w) noexcept;
  bool validItem(NodeId m, NodeId w) noexcept;
  void greatestFixpoint() noexcept;
  void pruneDominatedEdges() noexcept;
};

void TagPruner::run() noexcept {
  graph.tagEdgeDominated.assign(nRecipe, 0);
  if (nItem == 0 || nReal == 0 || nRecipe == 0)
    return;

  findSimpleTags();
  budgetTagUniverse();
  buildMemberIndex();
  buildConsumerIndex();
  computeQualifiedInputs();
  seedItemPairs();
  seedTagPairs();
  transitiveClosure();
  if (!indexUniverse())
    return;
  computeUnitOutputs();
  coverBudget = options.maxTagCoverWork;
  greatestFixpoint();
  pruneDominatedEdges();
}

// ---- Simple tags and their members ------------------------------------
void TagPruner::findSimpleTags() noexcept {
  simpleTag.assign(nItem, 0);
  members.resize(nItem);
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

  producible.assign(nItem, 0);
  for (NodeId m = 0; m < nItem; m++)
    if (!graph.i2r.targetsOf(m).empty())
      producible[m] = 1;
}

// ---- Budget the tag universe ------------------------------------------
// The support pairs below are the cross product of a tag with itself and the
// SCC stage scans the same square, so both are quadratic in the member count.
// Admit tags smallest first, up to the budget; one that does not fit is
// dropped from the relation. Dropping a tag only removes justifications, so
// `valid` fires less often and fewer tag edges are ever marked dominated. It
// weakens the pass but cannot make it unsound.
void TagPruner::budgetTagUniverse() noexcept {
  aw::vector<uint32_t> order;
  order.reserve(nItem);
  for (NodeId t = nReal; t < nItem; t++)
    if (simpleTag[t])
      order.push_back_unchecked(t);
  std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) noexcept {
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

void TagPruner::buildMemberIndex() noexcept {
  itemTagOffsets.assign(nReal, 0);
  for (NodeId t = nReal; t < nItem; t++)
    if (simpleTag[t])
      for (NodeId m : members[t])
        itemTagOffsets[m]++;
  prefixSum(itemTagOffsets);
  itemTagTargets.resize(itemTagOffsets.back());
  aw::vector<uint> cursor(itemTagOffsets.begin(), itemTagOffsets.end() - 1);
  for (NodeId t = nReal; t < nItem; t++)
    if (simpleTag[t])
      for (NodeId m : members[t])
        itemTagTargets[cursor[m]++] = t;
}

void TagPruner::buildConsumerIndex() noexcept {
  tagConsumerOffsets.assign(nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.output[r] >= nReal)
      continue;
    for (NodeId j : graph.r2i.targetsOf(r))
      if (j >= nReal && j < nItem && simpleTag[j])
        tagConsumerOffsets[j]++;
  }
  prefixSum(tagConsumerOffsets);
  tagConsumerTargets.resize(tagConsumerOffsets.back());
  aw::vector<uint> cursor(tagConsumerOffsets.begin(),
                          tagConsumerOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.output[r] >= nReal)
      continue;
    const NodeId m = graph.output[r];
    for (NodeId j : graph.r2i.targetsOf(r))
      if (j >= nReal && j < nItem && simpleTag[j])
        tagConsumerTargets[cursor[j]++] = m;
  }
}

// ---- Amount-qualified inputs ------------------------------------------
// Per real item: the real items and simple tags that some recipe of it
// consumes in an amount at least equal to that recipe's output. Those are the
// inputs a single execution can rely on; `validItem` repeats the same scan
// recipe by recipe, and the closure below expands the whole relation.
void TagPruner::computeQualifiedInputs() noexcept {
  qualReal.resize(nReal);
  qualTags.resize(nReal);
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
void TagPruner::seedItemPairs() noexcept {
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
}

// ---- Tag pairs ---------------------------------------------------------
// An item pair contributes a tag pair (j, w) for every amount-qualified tag
// input j of some recipe of m. In exact mode only the tags that literally
// contain w are kept, which is the old one-step gate; the tag-pair lookup in
// validItem then enforces the member restriction for free.
void TagPruner::seedTagPairs() noexcept {
  aw::vector<PairKey> candTags;
  for (PairKey key : itemPairs) {
    const NodeId m = pairFirst(key);
    const NodeId w = pairSecond(key);
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
// closure costs one pass over the pairs it reaches. options.maxPrunePairs caps
// what the closure adds; running out leaves pairs out of the relation, never
// in it. The tag pairs above are deliberately outside the budget: the exact
// mode gates through them, so they must survive however large the seeds are.
void TagPruner::transitiveClosure() noexcept {
  if (!nonoptimal)
    return;
  uint64_t total = itemPairs.size() + tagPairs.size();
  aw::vector<PairKey> frontierItems = itemPairs;
  aw::vector<PairKey> frontierTags = tagPairs;
  while ((!frontierItems.empty() || !frontierTags.empty()) &&
         total < options.maxPrunePairs) {
    aw::vector<PairKey> candTags;
    for (PairKey key : frontierItems) {
      const NodeId m = pairFirst(key);
      const NodeId w = pairSecond(key);
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
      const NodeId j = pairFirst(key);
      const NodeId w = pairSecond(key);
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
      const NodeId m = pairFirst(key);
      const NodeId w = pairSecond(key);
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
bool TagPruner::indexUniverse() noexcept {
  aw::vector<PairKey> pairs;
  pairs.reserve(itemPairs.size() + tagPairs.size());
  pairs.insert(pairs.end(), itemPairs.begin(), itemPairs.end());
  pairs.insert(pairs.end(), tagPairs.begin(), tagPairs.end());
  itemPairs.clear();
  itemPairs.shrink_to_fit();
  tagPairs.clear();
  tagPairs.shrink_to_fit();

  universe = (uint32_t) pairs.size();
  if (universe == 0)
    return false;

  // Group the universe by witness. Sorting by (x, w) means a single scan fills
  // each witness row in ascending x, so idOf can binary search the row.
  byWOffsets.assign(nItem, 0);
  for (PairKey key : pairs)
    byWOffsets[pairSecond(key)]++;
  prefixSum(byWOffsets);
  byWTargets.resize(universe);
  {
    aw::vector<uint> cursor(byWOffsets.begin(), byWOffsets.end() - 1);
    for (PairKey key : pairs) {
      const NodeId w = pairSecond(key);
      const NodeId x = pairFirst(key);
      const uint slot = cursor[w]++;
      byWTargets[slot] = x;
    }
  }
  pairs.clear();
  pairs.shrink_to_fit();

  alive.assign(universe, 1);
  return true;
}

uint32_t TagPruner::idOf(NodeId x, NodeId w) const noexcept {
  const uint lo = byWOffsets[w], hi = byWOffsets[w + 1];
  const auto begin = byWTargets.begin() + lo;
  const auto end = byWTargets.begin() + hi;
  const auto it = std::lower_bound(begin, end, x);
  if (it == end || *it != x)
    return UINT32_MAX;
  return (uint32_t) (it - byWTargets.begin());
}

// Construct a hash table to query for id `w`.
void TagPruner::beginRow(uint w) noexcept {
  const uint lo = byWOffsets[w], hi = byWOffsets[w + 1];
  uint cap = 1, bits = 0;
  while (cap < 2 * (hi - lo)) {
    cap <<= 1;
    bits++;
  }
  rowSlots.assign(cap, 0);
  rowMask = cap - 1;
  rowShift = 32 - bits;
  for (uint id = lo; id < hi; id++) {
    uint s = (byWTargets[id] * 2654435761u) >> rowShift;
    while (rowSlots[s] != 0)
      s = (s + 1) & rowMask;
    rowSlots[s] = id + 1;
  }
}

uint32_t TagPruner::rowIdOf(NodeId x) const noexcept {
  uint s = (x * 2654435761u) >> rowShift;
  for (;;) {
    const uint v = rowSlots[s];
    if (v == 0)
      return UINT32_MAX;
    if (byWTargets[v - 1] == x)
      return v - 1;
    s = (s + 1) & rowMask;
  }
}

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
void TagPruner::computeUnitOutputs() noexcept {
  if (nonoptimal)
    return;
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
bool TagPruner::covers(uint s, uint r) noexcept {
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
}

bool TagPruner::colCover(uint r, NodeId w) noexcept {
  for (NodeId sNode : graph.i2r.targetsOf(w))
    if (covers(sNode - nItem, r))
      return true;
  return false;
}

// A tag pair (t, w) holds when every producible member of t is dominated by
// w. A member with no recipe is gathered directly, so it never requires w and
// it blocks the gate: it has no pair, so the lookup fails.
bool TagPruner::validTag(NodeId t, NodeId w) noexcept {
  for (NodeId z : members[t]) {
    if (z == w)
      continue;
    const uint32_t q = rowIdOf(z);
    if (q == UINT32_MAX || !alive[q])
      return false;
  }
  return true;
}

// A real item m is dominated by w when every recipe of m has an
// amount-qualified input that is w itself, an item pair in turn dominated by
// w, or a tag pair dominated by w. The column-cover rule is the same escape
// hatch as before.
bool TagPruner::validItem(NodeId m, NodeId w) noexcept {
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
      const uint32_t q = rowIdOf(j);
      if (q != UINT32_MAX && alive[q]) {
        ok = true;
        break;
      }
    }
    if (!ok && !colCover(r, w))
      return false;
  }
  return true;
}

// ---- Greatest fixpoint ------------------------------------------------
// Every pair starts alive and is killed once when it loses its justification.
// Killing an item pair can invalidate the tag pairs that contain it as a
// member, and killing a tag pair invalidates the recipes that gated through
// it; both are re-queued, which yields the greatest fixpoint.
//
// Every one of those steps keeps the witness fixed, so the rows of byWOffsets
// are independent and the pass fixes one row at a time. That lets rowIdOf use a
// row-local lookup and keeps the row in cache for the whole of its fixpoint.
void TagPruner::greatestFixpoint() noexcept {
  aw::vector<uint32_t> queue;
  aw::vector<uint8_t> queued;
  for (uint w = 0; w < nReal; w++) {
    const uint lo = byWOffsets[w], hi = byWOffsets[w + 1];
    const uint k = hi - lo;
    if (k == 0)
      continue;
    beginRow(w);
    queue.resize(k);
    queued.assign(k, 0);
    size_t head = 0, tail = 0, pending = 0;
    auto push = [&](uint local) noexcept {
      if (queued[local])
        return;
      queued[local] = 1;
      queue[tail] = local;
      tail++;
      if (tail == k)
        tail = 0;
      pending++;
    };
    for (uint local = 0; local < k; local++)
      push(local);

    while (pending != 0) {
      const uint local = queue[head];
      head++;
      if (head == k)
        head = 0;
      pending--;
      queued[local] = 0;
      const uint32_t id = lo + local;
      if (!alive[id])
        continue;

      const NodeId x = byWTargets[id];
      // A first component at or above nReal is a simple tag: its pair is the
      // AND over its members, not a recipe scan.
      const bool tagPair = x >= nReal;
      if (tagPair ? validTag(x, w) : validItem(x, w))
        continue;

      alive[id] = 0;
      if (tagPair) {
        // The tag lost a member, so every real recipe that consumed it may no
        // longer gate through it.
        for (uint e = tagConsumerOffsets[x]; e < tagConsumerOffsets[x + 1]; e++) {
          const uint32_t q = rowIdOf(tagConsumerTargets[e]);
          if (q != UINT32_MAX)
            push(q - lo);
        }
      } else {
        // The item lost, so every tag that contains it lost a supporting member.
        for (uint e = itemTagOffsets[x]; e < itemTagOffsets[x + 1]; e++) {
          const uint32_t q = rowIdOf(itemTagTargets[e]);
          if (q != UINT32_MAX)
            push(q - lo);
        }
      }
    }
  }
}

// ---- Keep one representative per sink SCC -----------------------------
// On the alive edges inside a tag, every member reaches some sink SCC, and a
// sink representative is substitutable for everything that reaches it. This
// also keeps at least one member of every tag, including mutual-requirement
// cycles, so no tag becomes unsatisfiable.
void TagPruner::pruneDominatedEdges() noexcept {
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

}  // namespace

// Fills graph.tagEdgeDominated. Never fails; on malformed input it simply
// prunes less.
void computeTagPruning(CraftingGraph &graph) noexcept {
  TagPruner pass(graph);
  pass.run();
}

}
