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
//
// Every propagation step keeps the witness w fixed: a pair (x, w) only ever
// justifies another pair (y, w). The pass therefore solves one witness row at a
// time -- seeding it, closing and fixing it, then discarding it -- instead of
// materialising the whole O(sum k^2) universe of support pairs. The one stage
// that reads across witnesses is the sink-SCC reduction of pruneDominatedEdges;
// it consumes a per-tag bit matrix of the alive co-member edges, which
// recordRowScc fills while each row is still in hand.

#include <algorithm>
#include <bit>

#include "Prune.h"
#include "aw/plan/Options.h"

namespace aw::detail {

using uint = uint32_t;

namespace {

void prefixSum(aw::vector<uint> &v) noexcept {
  v.insert(v.begin(), 0);
  for (uint i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
}

template<class T>
void dedup(aw::vector<T> &v) noexcept {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
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
// `dst`, keeping `dst` sorted, and returns the entries that were not already in
// it. The closure below inserts one frontier at a time, so appending a sorted
// batch beats probing a hash table and matches the sort/unique style of the
// rest of the pass. A row holds first components only -- its witness is the row
// index -- so T is always NodeId here.
template<class T>
aw::vector<T> mergeNewSorted(aw::vector<T> &dst,
                             aw::vector<T> &cand) noexcept {
  dedup<T>(cand);
  aw::vector<T> added;
  if (cand.empty())
    return added;
  // Every new entry comes from `cand`, so size the result up front.
  added.reserve(cand.size());

  aw::vector<T> merged;
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
//   computeUnitOutputs      batching guard, exact mode only
//   allocateTagAdjacency    per-tag alive-edge bit matrix for the SCC stage
//   seedWitnessPairs        bucket the witness seeds (m, w) by w
//   seedRow                 one witness row: support and tag seeds
//   rowClosure              nonoptimal mode only, one witness row
//   fixRow                  kill that row's unjustified pairs until convergence
//   recordRowScc            copy the row's alive co-member edges into the matrix
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

  // Everything after `nReal` is guaranteed a tag, but we might quit
  // if the tag is too large.
  aw::vector<uint8_t> allowedTags;         // nItem
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

  // Witness seeds grouped by witness: witnessByW[w] holds the m with a witness
  // pair (m, w). Seeding is per item but solving is per witness, so the pairs
  // are bucketed here instead of stored packed.
  aw::vector<aw::vector<NodeId>> witnessByW;  // nReal

  // The witness row currently being solved. Pairs carry their first component
  // only, because their witness is the row being processed. `rowItems` and
  // `rowTags` are sorted and unique, `rowX` is their sorted union and
  // `rowAlive` is parallel to `rowX`. The rows of one witness are independent,
  // so a row is thrown away as soon as it is fixed.
  aw::vector<NodeId> rowItems, rowTags, rowX;
  aw::vector<uint8_t> rowAlive;
  aw::vector<NodeId> candRow;

  // The alive co-member edges of every tag, packed as a bit matrix so that the
  // SCC stage never needs a discarded row: bit (a, b) of tag t is set when
  // alive(members[t][a], members[t][b]). tagAdjBits is [tagAdjOffsets[t],
  // tagAdjOffsets[t + 1]) for tag t, k * ceil(k / 64) words, row-major.
  aw::vector<uint> tagAdjOffsets;          // nItem + 1
  aw::vector<uint64_t> tagAdjBits;

  // Scratch for the witness row currently being fixed. rowSlots is an
  // open-addressed x -> row-local id + 1 map (0 is empty), sized to at least
  // twice the row; rowQueue / rowQueued are the fixpoint worklist.
  aw::vector<uint32_t> rowSlots;
  uint rowMask = 0;
  uint rowShift = 0;
  aw::vector<uint32_t> rowQueue;
  aw::vector<uint8_t> rowQueued;

  // Batching guard, see computeUnitOutputs(). Empty in nonoptimal mode.
  aw::vector<uint8_t> unitOutput;          // nItem

  // Column-cover work budget and scratch, see covers().
  uint64_t coverBudget = 0;
  aw::vector<std::pair<NodeId, Amount>> cap;

  // The closure's global pair budget, carried across witness rows.
  uint64_t prunePairs = 0;

  void findSimpleTags() noexcept;
  void budgetTagUniverse() noexcept;
  void buildMemberIndex() noexcept;
  void buildConsumerIndex() noexcept;
  void computeQualifiedInputs() noexcept;
  void computeUnitOutputs() noexcept;
  void allocateTagAdjacency() noexcept;
  void seedWitnessPairs() noexcept;
  // Builds the pair set for witness w; false when the row has no seeds.
  bool seedRow(NodeId w) noexcept;
  // Expands that row to its closure; nonoptimal mode only.
  void rowClosure(NodeId w) noexcept;
  // Builds the row-local lookup for the seeds of seedRow().
  void beginRow() noexcept;
  // The row-local id of x, or UINT32_MAX when it is not in the row. Only valid
  // until the next beginRow().
  uint32_t rowIdOf(NodeId x) const noexcept;
  // Kills the row's unjustified pairs until convergence.
  void fixRow(NodeId w) noexcept;
  // Copies the row's alive co-member edges into tagAdjBits.
  void recordRowScc(NodeId w) noexcept;
  // True when one execution of recipe s can replace one of r, see the
  // column-cover comment above covers().
  bool covers(uint s, uint r) noexcept;
  bool colCover(uint r, NodeId w) noexcept;
  bool validTag(NodeId t, NodeId w) noexcept;
  bool validItem(NodeId m, NodeId w) noexcept;
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
  computeUnitOutputs();
  allocateTagAdjacency();
  seedWitnessPairs();

  coverBudget = options.maxTagCoverWork;
  for (NodeId w = 0; w < nReal; w++) {
    if (!seedRow(w))
      continue;
    fixRow(w);
    recordRowScc(w);
  }
  pruneDominatedEdges();
}

// ---- Simple tags and their members ------------------------------------
// A tag is a pseudo-item whose recipes are all workstation-free `T <- m` with a
// single real member m. That is a contract of the registered data, so the
// recipe scan below takes inputs[0] on trust; members[t] comes out sorted and
// unique.
void TagPruner::findSimpleTags() noexcept {
  allowedTags.assign(nItem, 0);
  members.resize(nItem);
  for (NodeId t = nReal; t < nItem; t++) {
    const auto recipes = graph.i2r.targetsOf(t);
    if (recipes.empty())
      continue;

    aw::vector<NodeId> ms;
    ms.reserve(recipes.size());
    for (NodeId recipeNode : recipes) {
      const uint r = recipeNode - nItem;
      const auto inputs = graph.r2i.targetsOf(r);
      ms.push_back_unchecked(inputs[0]);
    }

    dedup<NodeId>(ms);
    allowedTags[t] = 1;
    members[t] = std::move(ms);
  }

  producible.assign(nItem, 0);
  for (NodeId m = 0; m < nItem; m++)
    if (!graph.i2r.targetsOf(m).empty())
      producible[m] = 1;
}

// ---- Budget the tag universe ------------------------------------------
// The support seeds below are the cross product of a tag with itself and the
// SCC stage scans the same square, so both are quadratic in the member count.
// Admit tags smallest first, up to the budget; one that does not fit is
// dropped from the relation. Dropping a tag only removes justifications, so
// `valid` fires less often and fewer tag edges are ever marked dominated. It
// weakens the pass but cannot make it unsound.
void TagPruner::budgetTagUniverse() noexcept {
  aw::vector<uint32_t> order;
  order.reserve(nItem);
  for (NodeId t = nReal; t < nItem; t++)
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
      allowedTags[t] = 0;
      continue;
    }
    used += cost;
  }
}

void TagPruner::buildMemberIndex() noexcept {
  itemTagOffsets.assign(nReal, 0);
  for (NodeId t = nReal; t < nItem; t++) {
    if (allowedTags[t])
      for (NodeId m : members[t])
        itemTagOffsets[m]++;
  }
  prefixSum(itemTagOffsets);
  itemTagTargets.resize(itemTagOffsets.back());
  aw::vector<uint> cursor(itemTagOffsets.begin(), itemTagOffsets.end() - 1);
  for (NodeId t = nReal; t < nItem; t++) {
    if (allowedTags[t])
      for (NodeId m : members[t])
        itemTagTargets[cursor[m]++] = t;
  }
}

void TagPruner::buildConsumerIndex() noexcept {
  tagConsumerOffsets.assign(nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.output[r] >= nReal)
      continue;
    
    for (NodeId j : graph.r2i.targetsOf(r)) {
      if (j >= nReal && j < nItem && allowedTags[j])
        tagConsumerOffsets[j]++;
    }
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
      if (j >= nReal && j < nItem && allowedTags[j])
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
        } else if (j < nItem && allowedTags[j]) {
          qualTags[m].push_back(j);
        }
      }
    }
    dedup<NodeId>(qualReal[m]);
    dedup<NodeId>(qualTags[m]);
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

