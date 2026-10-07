// Variant folding.
//
// docs/algorithm.typ (@multi-exit) has the statement. In one line: a
// decorative twin d of a base b is one that a tag, or a chain of "same recipe,
// other item" copies, makes interchangeable with b everywhere it is demanded
// by name, so a plan can produce b directly instead of routing through d.
//
// The soundness argument is the projection lemma:
//
//   Let D be the folded items and sigma : D -> items \ D the fold, extended by
//   sigma(u) = u outside D. If for every recipe r there is a D-free plan r*
//   with A(r*) = pi(A r) and cost(r*) <= cost(r), then no optimal plan uses a
//   recipe that mentions D, provided no item of D is stocked and the target is
//   not in D.
//
// The pass never trusts the heuristic: it builds a candidate fold (conversion
// components, the most-demanded member as the base, the tags as the
// orientation), then verifies every D-touching recipe against the D-free
// recipes of the subgraph (a tag member may stand in for the item it contains),
// and only marks recipes when the whole fold verifies. On failure it unfolds
// the folded items the failures point at and retries; if the budget runs out
// with failures left, nothing is marked. Running at query time is what lets a
// fold that is invalid on the full graph survive the recipes the other passes
// already removed.

#include "Prune.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_map>

#include "aw/plan/Options.h"
#include "aw/plan/Profiler.h" // IWYU pragma: keep
#include "aw/utils/LargeStackCall.h"

namespace aw {

namespace {

using uint = uint32_t;

constexpr ItemId kNoItem = UINT32_MAX;

// Mixes one (item, amount) row into a 64-bit signature.
inline uint64_t mixRow(uint64_t h, ItemId item, Amount amount) noexcept {
  h ^= (uint64_t) item * 0x9E3779B97F4A7C15ull + (uint64_t) amount;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 29;
  return h;
}

// One query's indexes plus the fold and its signature check.
struct VariantFoldRun {
  const BaseCraftingGraph &g;
  const ItemId target;
  const uint nReal;
  const uint nItem;
  const uint nRecipe;
  // Real recipes are a prefix of the recipe list, tag edges the suffix.
  const uint nRealRecipe;

  const std::chrono::steady_clock::time_point started;

  // Tag index: `memberTags[m]` lists the tags whose member list contains the
  // real item m, ascending. Every item handle at or above `nReal` is a tag, so
  // no separate "is this a tag" flag is needed.
  aw::vector<aw::vector<ItemId>> memberTags;   // nReal
  aw::vector<uint8_t> tagHasRealConsumer;      // nItem

  // Neutral conversions: one output, one input, equal amounts, both real. The
  // edge points from the consumed member to the produced one.
  aw::vector<aw::vector<ItemId>> convOut;      // nReal
  aw::vector<aw::vector<ItemId>> convIn;       // nReal
  aw::vector<uint8_t> neutralR;                // nRecipe

  // The current fold. sigma[u] == u means "kept".
  aw::vector<ItemId> sigma;                    // nReal
  aw::vector<ItemId> candidates;               // nReal, sigma[u] != u
  aw::vector<uint8_t> stocked;                 // nReal
  aw::vector<uint8_t> rejected;                // nReal, growth may not re-add
  aw::vector<uint32_t> demand;                 // nReal
  aw::vector<uint32_t> comp;                   // nReal

  // Verification scratch.
  aw::vector<Amount> accum;                    // nItem
  aw::vector<uint8_t> accumSet;                // nItem
  aw::vector<ItemId> touched;                  // nItem
  aw::vector<ItemEntry> posScratch;
  aw::vector<ItemEntry> needScratch;
  aw::vector<uint8_t> usedScratch;
  aw::vector<uint32_t> failing;

  // CSR form of witness index. witness[item] = { recipes },
  // such that the first output (anchor) of each recipe in the set is `item`.
  aw::vector<uint32_t> witnessOffsets;  // nItem + 1
  aw::vector<uint32_t> witnessRecipes;  // number of D-free recipes
  aw::vector<uint32_t> witnessCursor;   // nItem, next slot per bucket
  aw::vector<uint32_t> alive;           // D-free recipes, ascending
  aw::vector<uint32_t> touching;        // D-touching recipes, ascending

