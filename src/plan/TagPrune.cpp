// Tag-edge dominance pruning.
//
// A tag is a pseudo-item with one synthetic recipe `T <- m` per member m. When
// another member w cost-dominates m -- every way of crafting m either consumes
// at least as much w, or is replaceable by a recipe of w -- the edge `T <- m`
// is never part of an optimal plan, so it is marked dominated here and the
// query-time subgraph drops it. See docs/algorithm.typ.
//
// The pass is material, not cost-based: dropping `T <- m` in favour of `T <- w`
// is paid for by the executions that used to craft the m, so the objective only
// ever loses executions, all of which cost at least nothing. The one rule that
// does compare two executions -- the column-cover escape hatch in `covers` --
// requires the replacement not to cost more than what it replaces, which under
// a uniform cost always holds.
//
// The pass computes the greatest fixpoint of a candidate relation over pairs
// (x, w), read "x is dominated by w", where x is a real item or a tag
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
//
// Every node id read out of a recipe is below nItem: the parser sizes the item
// arrays from the largest handle in the dump, so an input either is a real item
// (below nReal) or a tag, and nothing below has to re-check that.

#include <algorithm>
#include <bit>

#include "Prune.h"
#include "aw/plan/Options.h"

namespace aw::detail {

using uint = uint32_t;

namespace {

// rowState flags of the row being fixed: the pair's first component is in the
// row, its pair is still alive, and it sits in the worklist.
enum RowFlag : uint8_t {
  ROW_IN = 1,
  ROW_ALIVE = 2,
  ROW_QUEUED = 4,
};

void prefixSum(aw::vector<uint> &v) noexcept {
  v.insert(v.begin(), 0);
  for (uint i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
}

void intersect(const aw::vector<ItemId> &a, const aw::vector<ItemId> &b,
                     aw::vector<ItemId> &out) noexcept {
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

// Merges two ascending, internally unique runs into `out`. The item ids of a
// witness row all sit below nReal and its tag ids all at or above it, so the
// two halves of a row never overlap and the merge needs no duplicate handling.
void mergeDisjoint(const aw::vector<ItemId> &a, const aw::vector<ItemId> &b,
                   aw::vector<ItemId> &out) noexcept {
  out.clear();
  out.reserve(a.size() + b.size());
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size())
    out.push_back_unchecked(a[i] < b[j] ? a[i++] : b[j++]);
  while (i < a.size())
    out.push_back_unchecked(a[i++]);
  while (j < b.size())
    out.push_back_unchecked(b[j++]);
}

// State of one tag-pruning pass over the crafting graph. run() calls the
// stages in this order:
//
//   findSimpleTags          which pseudo-items are tags, and their members
//   budgetTagUniverse       admit tags into the relation within the pair budget
//   buildMemberIndex        member -> tags containing it (CSR)
//   buildConsumerIndex      tag -> real items consuming it (CSR)
//   buildQualifiedInputs    per-recipe inputs consumed in at least the output amount
//   computeQualifiedInputs  per-item union of those, the closure seeds
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
  aw::vector<aw::vector<ItemId>> members;  // nItem
  // producible[m]: m has at least one recipe (it is crafted, not gathered).
  aw::vector<uint8_t> producible;          // nItem

  // member -> tags containing it, ascending.
  aw::vector<uint> itemTagOffsets;         // nReal + 1 after prefixSum
  aw::vector<ItemId> itemTagTargets;
  // tag -> real items that consume it in a real recipe. Duplicates are kept;
  // the worklist dedups them.
  aw::vector<uint> tagConsumerOffsets;     // nItem + 1 after prefixSum
  aw::vector<ItemId> tagConsumerTargets;

  // Amount-qualified inputs, see computeQualifiedInputs().
  aw::vector<aw::vector<ItemId>> qualReal, qualTags;  // nReal each

  // Per recipe, the inputs it consumes at least as much of as it outputs, with
  // tags the budget dropped left out. Recipes, amounts and the tag budget are
  // all fixed before the row loop, so the rule is applied once here instead of
  // on every one of the millions of validItem() and seedWitnessPairs() steps
  // that need it. computeQualifiedInputs() unions these per item.
  aw::vector<uint> qualInputOffsets;       // nRecipe + 1 after prefixSum
  aw::vector<ItemId> qualInputItems;

  // Whether a recipe names at least one allowed tag among its inputs. A tag
  // input is flexible -- any member can pay for it -- so such a recipe's column
  // is not a fixed vector and covers() has to build the member pool for it.
  aw::vector<uint8_t> hasAllowedTagInput;  // nRecipe

  // Witness seeds grouped by witness: witnessByW[w] holds the m with a witness
  // pair (m, w). Seeding is per item but solving is per witness, so the pairs
  // are bucketed here instead of stored packed.
  aw::vector<aw::vector<ItemId>> witnessByW;  // nReal

  // The witness row currently being solved. Pairs carry their first component
  // only, because their witness is the row being processed. `rowItems` and
  // `rowTags` are sorted and unique and `rowX` is their sorted union. The rows
  // of one witness are independent, so a row is thrown away as soon as it is
  // fixed.
  aw::vector<ItemId> rowItems, rowTags, rowX;
  aw::vector<ItemId> candRow;

  // Scratch bitmap over the item id universe, one bit per item, always left
  // clear. sortIds() uses it to sort and dedup a batch without comparisons.
  aw::vector<uint64_t> idBits;             // ceil(nItem / 64)

  // The alive co-member edges of every tag, packed as a bit matrix so that the
  // SCC stage never needs a discarded row: bit (a, b) of tag t is set when
  // alive(members[t][a], members[t][b]). tagAdjBits is [tagAdjOffsets[t],
  // tagAdjOffsets[t + 1]) for tag t, k * ceil(k / 64) words, row-major.
  aw::vector<uint> tagAdjOffsets;          // nItem + 1
  aw::vector<uint64_t> tagAdjBits;

  // State of the pairs of the witness row currently being fixed, indexed by
  // first component: a byte per item holding the ROW_* flags. The row is small
  // next to the universe and every lookup is on one of its ids, so the few
  // lines it touches stay in cache and a flat array answers in one load -- no
  // hash, no probe, no parallel id array.
  aw::vector<uint8_t> rowState;            // nItem
  // The fixpoint worklist. It holds first components, not row-local ids, so a
  // pop needs no second lookup and the queue cannot outlive the row.
  aw::vector<ItemId> rowQueue;

  // Batching guard, see computeUnitOutputs(). Empty in nonoptimal mode.
  aw::vector<uint8_t> unitOutput;          // nItem

  // Column-cover work budget and scratch, see covers().
  uint64_t coverBudget = 0;
  aw::vector<ItemEntry> cap;

  // The closure's global pair budget, carried across witness rows.
  uint64_t prunePairs = 0;

  void findSimpleTags() noexcept;
  void budgetTagUniverse() noexcept;
  void buildMemberIndex() noexcept;
  void buildConsumerIndex() noexcept;
  void buildQualifiedInputs() noexcept;
  void computeQualifiedInputs() noexcept;
  void computeUnitOutputs() noexcept;
  void allocateTagAdjacency() noexcept;
  void seedWitnessPairs() noexcept;
  // Replaces `v` with its ascending, duplicate-free contents.
  void sortIds(aw::vector<ItemId> &v) noexcept;
  // Merges the (unsorted, possibly duplicated) `cand` into the sorted-unique
  // `dst`, keeping `dst` sorted, and returns the entries that were not already
  // in it.
  aw::vector<ItemId> mergeNewSorted(aw::vector<ItemId> &dst,
                                    aw::vector<ItemId> &cand) noexcept;
  // Builds the pair set for witness w; false when the row has no seeds.
  bool seedRow(ItemId w) noexcept;
  // Expands that row to its closure; nonoptimal mode only.
  void rowClosure(ItemId w) noexcept;
  // Marks the pairs of the row as present and alive in rowState.
  void beginRow() noexcept;
  // Clears those marks again.
  void endRow() noexcept;
  // True while the pair (x, w) of the row being fixed is alive.
  bool rowAliveOf(ItemId x) const noexcept;
  // Kills the row's unjustified pairs until convergence.
  void fixRow(ItemId w) noexcept;
  // Copies the row's alive co-member edges into tagAdjBits.
  void recordRowScc(ItemId w) noexcept;
  // True when one execution of recipe s can replace one of r, see the
  // column-cover comment above covers().
  bool covers(uint s, uint r) noexcept;
  bool colCover(uint r, ItemId w) noexcept;
  bool validTag(ItemId t, ItemId w) noexcept;
  bool validItem(ItemId m, ItemId w) noexcept;
  void pruneDominatedEdges() noexcept;
};

void TagPruner::run() noexcept {
  graph.tagEdgeDominated.assign(nRecipe, 0);
  if (nItem == 0 || nReal == 0 || nRecipe == 0)
    return;

  idBits.assign((nItem + 63) / 64, 0);
  rowState.assign(nItem, 0);
  findSimpleTags();
  budgetTagUniverse();
  buildMemberIndex();
  buildConsumerIndex();
  buildQualifiedInputs();
  computeQualifiedInputs();
  computeUnitOutputs();
  allocateTagAdjacency();
  seedWitnessPairs();

  coverBudget = options.maxTagCoverWork;
  for (ItemId w = 0; w < nReal; w++) {
    if (!seedRow(w))
      continue;
    fixRow(w);
    recordRowScc(w);
    endRow();
  }
  pruneDominatedEdges();
}

// Optimized from std::sort + std::unique.
// Use a bitset when v is large, to avoid O(n log n).
void TagPruner::sortIds(aw::vector<ItemId> &v) noexcept {
  const size_t n = v.size();
  if (n < 2)
    return;
  if (n < 64 || n * 8 < idBits.size()) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return;
  }