// ---- The universe of candidate pairs ----------------------------------
// A pair (x, w) reads "x is dominated by w". x is a real item or a simple
// tag, w is always a real item. For an item pair `alive(x, w)` is the old
// one-step relation; for a tag pair it is the conjunction over the members,
// which is the general form of the tag rule: every member of the consumed
// tag has to require w, not merely contain it.
//
// A row w is seeded with:
// (a) witness pairs: w is an amount-qualified input source of every recipe of
//     m, with tag inputs expanded to their members. These are collected per
//     item by seedWitnessPairs() and bucketed by w.
// (b) support pairs (z, w) for producible members z of a common tag: every tag
//     containing w contributes its other members. That is exactly the list of
//     tagsOf(w), which seedRow() walks directly. This is the O(sum k^2) set
//     that used to be materialised; it is now regenerated per row.
// The closure then alternates: an item pair contributes a tag pair for each
// of its amount-qualified tag inputs, a tag pair contributes an item pair for
// each of its producible members, and an item pair contributes an item pair
// for each of its amount-qualified real inputs. The last two steps make the
// relation transitive; they only run in nonoptimal mode.
// ---- Witness seeds -----------------------------------------------------
// c1(m) is the intersection over m's recipes of the amount-qualified input
// sources, tag inputs expanded to their members. Each w in c1(m) seeds (m, w);
// bucketing by w lets the row loop below recover them one witness at a time.
void TagPruner::seedWitnessPairs() noexcept {
  witnessByW.clear();
  witnessByW.resize(nReal);

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
        } else if (j < nItem && allowedTags[j]) {
          scratch.insert(scratch.end(), members[j].begin(), members[j].end());
        }
      }
      dedup<NodeId>(scratch);
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
      witnessByW[w].push_back(m);
      if (++witnessPairs >= options.maxWitnessPairs)
        break;
    }
  }
}