  // Shape index for `tryGrow`, keyed by the output and input amounts alone.
  // Built once per run, so the hash map's per-key vectors are affordable.
  std::unordered_map<uint64_t, aw::vector<uint32_t>> byShape;
  // Set while a growth round proposes a fold; the caller restarts then.
  bool grew = false;

  VariantFoldRun(const Subgraph &sub, ItemId target,
                 std::chrono::steady_clock::time_point started) noexcept
      : g(sub.graph), target(target), nReal(sub.graph.nReal),
        nItem(sub.graph.nItem), nRecipe(sub.graph.nRecipe),
        nRealRecipe(firstTagEdge(sub.graph)), started(started),
        memberTags(nReal, aw::vector<ItemId> {}),
        tagHasRealConsumer(nItem, 0),
        convOut(nReal, aw::vector<ItemId> {}), convIn(nReal, aw::vector<ItemId> {}),
        neutralR(nRecipe, 0), sigma(nReal, 0),
        stocked(nReal, 0), rejected(nReal, 0), demand(nReal, 0), comp(nReal, UINT32_MAX),
        accum(nItem, 0), accumSet(nItem, 0)      
  {
    alive.reserve(nRecipe);
    touching.reserve(nRecipe);
  }

  static uint firstTagEdge(const BaseCraftingGraph &graph) noexcept {
    uint r = 0;
    while (r < graph.nRecipe && graph.output[r] < graph.nReal)
      r++;
    return r;
  }

  [[nodiscard]]
  bool timeLeft() const noexcept {
    if (options.variantFold.maxSeconds <= 0)
      return true;
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    return elapsed < options.variantFold.maxSeconds;
  }

  [[nodiscard]]
  ItemId rep(ItemId node) const noexcept {
    return node < nReal ? sigma[node] : node;
  }

  [[nodiscard]]
  bool touchesFold(uint r) const noexcept {
    for (ItemId o : g.outputsOf(r))
      if (o < nReal && sigma[o] != o)
        return true;
    for (ItemId in : g.inputsOf(r))
      if (in < nReal && sigma[in] != in)
        return true;
    return false;
  }

  [[nodiscard]]
  std::span<const uint32_t> witnessesOf(ItemId anchor) noexcept {
    return {witnessRecipes.data() + witnessOffsets[anchor],
            witnessRecipes.data() + witnessOffsets[anchor + 1]};
  }