  // Bucket sorting, set bit `x` to 1.
  for (ItemId x : v)
    idBits[x >> 6] |= 1ULL << (x & 63);

  // Scan through 1's and extract the elements.
  uint out = 0;
  for (uint word = 0; word < idBits.size(); word++) {
    uint64_t bits = idBits[word];
    if (bits == 0)
      continue;
    idBits[word] = 0;
    do {
      v[out++] = (ItemId) (word * 64 + std::countr_zero(bits));
      bits &= bits - 1;
    } while (bits != 0);
  }
  v.resize(out);
}

// Merges the (unsorted, possibly duplicated) `cand` into the sorted-unique
// `dst`, keeping `dst` sorted, and returns the entries that were not already in
// it. A row holds first components only -- its witness is the row index -- so
// the ids are always plain NodeIds. Turning the batch into a sorted run first
// and then merging it linearly is what lets the closure expand one frontier at
// a time: whatever `dst` already held is dropped here, so the returned frontier
// is exactly the new pairs of that round.
aw::vector<ItemId> TagPruner::mergeNewSorted(aw::vector<ItemId> &dst,
                                             aw::vector<ItemId> &cand) noexcept {
  sortIds(cand);
  aw::vector<ItemId> added;
  if (cand.empty())
    return added;
  // Every new entry comes from `cand`, so size the result up front.
  added.reserve(cand.size());

  aw::vector<ItemId> merged;
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

// ---- Simple tags and their members ------------------------------------
// A tag is a pseudo-item whose recipes are all workstation-free `T <- m` with a
// single real member m. That is a contract of the registered data, so the
// recipe scan below takes inputs[0] on trust; members[t] comes out sorted and
// unique.
void TagPruner::findSimpleTags() noexcept {
  allowedTags.assign(nItem, 0);
  members.resize(nItem);
  for (ItemId t = nReal; t < nItem; t++) {
    const auto recipes = graph.producersOf(t);
    if (recipes.empty())
      continue;

    aw::vector<ItemId> ms;
    ms.reserve(recipes.size());
    for (RecipeId r : recipes) {
      const auto inputs = graph.inputsOf(r);
      ms.push_back_unchecked(inputs[0]);
    }

    sortIds(ms);
    allowedTags[t] = 1;
    members[t] = std::move(ms);
  }

  producible.assign(nItem, 0);
  for (ItemId m = 0; m < nItem; m++)
    if (!graph.producersOf(m).empty())
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
  for (ItemId t = nReal; t < nItem; t++)
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
  for (ItemId t = nReal; t < nItem; t++) {
    if (allowedTags[t])
      for (ItemId m : members[t])
        itemTagOffsets[m]++;
  }
  prefixSum(itemTagOffsets);
  itemTagTargets.resize(itemTagOffsets.back());
  aw::vector<uint> cursor(itemTagOffsets.begin(), itemTagOffsets.end() - 1);
  for (ItemId t = nReal; t < nItem; t++) {
    if (allowedTags[t])
      for (ItemId m : members[t])
        itemTagTargets[cursor[m]++] = t;
  }
}

void TagPruner::buildConsumerIndex() noexcept {
  // Only a real recipe can be a consumer, and real recipes are a prefix of the
  // recipe list, so both passes stop at the first tag edge. See the ordering
  // note on BaseCraftingGraph::output.
  tagConsumerOffsets.assign(nItem, 0);
  for (uint r = 0; r < nRecipe && graph.output[r] < nReal; r++) {
    // A byproduct recipe names several outputs, so the tag substitution
    // relation has no single item to attach it to. Leaving it out is
    // conservative: it only shrinks the relation.
    if (graph.hasByproducts(r))
      continue;
    for (ItemId j : graph.inputsOf(r)) {
      if (j >= nReal && allowedTags[j])
        tagConsumerOffsets[j]++;
    }
  }
  prefixSum(tagConsumerOffsets);
  tagConsumerTargets.resize(tagConsumerOffsets.back());
  aw::vector<uint> cursor(tagConsumerOffsets.begin(),
                          tagConsumerOffsets.end() - 1);
  for (uint r = 0; r < nRecipe && graph.output[r] < nReal; r++) {
    if (graph.hasByproducts(r))
      continue;
    const ItemId m = graph.output[r];
    for (ItemId j : graph.inputsOf(r))
      if (j >= nReal && allowedTags[j])
        tagConsumerTargets[cursor[j]++] = m;
  }
}

// ---- Amount-qualified inputs ------------------------------------------
// A recipe's amount-qualified inputs are the ones a single execution of it
// consumes at least as much of as the recipe outputs; only those can carry a
// substitution, because a plan can drop one execution and pay for it with the
// input. A tag the budget dropped is left out, since it can never justify a
// pair -- exactly as if the recipe did not list it.
//
// The same sweep also marks the recipes that name an allowed tag at all, which
// is what tells covers() it cannot use the plain column comparison. That bit is
// about every input, not just the qualified ones, so it is set before the
// byproduct skip: covers() rejects a byproduct recipe anyway, but it reads the
// bit first.
void TagPruner::buildQualifiedInputs() noexcept {
  qualInputOffsets.assign(nRecipe, 0);
  hasAllowedTagInput.assign(nRecipe, 0);
  for (uint r = 0; r < nRecipe; r++) {
    for (ItemId j : graph.inputsOf(r))
      if (j >= nReal && allowedTags[j]) {
        hasAllowedTagInput[r] = 1;
        break;
      }
    if (graph.hasByproducts(r))
      continue;
    const auto inputs = graph.inputsOf(r);
    const auto weights = graph.inputAmountsOf(r);
    const Amount out = graph.outputAmt[r];
    for (size_t k = 0; k < inputs.size(); k++)
      if (weights[k] >= out && (inputs[k] < nReal || allowedTags[inputs[k]]))
        qualInputOffsets[r]++;
  }
  prefixSum(qualInputOffsets);
  qualInputItems.resize(qualInputOffsets.back());

  aw::vector<uint> cursor(qualInputOffsets.begin(), qualInputOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.hasByproducts(r))
      continue;
    const auto inputs = graph.inputsOf(r);
    const auto weights = graph.inputAmountsOf(r);
    const Amount out = graph.outputAmt[r];
    for (size_t k = 0; k < inputs.size(); k++) {
      const ItemId j = inputs[k];
      if (weights[k] >= out && (j < nReal || allowedTags[j]))
        qualInputItems[cursor[r]++] = j;
    }
  }
}

// ---- The per-item union of those --------------------------------------
// Per real item: the real items and tags that some recipe of it
// consumes in an amount at least equal to that recipe's output. `validItem`
// reads the per-recipe lists directly; the closure below expands this union.
void TagPruner::computeQualifiedInputs() noexcept {
  qualReal.resize(nReal);
  qualTags.resize(nReal);
  for (ItemId m = 0; m < nReal; m++) {
    for (RecipeId r : graph.producersOf(m)) {
      for (uint e = qualInputOffsets[r]; e < qualInputOffsets[r + 1]; e++) {
        const ItemId j = qualInputItems[e];
        if (j < nReal) {
          // A raw input is gathered, not crafted, so it never requires w.
          if (producible[j])
            qualReal[m].push_back(j);
        } else {
          qualTags[m].push_back(j);
        }
      }
    }
    sortIds(qualReal[m]);
    sortIds(qualTags[m]);
  }
}

// On optimal mode, we can prune only if it's 1:1.
void TagPruner::computeUnitOutputs() noexcept {
  if (nonoptimal)
    return;
  unitOutput.assign(nItem, 1);
  for (ItemId m = 0; m < nReal; m++) {
    for (RecipeId r : graph.producersOf(m))
      if (graph.hasByproducts(r) || graph.producedAmountOf(r, m) != 1) {
        unitOutput[m] = 0;
        break;
      }
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

  aw::vector<ItemId> acc, next, scratch;
  uint64_t witnessPairs = 0;
  for (ItemId m = 0; m < nReal && witnessPairs < options.maxWitnessPairs; m++) {
    const auto recipes = graph.producersOf(m);
    if (recipes.empty())
      continue;

    bool first = true;
    acc.clear();
    for (RecipeId r : recipes) {
      scratch.clear();
      for (uint e = qualInputOffsets[r]; e < qualInputOffsets[r + 1]; e++) {
        const ItemId j = qualInputItems[e];
        if (j < nReal)
          scratch.push_back(j);
        else
          scratch.insert(scratch.end(), members[j].begin(), members[j].end());
      }
      sortIds(scratch);
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
    for (ItemId w : acc) {
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
bool TagPruner::seedRow(ItemId w) noexcept {
  rowItems.clear();
  rowTags.clear();

  const aw::vector<ItemId> &seeds = witnessByW[w];
  rowItems.insert(rowItems.end(), seeds.begin(), seeds.end());
  // Support pairs: a tag containing w supports (z, w) for each other member z
  // that is itself crafted.
  for (uint e = itemTagOffsets[w]; e < itemTagOffsets[w + 1]; e++) {
    const ItemId t = itemTagTargets[e];
    for (ItemId z : members[t]) {
      if (z == w || !producible[z])
        continue;
      rowItems.push_back(z);
    }
  }
  if (rowItems.empty())
    return false;
  sortIds(rowItems);

  candRow.clear();
  for (ItemId m : rowItems)
    for (ItemId j : qualTags[m]) {
      if (!nonoptimal &&
          !std::binary_search(members[j].begin(), members[j].end(), w))
        continue;
      candRow.push_back(j);
    }
  mergeNewSorted(rowTags, candRow);

  if (nonoptimal)
    rowClosure(w);

  // Every item id sits below nReal and every tag id at or above it, so the row
  // is the two sorted runs merged back to back.
  mergeDisjoint(rowItems, rowTags, rowX);
  return true;
}

// ---- Transitive closure -----------------------------------------------
// Only the frontier is expanded, so every pair is processed once and the
// closure costs one pass over the pairs it reaches. options.maxPrunePairs caps
// what the closure adds; running out leaves pairs out of the relation, never
// in it. The cap is global but consumed one row at a time, so a row that grows
// large enough to exhaust it can leave later rows thinner than a global
// seeding order would; running out only leaves pairs out, so that is safe.
void TagPruner::rowClosure(ItemId w) noexcept {
  uint64_t total = prunePairs + rowItems.size() + rowTags.size();
  aw::vector<ItemId> frontierItems = rowItems;
  aw::vector<ItemId> frontierTags = rowTags;
  while ((!frontierItems.empty() || !frontierTags.empty()) &&
         total < options.maxPrunePairs) {
    candRow.clear();
    for (ItemId m : frontierItems)
      for (ItemId j : qualTags[m]) {
        if (total + candRow.size() >= options.maxPrunePairs)
          break;
        candRow.push_back(j);
      }
    const aw::vector<ItemId> newTags = mergeNewSorted(rowTags, candRow);
    total += newTags.size();

    candRow.clear();
    // A tag has to dominate w through all of its producible members.
    for (ItemId j : frontierTags)
      for (ItemId z : members[j]) {
        if (z == w || !producible[z])
          continue;
        if (total + candRow.size() >= options.maxPrunePairs)
          break;
        candRow.push_back(z);
      }
    // A real item m is dominated by w when its input j is; a raw input is
    // gathered rather than crafted, so it never requires w and is skipped by
    // the `producible` precomputation above.
    for (ItemId m : frontierItems)
      for (ItemId j : qualReal[m]) {
        if (j == w)
          continue;
        if (total + candRow.size() >= options.maxPrunePairs)
          break;
        candRow.push_back(j);
      }
    const aw::vector<ItemId> newItems =
        mergeNewSorted(rowItems, candRow);
    total += newItems.size();
    frontierItems = newItems;
    frontierTags = newTags;
  }
  prunePairs = total;
}

// ---- Row-local lookup --------------------------------------------------
// The row is the only witness in play, so a pair is identified by its first
// component alone and one flat array over the item universe answers all three
// questions the fixpoint asks about it: is it in the row, is it still alive,
// is it queued. See rowState.
void TagPruner::beginRow() noexcept {
  for (ItemId x : rowX)
    rowState[x] = ROW_IN | ROW_ALIVE;
}

void TagPruner::endRow() noexcept {
  for (ItemId x : rowX)
    rowState[x] = 0;
}

bool TagPruner::rowAliveOf(ItemId x) const noexcept {
  return (rowState[x] & ROW_ALIVE) != 0;
}

// Consumes up to `need` units of `item` from `pool`, and returns what is still missing.
// The sizes are always small (mostly 1 item each), so we use a linear scan.
Amount consume(aw::vector<ItemEntry> &pool, ItemId item,
               Amount need) noexcept {
  for (auto &p : pool) {
    if (p.item != item)
      continue;
    const Amount take = std::min(need, p.amt);
    p.amt -= take;
    return need - take;
  }
  return need;
}

// ---- Column cover ------------------------------------------------------
// Besides consuming a dominator (directly or through a tag), a recipe of m
// can also be replaced by a recipe of the candidate dominator: it must yield
// at least as much and consume no more of every item, with a tag it
// consumes allowed to pick any member. One execution of s then stands in for
// one of r, so it must not cost more than r either; under a uniform cost that
// is always true.
//
// A recipe that consumes no allowed tag has a fixed column and is decided by
// leVectorRaw alone; only a tag input needs the mutable member pool.
bool TagPruner::covers(uint s, uint r) noexcept {
  if (coverBudget == 0)
    return false;
  coverBudget--;
  if (graph.hasByproducts(s) || graph.hasByproducts(r))
    return false;
  if (graph.cost[s] > graph.cost[r])
    return false;
  if (graph.outputAmt[s] < graph.outputAmt[r])
    return false;

  const auto ri = graph.inputsOf(r);
  const auto rw = graph.inputAmountsOf(r);
  const auto si = graph.inputsOf(s);
  const auto sw = graph.inputAmountsOf(s);
  if (!hasAllowedTagInput[s])
    return leVectorRaw(si.data(), sw.data(), si.size(),
                       ri.data(), rw.data(), ri.size());

  cap.clear();
  for (size_t j = 0; j < ri.size(); j++)
    cap.emplace_back(ri[j], rw[j]);

  for (size_t j = 0; j < si.size(); j++) {
    const ItemId h = si[j];

    // An exact row first, so a tag consumed atomically matches itself.
    Amount need = consume(cap, h, sw[j]);
    if (need > 0 && h >= nReal && allowedTags[h]) {
      for (ItemId z : members[h]) {
        if (need <= 0)
          break;
        need = consume(cap, z, need);
      }
    }
    if (need > 0)
      return false;
  }
  return true;
}

bool TagPruner::colCover(uint r, ItemId w) noexcept {
  for (RecipeId s : graph.producersOf(w))
    if (covers(s, r))
      return true;
  return false;
}

// A tag pair (t, w) holds when every producible member of t is dominated by
// w. A member with no recipe is gathered directly, so it never requires w and
// it blocks the gate: it has no pair, so the lookup fails.
bool TagPruner::validTag(ItemId t, ItemId w) noexcept {
  for (ItemId z : members[t]) {
    if (z == w)
      continue;
    if (!rowAliveOf(z))
      return false;
  }
  return true;
}

// A real item m is dominated by w when every recipe of m has an
// amount-qualified input that is w itself, an item pair in turn dominated by
// w, or a tag pair dominated by w. The column-cover rule is the same escape
// hatch as before.
bool TagPruner::validItem(ItemId m, ItemId w) noexcept {
  for (RecipeId r : graph.producersOf(m)) {
    bool ok = false;
    for (uint e = qualInputOffsets[r]; e < qualInputOffsets[r + 1]; e++) {
      const ItemId j = qualInputItems[e];
      // Consuming w itself is the strongest justification, and it holds even
      // for a gathered w, which never carries a pair of its own.
      if (j == w) {
        ok = true;
        break;
      }
      // Chaining a real input is the transitive closure; exact mode only
      // accepts the direct w above. A raw input has no pair, so the lookup
      // below fails for it, which is what we want.
      if (j < nReal && !nonoptimal)
        continue;
      if (rowAliveOf(j)) {
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
void TagPruner::fixRow(ItemId w) noexcept {
  beginRow();
  const uint k = (uint) rowX.size();
  rowQueue.resize(k);
  size_t head = 0, tail = 0, pending = 0;
  auto push = [&](ItemId x) noexcept {
    if (rowState[x] & ROW_QUEUED)
      return;
    rowState[x] |= ROW_QUEUED;
    rowQueue[tail] = x;
    tail++;
    if (tail == k)
      tail = 0;
    pending++;
  };
  for (ItemId x : rowX)
    push(x);

  while (pending != 0) {
    const ItemId x = rowQueue[head];
    head++;
    if (head == k)
      head = 0;
    pending--;
    rowState[x] &= (uint8_t) ~ROW_QUEUED;
    if (!(rowState[x] & ROW_ALIVE))
      continue;

    // A first component at or above nReal is a tag: its pair is the
    // AND over its members, not a recipe scan.
    const bool tagPair = x >= nReal;
    if (tagPair ? validTag(x, w) : validItem(x, w))
      continue;

    rowState[x] &= (uint8_t) ~ROW_ALIVE;
    if (tagPair) {
      // The tag lost a member, so every real recipe that consumed it may no
      // longer gate through it.
      for (uint e = tagConsumerOffsets[x]; e < tagConsumerOffsets[x + 1]; e++)
        if (rowState[tagConsumerTargets[e]] & ROW_IN)
          push(tagConsumerTargets[e]);
    } else {
      // The item lost, so every tag that contains it lost a supporting member.
      for (uint e = itemTagOffsets[x]; e < itemTagOffsets[x + 1]; e++)
        if (rowState[itemTagTargets[e]] & ROW_IN)
          push(itemTagTargets[e]);
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
  for (ItemId t = nReal; t < nItem; t++) {
    tagAdjOffsets[t] = (uint) total;
    if (!allowedTags[t] || members[t].size() < 2)
      continue;
    const uint64_t k = members[t].size();
    total += k * ((k + 63) / 64);
  }
  tagAdjOffsets[nItem] = (uint) total;
  tagAdjBits.assign(total, 0);
}

void TagPruner::recordRowScc(ItemId w) noexcept {
  for (uint e = itemTagOffsets[w]; e < itemTagOffsets[w + 1]; e++) {
    const ItemId t = itemTagTargets[e];
    const aw::vector<ItemId> &ms = members[t];
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
      const ItemId z = ms[a];
      // Only crafted members carry a pair, hence an edge.
      if (!producible[z] || !rowAliveOf(z))
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
  for (ItemId t = nReal; t < nItem; t++) {
    if (!allowedTags[t])
      continue;
    const aw::vector<ItemId> &ms = members[t];
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
      const ItemId m = ms[a];
      // A batched member keeps its tag edge: the batch surplus would otherwise
      // be a free way to satisfy T, and dropping the edge costs real steps.
      // Nonoptimal mode waives the guard, see above.
      if (!nonoptimal && !unitOutput[m])
        continue;
      for (RecipeId r : graph.producersOf(t)) {
        const auto inputs = graph.inputsOf(r);
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