// ---- One witness row ---------------------------------------------------
// Seeds the row (witness pairs plus the support pairs of every tag containing
// w), then adds the tag pairs its item pairs are gated through. In exact mode
// only the tags that literally contain w survive that gate, which is the old
// one-step rule; the tag-pair lookup in validItem then enforces the member
// restriction for free. In nonoptimal mode the gate is dropped and rowClosure()
// expands the relation.
bool TagPruner::seedRow(NodeId w) noexcept {
  rowItems.clear();
  rowTags.clear();

  const aw::vector<NodeId> &seeds = witnessByW[w];
  rowItems.insert(rowItems.end(), seeds.begin(), seeds.end());
  // Support pairs: a tag containing w supports (z, w) for each other member z
  // that is itself crafted.
  for (uint e = itemTagOffsets[w]; e < itemTagOffsets[w + 1]; e++) {
    const NodeId t = itemTagTargets[e];
    for (NodeId z : members[t]) {
      if (z == w || !producible[z])
        continue;
      rowItems.push_back(z);
    }
  }
  if (rowItems.empty())
    return false;
  dedup<NodeId>(rowItems);

  candRow.clear();
  for (NodeId m : rowItems)
    for (NodeId j : qualTags[m]) {
      if (!nonoptimal &&
          !std::binary_search(members[j].begin(), members[j].end(), w))
        continue;
      candRow.push_back(j);
    }
  mergeNewSorted<NodeId>(rowTags, candRow);

  if (nonoptimal)
    rowClosure(w);

  rowX.clear();
  rowX.reserve(rowItems.size() + rowTags.size());
  rowX.insert(rowX.end(), rowItems.begin(), rowItems.end());
  rowX.insert(rowX.end(), rowTags.begin(), rowTags.end());
  dedup<NodeId>(rowX);
  return true;
}