  void buildTagIndex() noexcept;
  void buildConversions() noexcept;
  // Seeds the fold from the conversion components: picks each component's base
  // and folds the twins that base reaches onto it. Runs on the large stack.
  bool buildCandidates(std::span<const uint8_t> hasStock) noexcept;
  void buildWitnessIndex() noexcept;
  void buildShapeIndex() noexcept;
  // Fills posScratch / needScratch from pi(A r). False when the column is 0.
  bool projectedColumn(uint r) noexcept;
  // True when a D-free recipe replays the projected column of r.
  bool columnRealizable(uint r, Amount cost) noexcept;
  // Grows the fold so that r's projection can be replayed, if possible.
  bool tryGrow(uint r) noexcept;
  bool proposeFold(ItemId from, ItemId to) noexcept;
  void collectFailures() noexcept;
  bool verifyAndRepair() noexcept;
};

static bool matchInputs(const VariantFoldRun &run,
                        std::span<const ItemEntry> required,
                        std::span<const ItemId> ins,
                        std::span<const Amount> inAmts, size_t index,
                        aw::vector<uint8_t> &used) noexcept;

// Like `matchInputs`, but a required item that no witness input can take may be
// folded onto a witness input of the same conversion component, and the fold is
// recorded in `proposals`. Used by the growth step only; the check itself never
// grows the fold.
static bool matchInputsGrow(VariantFoldRun &run,
                            std::span<const ItemEntry> required,
                            std::span<const ItemId> ins,
                            std::span<const Amount> inAmts, size_t index,
                            aw::vector<uint8_t> &used,
                            aw::vector<std::pair<ItemId, ItemId>> &proposals) noexcept;

void VariantFoldRun::buildTagIndex() noexcept {
  for (ItemId t = nReal; t < nItem; t++) {
    aw::vector<ItemId> members;
    for (RecipeId r : g.producersOf(t))
      members.push_back(g.inputsOf(r)[0]);
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    for (ItemId m : members)
      memberTags[m].push_back(t);
  }
  for (uint r = 0; r < nRealRecipe; r++)
    for (ItemId in : g.inputsOf(r))
      if (in >= nReal)
        tagHasRealConsumer[in] = 1;
}

// Mark equal-amount conversions: `a * out <- a * in`.
void VariantFoldRun::buildConversions() noexcept {
  for (uint r = 0; r < nRealRecipe; r++) {
    const auto outs = g.outputsOf(r);
    const auto ins = g.inputsOf(r);
    if (outs.size() != 1 || ins.size() != 1 || outs[0] >= nReal ||
        ins[0] >= nReal || g.outputAmountsOf(r)[0] != g.inputAmountsOf(r)[0])
      continue;
    neutralR[r] = 1;
    convOut[ins[0]].push_back(outs[0]);
    convIn[outs[0]].push_back(ins[0]);
  }
}

// The candidate walk's scratch. `seen` and `inReach` are reused across seeds,
// while `component` and `reach` are refilled for each one. Held in one struct
// so that the recursions below take a reference rather than a handful of
// parallel arrays.
struct CandidateWalk {
  aw::vector<uint8_t> seen;      // nReal, 0/1: component flood fill
  aw::vector<uint8_t> inReach;   // nReal, 0/1: downstream closure of the base
  aw::vector<ItemId> component;  // the seed's weakly connected component
  aw::vector<ItemId> reach;      // everything the base converts into
};

// Marks `u` and everything reachable from it through conversion edges in either
// direction: the seed's weakly connected component. A plain recursion, run on
// the large stack by the caller.
void collectComponent(const VariantFoldRun &run, CandidateWalk &walk,
                      ItemId u) noexcept {
  walk.seen[u] = 1;
  walk.component.push_back(u);
  for (ItemId v : run.convOut[u])
    if (!walk.seen[v])
      collectComponent(run, walk, v);
  for (ItemId v : run.convIn[u])
    if (!walk.seen[v])
      collectComponent(run, walk, v);
}

// Marks the downstream closure of the base under the conversion edges: every
// item it can be converted into, the base itself excluded. A plain recursion,
// run on the large stack by the caller.
void collectReach(const VariantFoldRun &run, CandidateWalk &walk,
                  ItemId u) noexcept {
  for (ItemId v : run.convOut[u]) {
    if (walk.inReach[v])
      continue;
    walk.inReach[v] = 1;
    walk.reach.push_back(v);
    collectReach(run, walk, v);
  }
}

// One seed at a time: its conversion component, the component's base, and the
// twins that base reaches. `comp` numbers the components in seed order.
void collectCandidates(VariantFoldRun &run, CandidateWalk &walk,
                       std::span<const uint8_t> hasStock) noexcept {
  uint32_t seedIndex = 0;

  for (ItemId seed = 0; seed < run.nReal; seed++) {
    if (walk.seen[seed] ||
        (run.convOut[seed].empty() && run.convIn[seed].empty()))
      continue;
    // Weakly connected component of the conversion graph.
    walk.component.clear();
    collectComponent(run, walk, seed);
    if (walk.component.size() < 2)
      continue;
    for (ItemId u : walk.component)
      run.comp[u] = seedIndex;
    seedIndex++;

    ItemId base = walk.component[0];
    for (ItemId u : walk.component) {
      const auto key =
          std::make_pair(run.demand[u], (uint32_t) run.memberTags[u].size());
      const auto best =
          std::make_pair(run.demand[base], (uint32_t) run.memberTags[base].size());
      if (key > best || (key == best && u < base))
        base = u;
    }
    if (run.demand[base] == 0)
      continue;

    // Downstream closure from the base: everything the base can be converted
    // into is a decorative twin of it.
    walk.reach.clear();
    walk.inReach[base] = 1;
    collectReach(run, walk, base);
    for (ItemId u : walk.reach) {
      walk.inReach[u] = 0;
      if (u == run.target)
        continue;
      if (u < hasStock.size() && hasStock[u])
        continue;
      // Orientation: the source must live in strictly fewer tags than the
      // base, and at least one of those tags must really be consumed by a
      // recipe. Without the amt half the material-form cycles (a dust and
      // an ingot that share an unused tag) would be folded too.
      if (!std::includes(run.memberTags[base].begin(), run.memberTags[base].end(),
                         run.memberTags[u].begin(), run.memberTags[u].end()) ||
          run.memberTags[u].size() >= run.memberTags[base].size())
        continue;
      bool real = false;
      for (ItemId t : run.memberTags[u])
        if (run.tagHasRealConsumer[t]) {
          real = true;
          break;
        }
      if (!real)
        continue;
      run.sigma[u] = base;
      run.candidates.push_back(u);
    }
    walk.inReach[base] = 0;
  }
}

bool VariantFoldRun::buildCandidates(std::span<const uint8_t> hasStock) noexcept {
  for (ItemId i = 0; i < nReal; i++) {
    sigma[i] = i;
    if (i < hasStock.size())
      stocked[i] = hasStock[i];
  }

  // How many non-conversion real recipes demand each item: the anchor. A
  // component without one is symmetric, so no member is singled out as the
  // base and the component is left alone.
  for (uint r = 0; r < nRealRecipe; r++) {
    if (neutralR[r])
      continue;
    for (ItemId in : g.inputsOf(r))
      if (in < nReal)
        demand[in]++;
  }

  CandidateWalk walk;
  walk.seen.assign(nReal, 0);
  walk.inReach.assign(nReal, 0);
  // Entered on the large stack: the walks above are as deep as the conversion
  // graph is long.
  aw::ls::call(collectCandidates, *this, walk, hasStock);
  return !candidates.empty();
}

// Bucket-sort all D-free recipes according to the smallest output handle.
void VariantFoldRun::buildWitnessIndex() noexcept {
  alive.clear();
  touching.clear();
  witnessOffsets.assign(nItem + 1, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (touchesFold(r)) {
      touching.push_back_unchecked(r);
      continue;
    }
    alive.push_back_unchecked(r);
    witnessOffsets[g.outputsOf(r)[0] + 1]++;
  }

  for (uint i = 0; i < nItem; i++)
    witnessOffsets[i + 1] += witnessOffsets[i];
  witnessCursor.assign(witnessOffsets.begin(), witnessOffsets.end() - 1);
  witnessRecipes.resize(alive.size());
  for (uint r : alive)
    witnessRecipes[witnessCursor[g.outputsOf(r)[0]]++] = r;
}

void VariantFoldRun::buildShapeIndex() noexcept {
  byShape.clear();
  byShape.reserve(nRecipe * 2);
  aw::vector<Amount> amounts;
  for (uint r = 0; r < nRecipe; r++) {
    uint64_t h = 1469598103934665603ull;
    amounts.clear();
    for (size_t k = 0; k < g.outputsOf(r).size(); k++)
      amounts.push_back(g.outputAmountsOf(r)[k]);
    std::sort(amounts.begin(), amounts.end());
    for (Amount a : amounts)
      h = mixRow(h, 0, a);
    amounts.clear();
    for (size_t k = 0; k < g.inputsOf(r).size(); k++)
      amounts.push_back(g.inputAmountsOf(r)[k]);
    std::sort(amounts.begin(), amounts.end());
    for (Amount a : amounts)
      h = mixRow(h, 1, a);
    byShape[h].push_back(r);
  }
}

bool VariantFoldRun::proposeFold(ItemId from, ItemId to) noexcept {
  if (from >= nReal || to >= nReal || from == to)
    return false;
  if (to == target || stocked[from])
    return false;
  if (rejected[from])
    return false;
  if (candidates.size() >= options.variantFold.maxFoldItems)
    return false;
  if (sigma[from] != from)
    return false;
  sigma[from] = to;
  candidates.push_back(from);
  grew = true;
  return true;
}

// Grows the fold so that r's projected column can be replayed: either a single
// consumed member cancels against the single produced one, or a recipe of the
// same shape exists once the produced twins are folded onto its outputs.
bool VariantFoldRun::tryGrow(uint r) noexcept {
  if (!projectedColumn(r))
    return false;

  // The whole column is one member against another: folding the consumed one
  // onto the produced one makes it empty.
  if (posScratch.size() == 1 && needScratch.size() == 1 &&
      posScratch[0].amt == needScratch[0].amt) {
    const ItemId to = posScratch[0].item;
    const ItemId from = needScratch[0].item;
    if (from < nReal && to < nReal && comp[from] == comp[to] &&
        comp[from] != UINT32_MAX &&
        std::ranges::includes(memberTags[to], memberTags[from]))
      return proposeFold(from, to);
  }

  // A recipe with the same output amounts and the same required inputs, whose
  // differing outputs are exactly the unfolded twins of pi's outputs.
  uint64_t h = 1469598103934665603ull;
  aw::vector<Amount> shapeAmounts;
  for (const auto &row : posScratch)
    shapeAmounts.push_back(row.amt);
  std::sort(shapeAmounts.begin(), shapeAmounts.end());
  for (Amount a : shapeAmounts)
    h = mixRow(h, 0, a);
  shapeAmounts.clear();
  for (const auto &row : needScratch)
    shapeAmounts.push_back(row.amt);
  std::sort(shapeAmounts.begin(), shapeAmounts.end());
  for (Amount a : shapeAmounts)
    h = mixRow(h, 1, a);
  const auto it = byShape.find(h);
  if (it == byShape.end())
    return false;

  for (uint witness : it->second) {
    if (g.cost[witness] > g.cost[r])
      continue;
    const auto wOuts = g.outputsOf(witness);
    const auto wIns = g.inputsOf(witness);
    if (wOuts.size() != posScratch.size() || wIns.size() != needScratch.size())
      continue;
    aw::vector<std::pair<ItemId, ItemId>> proposals;
    usedScratch.assign(wIns.size(), 0);
    const bool inOk = matchInputsGrow(*this, needScratch, wIns,
                                      g.inputAmountsOf(witness), 0, usedScratch,
                                      proposals);
    if (!inOk)
      continue;

    // Pair the two output lists by amount, then require every pair to match or
    // to be a fold of pi's item onto the witness's item.
    aw::vector<uint8_t> taken(wOuts.size(), 0);
    bool ok = true;
    for (const auto &[p, amount] : posScratch) {
      bool found = false;
      for (size_t j = 0; j < wOuts.size(); j++) {
        if (taken[j] || g.outputAmountsOf(witness)[j] != amount)
          continue;
        const ItemId q = wOuts[j];
        if (p == q) {
          taken[j] = 1;
          found = true;
          break;
        }
        if (p >= nReal || q >= nReal || sigma[p] != p || sigma[q] != q)
          continue;
        if (comp[p] == comp[q] && comp[p] != UINT32_MAX && demand[q] >= demand[p] &&
            std::includes(memberTags[q].begin(), memberTags[q].end(),
                          memberTags[p].begin(), memberTags[p].end())) {
          taken[j] = 1;
          proposals.emplace_back(p, q);
          found = true;
          break;
        }
      }
      if (!found) {
        ok = false;
        break;
      }
    }
    if (!ok || proposals.empty())
      continue;
    bool any = false;
    for (const auto &[from, to] : proposals)
      any |= proposeFold(from, to);
    if (any)
      return true;
  }
  return false;
}

// Computes the `pi` projection in `docs/algorithm.typ`.
// `posScratch` gives output and `needScratch` gives input.
bool VariantFoldRun::projectedColumn(uint r) noexcept {
  posScratch.clear();
  needScratch.clear();
  touched.clear();
  const auto outs = g.outputsOf(r);
  const auto outAmts = g.outputAmountsOf(r);
  for (size_t k = 0; k < outs.size(); k++) {
    const ItemId x = rep(outs[k]);
    if (!accumSet[x]) {
      accumSet[x] = 1;
      touched.push_back(x);
    }
    accum[x] += outAmts[k];
  }
  const auto ins = g.inputsOf(r);
  const auto inAmts = g.inputAmountsOf(r);
  for (size_t k = 0; k < ins.size(); k++) {
    const ItemId x = rep(ins[k]);
    if (!accumSet[x]) {
      accumSet[x] = 1;
      touched.push_back(x);
    }
    accum[x] -= inAmts[k];
  }
  bool any = false;
  for (ItemId x : touched) {
    const Amount a = accum[x];
    accum[x] = 0;
    accumSet[x] = 0;
    if (a > 0) {
      posScratch.emplace_back(x, a);
      any = true;
    } else if (a < 0) {
      needScratch.emplace_back(x, -a);
      any = true;
    }
  }
  std::sort(posScratch.begin(), posScratch.end());
  std::sort(needScratch.begin(), needScratch.end());
  return any;
}

// True when `node` is a tag that contains `member`.
[[nodiscard]]
static bool tagContains(const VariantFoldRun &run, ItemId node,
                        ItemId member) noexcept {
  if (node < run.nReal || member >= run.nReal)
    return false;
  const auto &tags = run.memberTags[member];
  return std::binary_search(tags.begin(), tags.end(), node);
}

// Matches the required inputs against the witness inputs, allowing a required
// item to be fed through any tag that contains it. Recipes are tiny, so a plain
// backtracking search is enough.
//
// In the lemma, r^* can be any set of recipes. But here we specialize it to
// a single recipe + optional tag forwarding.
static bool matchInputs(const VariantFoldRun &run,
                        std::span<const ItemEntry> required,
                        std::span<const ItemId> ins,
                        std::span<const Amount> inAmts, size_t index,
                        aw::vector<uint8_t> &used) noexcept {
  if (index == required.size())
    return true;
  const auto [x, a] = required[index];
  for (size_t j = 0; j < ins.size(); j++) {
    if (used[j] || inAmts[j] != a)
      continue;
    if (ins[j] == x || tagContains(run, ins[j], x)) {
      used[j] = 1;
      if (matchInputs(run, required, ins, inAmts, index + 1, used))
        return true;
      used[j] = 0;
    }
  }
  return false;
}

static bool matchInputsGrow(VariantFoldRun &run,
                            std::span<const ItemEntry> required,
                            std::span<const ItemId> ins,
                            std::span<const Amount> inAmts, size_t index,
                            aw::vector<uint8_t> &used,
                            aw::vector<std::pair<ItemId, ItemId>> &proposals) noexcept {
  if (index == required.size())
    return true;
  const auto [x, a] = required[index];
  for (size_t j = 0; j < ins.size(); j++) {
    if (used[j] || inAmts[j] != a)
      continue;
    const ItemId y = ins[j];
    if (y == x || tagContains(run, y, x)) {
      used[j] = 1;
      if (matchInputsGrow(run, required, ins, inAmts, index + 1, used, proposals))
        return true;
      used[j] = 0;
    } else if (x < run.nReal && y < run.nReal && run.sigma[x] == x &&
               run.sigma[y] == y && run.comp[x] == run.comp[y] &&
               run.comp[x] != UINT32_MAX && run.demand[y] >= run.demand[x] &&
               std::includes(run.memberTags[y].begin(), run.memberTags[y].end(),
                             run.memberTags[x].begin(), run.memberTags[x].end())) {
      used[j] = 1;
      proposals.emplace_back(x, y);
      if (matchInputsGrow(run, required, ins, inAmts, index + 1, used, proposals))
        return true;
      proposals.pop_back();
      used[j] = 0;
    }
  }
  return false;
}

// Checks whether the lemma's condition holds: A r^* = pi(A e_r) and c_(r^*) <= c_r.
bool VariantFoldRun::columnRealizable(uint r, Amount cost) noexcept {
  // A recipe always outputs something, so a column that only consumes can
  // never be replayed.
  if (posScratch.empty())
    return false;

  // Filters out recipes that can possibly match.
  for (uint witness : witnessesOf(posScratch[0].item)) {
    if (g.cost[witness] > cost)
      continue;
    const auto wOuts = g.outputsOf(witness);
    if (wOuts.size() != posScratch.size())
      continue;

    // Item ID and output amount must match exactly.
    bool same = true;
    for (size_t k = 0; k < wOuts.size(); k++) {
      if (wOuts[k] != posScratch[k].item ||
          g.outputAmountsOf(witness)[k] != posScratch[k].amt) {
        same = false;
        break;
      }
    }
    if (!same)
      continue;
    const auto wIns = g.inputsOf(witness);
    if (wIns.size() != needScratch.size())
      continue;
    usedScratch.assign(wIns.size(), 0);
    if (matchInputs(*this, needScratch, wIns, g.inputAmountsOf(witness), 0,
                    usedScratch))
      return true;
  }
  return false;
}

void VariantFoldRun::collectFailures() noexcept {
  failing.clear();
  // `touching` was filled by the buildWitnessIndex call that precedes every
  // call to this one, so the fold does not have to be scanned again.
  for (uint r : touching) {
    if (!projectedColumn(r))
      continue;
    if (!columnRealizable(r, g.cost[r]))
      failing.push_back(r);
  }
}

bool VariantFoldRun::verifyAndRepair() noexcept {
  for (;;) {
    buildWitnessIndex();
    collectFailures();
    if (failing.empty())
      return true;
    if (!timeLeft())
      return false;

    // First try to make a failing recipe realizable by folding more twins.
    grew = false;
    for (uint r : failing) {
      if (tryGrow(r))
        break;
      if (!timeLeft())
        break;
    }
    if (grew)
      continue;

    // No growth can fix the failures: blame the folded item that appears in
    // the most of them and unfold it. Unfolding only turns recipes into kept
    // ones, so the fold strictly shrinks and the loop terminates.
    std::unordered_map<ItemId, uint32_t> blame;
    for (uint r : failing) {
      for (ItemId in : g.inputsOf(r))
        if (in < nReal && sigma[in] != in)
          blame[in]++;
      for (ItemId o : g.outputsOf(r))
        if (o < nReal && sigma[o] != o)
          blame[o]++;
    }
    if (blame.empty())
      return false;
    ItemId victim = kNoItem;
    uint32_t best = 0;
    for (const auto &[item, count] : blame)
      if (count > best || (count == best && item < victim)) {
        best = count;
        victim = item;
      }
    sigma[victim] = victim;
    rejected[victim] = 1;
    candidates.erase(std::remove(candidates.begin(), candidates.end(), victim),
                     candidates.end());
    if (candidates.empty())
      return false;
  }
}

}  // namespace

bool computeVariantFoldPruning(QUERY_PRUNE_PARAM_LIST) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  if (!options.variantFold.enabled)
    return false;
  if (target >= g.nReal || drop.size() != g.nRecipe || g.nItem == g.nReal)
    return false;

  const auto started = std::chrono::steady_clock::now();
  VariantFoldRun run(sub, target, started);
  run.buildTagIndex();
  run.buildConversions();

  // The inventory is indexed in source nodes; the fold works on subgraph ids.
  aw::vector<uint8_t> hasStock(g.nItem, 0);
  for (ItemId i = 0; i < g.nItem; i++) {
    const ItemId source = sub.itemOrigin[i];
    if (source < sourceInventory.size() && sourceInventory[source] > 0)
      hasStock[i] = 1;
  }
  if (!run.buildCandidates(hasStock))
    return false;
  if ((uint32_t) run.candidates.size() > options.variantFold.maxFoldItems)
    return false;
  run.buildShapeIndex();


  if (!run.verifyAndRepair())
    return false;

  // Mark every recipe that mentions a folded item. The projection lemma
  // guarantees the pruned subgraph keeps the optimum.
  bool any = false;
  for (uint r = 0; r < g.nRecipe; r++) {
    if (run.touchesFold(r)) {
      drop[r] = 1;
      any = true;
    }
  }
  return any;
}

}  // namespace aw