// ---- Transitive closure -----------------------------------------------
// Only the frontier is expanded, so every pair is processed once and the
// closure costs one pass over the pairs it reaches. options.maxPrunePairs caps
// what the closure adds; running out leaves pairs out of the relation, never
// in it. The cap is global but consumed one row at a time, so a row that grows
// large enough to exhaust it can leave later rows thinner than a global
// seeding order would; running out only leaves pairs out, so that is safe.
void TagPruner::rowClosure(NodeId w) noexcept {
  uint64_t total = prunePairs + rowItems.size() + rowTags.size();
  aw::vector<NodeId> frontierItems = rowItems;
  aw::vector<NodeId> frontierTags = rowTags;
  while ((!frontierItems.empty() || !frontierTags.empty()) &&
         total < options.maxPrunePairs) {
    candRow.clear();
    for (NodeId m : frontierItems)
      for (NodeId j : qualTags[m]) {
        if (total + candRow.size() >= options.maxPrunePairs)
          break;
        candRow.push_back(j);
      }
    const aw::vector<NodeId> newTags = mergeNewSorted<NodeId>(rowTags, candRow);
    total += newTags.size();

    candRow.clear();
    // A tag has to dominate w through all of its producible members.
    for (NodeId j : frontierTags)
      for (NodeId z : members[j]) {
        if (z == w || !producible[z])
          continue;
        if (total + candRow.size() >= options.maxPrunePairs)
          break;
        candRow.push_back(z);
      }
    // A real item m is dominated by w when its input j is; a raw input is
    // gathered rather than crafted, so it never requires w and is skipped by
    // the `producible` precomputation above.
    for (NodeId m : frontierItems)
      for (NodeId j : qualReal[m]) {
        if (j == w)
          continue;
        if (total + candRow.size() >= options.maxPrunePairs)
          break;
        candRow.push_back(j);
      }
    const aw::vector<NodeId> newItems =
        mergeNewSorted<NodeId>(rowItems, candRow);
    total += newItems.size();
    frontierItems = newItems;
    frontierTags = newTags;
  }
  prunePairs = total;
}

// ---- Row-local lookup --------------------------------------------------
// One open-addressed table answers (x, w) with a row-local id, since the row
// is the only witness in play. The table is sized to at least twice the row,
// so the load factor stays below 1/2; it is rebuilt per row from scratch.
void TagPruner::beginRow() noexcept {
  const uint k = (uint) rowX.size();
  uint cap = 1, bits = 0;
  while (cap < 2 * k) {
    cap <<= 1;
    bits++;
  }
  rowSlots.assign(cap, 0);
  rowMask = cap - 1;
  rowShift = 32 - bits;
  for (uint id = 0; id < k; id++) {
    uint s = (rowX[id] * 2654435761u) >> rowShift;
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
    if (rowX[v - 1] == x)
      return v - 1;
    s = (s + 1) & rowMask;
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
    if (need > 0 && h >= nReal && h < nItem && allowedTags[h]) {
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
    if (q == UINT32_MAX || !rowAlive[q])
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
      } else if (j >= nItem || !allowedTags[j]) {
        continue;
      }
      const uint32_t q = rowIdOf(j);
      if (q != UINT32_MAX && rowAlive[q]) {
        ok = true;
        break;
      }
    }
    if (!ok && !colCover(r, w))
      return false;
  }
  return true;
}

// ---- Greatest fixpoint, one witness row -------------------------------
// Every pair starts alive and is killed once when it loses its justification.
// Killing an item pair can invalidate the tag pairs that contain it as a
// member, and killing a tag pair invalidates the recipes that gated through
// it; both are re-queued, which yields the greatest fixpoint.
//
// The witness never changes during any of those steps, so the rows are
// independent and fixRow() solves exactly one of them. That is what lets the
// pair set of the row be built on demand and dropped afterwards.
void TagPruner::fixRow(NodeId w) noexcept {
  beginRow();
  const uint k = (uint) rowX.size();
  rowAlive.assign(k, 1);
  rowQueue.resize(k);
  rowQueued.assign(k, 0);
  size_t head = 0, tail = 0, pending = 0;
  auto push = [&](uint local) noexcept {
    if (rowQueued[local])
      return;
    rowQueued[local] = 1;
    rowQueue[tail] = local;
    tail++;
    if (tail == k)
      tail = 0;
    pending++;
  };
  for (uint local = 0; local < k; local++)
    push(local);

  while (pending != 0) {
    const uint local = rowQueue[head];
    head++;
    if (head == k)
      head = 0;
    pending--;
    rowQueued[local] = 0;
    if (!rowAlive[local])
      continue;

    const NodeId x = rowX[local];
    // A first component at or above nReal is a simple tag: its pair is the
    // AND over its members, not a recipe scan.
    const bool tagPair = x >= nReal;
    if (tagPair ? validTag(x, w) : validItem(x, w))
      continue;

    rowAlive[local] = 0;
    if (tagPair) {
      // The tag lost a member, so every real recipe that consumed it may no
      // longer gate through it.
      for (uint e = tagConsumerOffsets[x]; e < tagConsumerOffsets[x + 1]; e++) {
        const uint32_t q = rowIdOf(tagConsumerTargets[e]);
        if (q != UINT32_MAX)
          push(q);
      }
    } else {
      // The item lost, so every tag that contains it lost a supporting member.
      for (uint e = itemTagOffsets[x]; e < itemTagOffsets[x + 1]; e++) {
        const uint32_t q = rowIdOf(itemTagTargets[e]);
        if (q != UINT32_MAX)
          push(q);
      }
    }
  }
}

// ---- Keep one representative per sink SCC -----------------------------
// The SCC test used to binary-search a global universe; with the rows gone it
// reads the per-tag bit matrix instead. Bit (a, b) of tag t is alive(ms[a],
// ms[b]), which is exactly a support pair of witness ms[b] and therefore
// already decided by the time that row was fixed. The matrix is tiny: at most
// maxTagPairs bits across all tags.
void TagPruner::allocateTagAdjacency() noexcept {
  tagAdjOffsets.assign(nItem + 1, 0);
  uint64_t total = 0;
  for (NodeId t = nReal; t < nItem; t++) {
    tagAdjOffsets[t] = (uint) total;
    if (!allowedTags[t] || members[t].size() < 2)
      continue;
    const uint64_t k = members[t].size();
    total += k * ((k + 63) / 64);
  }
  tagAdjOffsets[nItem] = (uint) total;
  tagAdjBits.assign(total, 0);
}

void TagPruner::recordRowScc(NodeId w) noexcept {
  for (uint e = itemTagOffsets[w]; e < itemTagOffsets[w + 1]; e++) {
    const NodeId t = itemTagTargets[e];
    const aw::vector<NodeId> &ms = members[t];
    const size_t k = ms.size();
    if (k < 2)
      continue;
    // The tag contains w, so lower_bound finds it; its index is the column.
    const size_t b =
        (size_t) (std::lower_bound(ms.begin(), ms.end(), w) - ms.begin());
    const uint words = (uint) ((k + 63) / 64);
    const uint base = tagAdjOffsets[t];
    for (size_t a = 0; a < k; a++) {
      if (a == b)
        continue;
      const NodeId z = ms[a];
      // Only crafted members carry a pair, hence an edge.
      if (!producible[z])
        continue;
      const uint32_t q = rowIdOf(z);
      if (q == UINT32_MAX || !rowAlive[q])
        continue;
      tagAdjBits[base + a * words + (b >> 6)] |= 1ULL << (b & 63);
    }
  }
}

// On the alive edges inside a tag, every member reaches some sink SCC, and a
// sink representative is substitutable for everything that reaches it. This
// also keeps at least one member of every tag, including mutual-requirement
// cycles, so no tag becomes unsatisfiable.
void TagPruner::pruneDominatedEdges() noexcept {
  aw::vector<uint> adjOffsets, adjTargets;
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  for (NodeId t = nReal; t < nItem; t++) {
    if (!allowedTags[t])
      continue;
    const aw::vector<NodeId> &ms = members[t];
    const size_t k = ms.size();
    if (k < 2)
      continue;

    // Expand the bit matrix into the CSR adjacency the SCC helper wants.
    const uint words = (uint) ((k + 63) / 64);
    const uint base = tagAdjOffsets[t];
    adjOffsets.assign(k + 1, 0);
    for (size_t a = 0; a < k; a++) {
      uint32_t degree = 0;
      for (uint w = 0; w < words; w++)
        degree += (uint32_t) std::popcount(tagAdjBits[base + a * words + w]);
      adjOffsets[a + 1] = degree;
    }
    for (size_t a = 0; a + 1 <= k; a++)
      adjOffsets[a + 1] += adjOffsets[a];
    adjTargets.assign(adjOffsets[k], 0);
    for (size_t a = 0; a < k; a++) {
      uint32_t cursor = adjOffsets[a];
      for (uint w = 0; w < words; w++) {
        uint64_t bits = tagAdjBits[base + a * words + w];
        while (bits != 0) {
          const uint bit = (uint) std::countr_zero(bits);
          adjTargets[cursor++] = (uint32_t) (w * 64 + bit);
          bits &= bits - 1;
        }
      }
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
