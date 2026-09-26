// Dominance pruning for the canonical crafting graph.
//
// Two independent passes run once, at registration time:
//
//   1. Tag-edge pruning deletes synthetic `T <- m` edges whose member m is
//      cost-dominated by another member of T. It only reasons about real
//      recipes that consume the same resource they produce.
//
//   2. Real-recipe (composite) pruning deletes a real recipe R: X <- Y, ...
//      when a sibling recipe S of X is provably at least as good after
//      inlining every producer of Y into R. The comparison is a sparse column
//      vector inequality, see docs/algorithm.typ ("基于支配的剪枝：真实配方篇").
//      S may need a workstation R does not, so that condition is checked at
//      query time instead of being folded into the preprocessing.
//
// Both passes can remove an edge that the player might usefully keep, because
// a stocked input can always be spent directly, and a recipe whose replacement
// needs an unavailable workstation must survive. Each pass therefore records
// the input the certificate relied on, and the recipe pass additionally records
// the workstations of the replacements, so reachableSubgraph can keep a recipe
// whose guard is in the inventory or whose replacement cannot be run.

#include "Prune.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "aw/CraftingGraph.h"

namespace aw {
namespace {

using uint = uint32_t;

// Both passes are on by default. The setters exist so the command line tool
// and the unit tests can A/B a plan against the unpruned graph.
bool tagPruning = true;
bool recipePruning = true;

// ---------------------------------------------------------------------------
// Budgets
// ---------------------------------------------------------------------------
// Both passes build a relation that is quadratic in the width of the graph.
// What is fine for a vanilla-sized pack is not fine for a 561-mod pack: ATM10
// ships two "catch-all" ingredients with 63k and 45k members, and items with
// tens of thousands of recipes. Left unbounded, the tag pass alone asks for
// ~6e9 pairs (~48 GB) on that pack.
//
// Every budget is conservative: running out of it means a tag is left out or
// an item is left unpruned, so the pass prunes less. Nothing here can mark an
// edge dominated that is not, because dominance is only ever accepted with a
// complete justification. The defaults are chosen to leave the vanilla, small
// and nast test graphs unchanged.

// A tag with more members than this is left out of the dominance relation. Its
// self cross product is what explodes first, and a tag this wide is a
// catch-all whose "every other member dominates w" gate can never be met.
constexpr size_t MAX_TAG_MEMBERS = 1024;

// Ceiling on the sum of |M|^2 over the tags that do take part. |M|^2 covers
// both the support pairs loop (b) pushes and the adjacency the SCC stage scans,
// so this bounds the whole per-tag cost. Tags are admitted smallest first, so
// the budget buys as many of them as possible.
constexpr uint64_t MAX_TAG_PAIRS = 4'000'000;

// Ceiling on the gating pairs loop (a) contributes. Truncating it only drops
// (m, w) candidates, which can only make `valid` reject more pairs.
constexpr uint64_t MAX_WITNESS_PAIRS = 4'000'000;

// A real item with more recipes than this is left unpruned. The composite pass
// compares every pair of an item's recipes; ATM10 has items with 35k recipes,
// where comparing all pairs is both quadratic in time and gigabytes of
// adjacency. Keeping recipes can only cost the planner time, never correctness.
constexpr size_t MAX_SIBLING_RECIPES = 2048;

// A witness input produced by more recipes than this is skipped, because the
// composite test has to hold for every producer of the witness. Skipping a
// witness just leaves its recipe without a guard. ATM10 has 8 items above this
// (up to 35k producers); the test graphs top out at 1985.
constexpr size_t MAX_WITNESS_PRODUCERS = 2048;

void prefixSum(std::vector<uint>& v) noexcept {
  v.insert(v.begin(), 0);
  for (size_t i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
}

// ---------------------------------------------------------------------------
// Sparse column vectors
// ---------------------------------------------------------------------------
// A recipe is a column vector over item rows: +outputAmt at its output row and
// -inputAmt at every input row. Equal rows are summed, so a recipe that eats
// its own output collapses that row. The vectors are stored as a CSR matrix
// sorted by item, which makes the componentwise comparison a linear merge.

struct RecipeVectors {
  std::vector<uint> offsets;  // nRecipe + 1
  std::vector<NodeId> items;  // nnz
  std::vector<Amount> coeffs;  // nnz

  [[nodiscard]]
  std::span<const NodeId> itemsOf(uint r) const noexcept {
    return {items.data() + offsets[r], items.data() + offsets[r + 1]};
  }

  [[nodiscard]]
  std::span<const Amount> coeffsOf(uint r) const noexcept {
    return {coeffs.data() + offsets[r], coeffs.data() + offsets[r + 1]};
  }
};

void buildRecipeVectors(const CraftingGraph& graph, RecipeVectors& out) noexcept {
  const uint nRecipe = graph.nRecipe;
  out.offsets.assign(nRecipe + 1, 0);
  std::vector<std::pair<NodeId, Amount>> scratch;
  for (uint r = 0; r < nRecipe; r++) {
    scratch.clear();
    scratch.emplace_back(graph.output[r], graph.outputAmt[r]);
    const auto inputs = graph.r2i.targetsOf(r);
    const auto weights = graph.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      scratch.emplace_back(inputs[k], -weights[k]);

    std::sort(scratch.begin(), scratch.end(),
              [](const auto& a, const auto& b) noexcept { return a.first < b.first; });

    // Sum equal rows and drop zero coefficients.
    for (size_t k = 0; k < scratch.size();) {
      const NodeId item = scratch[k].first;
      Amount coeff = 0;
      while (k < scratch.size() && scratch[k].first == item) {
        coeff += scratch[k].second;
        k++;
      }
      if (coeff != 0) {
        out.items.push_back(item);
        out.coeffs.push_back(coeff);
      }
    }
    out.offsets[r + 1] = (uint) out.items.size();
  }
}

// Returns true when a * b does not fit in int64_t. `out` is only written when
// it fits.
bool mulOverflow(int64_t a, int64_t b, int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  const bool overflow = a > 0
                            ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
                            : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b);
  if (overflow)
    return true;
  out = a * b;
  return false;
}

bool addOverflow(int64_t a, int64_t b, int64_t& out) noexcept {
  if (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)
    return true;
  out = a + b;
  return false;
}

// Builds `c = alpha * v_r + v_R`, sorted and unique, into `outItems` / `outCoeffs`.
// Returns false on integer overflow, which is treated as "cannot prove
// dominance" by the caller.
bool buildComposite(int64_t alpha, const RecipeVectors& vec, uint r, uint R,
                    std::vector<NodeId>& outItems,
                    std::vector<Amount>& outCoeffs) noexcept {
  const auto ri = vec.itemsOf(r);
  const auto rc = vec.coeffsOf(r);
  const auto Ri = vec.itemsOf(R);
  const auto Rc = vec.coeffsOf(R);
  outItems.clear();
  outCoeffs.clear();

  size_t i = 0, j = 0;
  while (i < ri.size() || j < Ri.size()) {
    NodeId next = UINT32_MAX;
    if (i < ri.size())
      next = std::min(next, ri[i]);
    if (j < Ri.size())
      next = std::min(next, Ri[j]);

    Amount coeff = 0;
    if (i < ri.size() && ri[i] == next) {
      int64_t scaled = 0;
      if (mulOverflow(alpha, rc[i], scaled))
        return false;
      coeff = scaled;
      i++;
    }
    if (j < Ri.size() && Ri[j] == next) {
      int64_t sum = 0;
      if (addOverflow(coeff, Rc[j], sum))
        return false;
      coeff = sum;
      j++;
    }
    if (coeff != 0) {
      outItems.push_back(next);
      outCoeffs.push_back(coeff);
    }
  }
  return true;
}

// True when c <= vS componentwise over the union of the two supports. A key
// present only in vS compares against 0, which is what makes a right-only
// negative entry (an input the composite does not mention) fail the test.
bool leVector(std::span<const NodeId> ci, std::span<const Amount> cc,
              std::span<const NodeId> si, std::span<const Amount> sc) noexcept {
  size_t i = 0, j = 0;
  while (i < ci.size() || j < si.size()) {
    NodeId next = UINT32_MAX;
    if (i < ci.size())
      next = std::min(next, ci[i]);
    if (j < si.size())
      next = std::min(next, si[j]);

    const Amount c = (i < ci.size() && ci[i] == next) ? cc[i++] : 0;
    const Amount s = (j < si.size() && si[j] == next) ? sc[j++] : 0;
    if (c > s)
      return false;
  }
  return true;
}

bool leZero(std::span<const Amount> cc) noexcept {
  for (Amount c : cc)
    if (c > 0)
      return false;
  return true;
}

// ---------------------------------------------------------------------------
// Sink SCC representatives
// ---------------------------------------------------------------------------
// Marks one representative per sink SCC of a directed graph on `k` nodes.
// `adjOffsets` has k + 1 entries and `adjTargets` has adjOffsets[k] entries.
// `comp` is filled with the component id of every node and `repOfComp` with the
// representative of every sink component (UINT32_MAX for a non-sink one).
// Components are numbered in reverse topological order: an edge to a different
// component always points at a smaller id. `keep[a] == 1` exactly for the
// representatives. Every node with no outgoing edge is its own sink SCC, so the
// result is never empty for k > 0. Returns the number of components.
uint32_t markSinkRepresentatives(uint k, const std::vector<uint>& adjOffsets,
                                 const std::vector<uint32_t>& adjTargets,
                                 std::vector<int32_t>& comp,
                                 std::vector<uint32_t>& repOfComp,
                                 std::vector<uint8_t>& keep) noexcept {
  keep.assign(k, 0);
  comp.assign(k, -1);
  repOfComp.clear();
  if (k == 0)
    return 0;

  // Iterative Tarjan, so a deep graph cannot overflow the stack.
  std::vector<int32_t> disc(k, -1), low(k, 0);
  std::vector<uint8_t> onStack(k, 0);
  std::vector<uint32_t> tstack, callNode, callEdge;
  int32_t timer = 0;
  uint32_t nComp = 0;
  for (uint32_t s = 0; s < k; s++) {
    if (disc[s] != -1)
      continue;
    disc[s] = low[s] = timer++;
    tstack.push_back(s);
    onStack[s] = 1;
    callNode.push_back(s);
    callEdge.push_back(adjOffsets[s]);
    while (!callNode.empty()) {
      const uint32_t v = callNode.back();
      uint32_t& edge = callEdge.back();
      if (edge < adjOffsets[v + 1]) {
        const uint32_t u = adjTargets[edge++];
        if (disc[u] == -1) {
          disc[u] = low[u] = timer++;
          tstack.push_back(u);
          onStack[u] = 1;
          callNode.push_back(u);
          callEdge.push_back(adjOffsets[u]);
        } else if (onStack[u] && disc[u] < low[v]) {
          low[v] = disc[u];
        }
      } else {
        if (low[v] == disc[v]) {
          while (true) {
            const uint32_t u = tstack.back();
            tstack.pop_back();
            onStack[u] = 0;
            comp[u] = (int32_t) nComp;
            if (u == v)
              break;
          }
          nComp++;
        }
        callNode.pop_back();
        callEdge.pop_back();
        if (!callNode.empty()) {
          const uint32_t parent = callNode.back();
          if (low[v] < low[parent])
            low[parent] = low[v];
        }
      }
    }
  }

  std::vector<uint8_t> isSink(nComp, 1);
  for (uint32_t a = 0; a < k; a++)
    for (uint32_t e = adjOffsets[a]; e < adjOffsets[a + 1]; e++) {
      const uint32_t b = adjTargets[e];
      if (comp[a] != comp[b])
        isSink[comp[a]] = 0;
    }
  repOfComp.assign(nComp, UINT32_MAX);
  for (uint32_t a = 0; a < k; a++) {
    const uint32_t c = (uint32_t) comp[a];
    if (isSink[c] && repOfComp[c] == UINT32_MAX)
      repOfComp[c] = a;
  }
  for (uint32_t a = 0; a < k; a++)
    if (repOfComp[comp[a]] == a)
      keep[a] = 1;
  return nComp;
}

// ---------------------------------------------------------------------------
// Tag-member dominance pruning
// ---------------------------------------------------------------------------
// A tag (pseudo-resource) T is a set of members, encoded as one synthetic
// `T <- m` recipe per member. Serving T through a member m is pointless when
// another member w is strictly cheaper, because then the tag edge `T <- m` can
// be replaced by `T <- w`. The relation used here is `requires(m, w)`: for
// every recipe r of m there is an amount-qualified input that is either w
// itself or a simple tag all of whose other producible members require w. It
// implies the per-unit cost of m exceeds that of w. See docs/algorithm.typ.
//
// A tag is "simple" when every one of its recipes is a member edge: exactly one
// input, no workstation, and a real member. Everything the Java writer emits
// looks like that. A malformed-but-parseable tag is left unpruned, which is
// conservative.

using PairKey = uint64_t;

constexpr PairKey packPair(NodeId m, NodeId w) noexcept {
  return ((PairKey) m << 32) | (PairKey) w;
}

void intersectSorted(const std::vector<NodeId>& a, const std::vector<NodeId>& b,
                     std::vector<NodeId>& out) noexcept {
  out.clear();
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] < b[j]) {
      i++;
    } else if (b[j] < a[i]) {
      j++;
    } else {
      out.push_back(a[i]);
      i++;
      j++;
    }
  }
}

// Fills graph.tagEdgeDominated. Never fails; on malformed input it simply
// prunes less.
void computeTagPruning(CraftingGraph& graph) noexcept {
  graph.tagEdgeDominated.assign(graph.nRecipe, 0);

  const uint nItem = graph.nItem;
  const uint nReal = graph.nReal;
  const uint nRecipe = graph.nRecipe;
  if (nItem == 0 || nReal == 0 || nRecipe == 0)
    return;

  // ---- Simple tags and their members ------------------------------------
  std::vector<uint8_t> simpleTag(nItem, 0);
  std::vector<std::vector<NodeId>> members(nItem);
  for (NodeId t = nReal; t < nItem; t++) {
    const auto recipes = graph.i2r.targetsOf(t);
    if (recipes.empty())
      continue;

    std::vector<NodeId> ms;
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
      ms.push_back(inputs[0]);
    }
    if (!simple)
      continue;

    std::sort(ms.begin(), ms.end());
    ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
    simpleTag[t] = 1;
    members[t] = std::move(ms);
  }

  std::vector<uint8_t> producible(nItem, 0);
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
    std::vector<uint32_t> order;
    order.reserve(nItem);
    for (NodeId t = nReal; t < nItem; t++)
      if (simpleTag[t])
        order.push_back(t);
    std::sort(order.begin(), order.end(), [&members](uint32_t a, uint32_t b) noexcept {
      if (members[a].size() != members[b].size())
        return members[a].size() < members[b].size();
      return a < b;
    });

    uint64_t used = 0;
    for (uint32_t t : order) {
      const uint64_t k = members[t].size();
      const uint64_t cost = k * k;
      if (k > MAX_TAG_MEMBERS || cost > MAX_TAG_PAIRS - used) {
        simpleTag[t] = 0;
        continue;
      }
      used += cost;
    }
  }

  // member -> tags containing it, ascending.
  std::vector<uint> itemTagOffsets(nReal, 0);
  for (NodeId t = nReal; t < nItem; t++)
    if (simpleTag[t])
      for (NodeId m : members[t])
        itemTagOffsets[m]++;
  prefixSum(itemTagOffsets);
  std::vector<NodeId> itemTagTargets(itemTagOffsets.back());
  {
    std::vector<uint> cursor(itemTagOffsets.begin(), itemTagOffsets.end() - 1);
    for (NodeId t = nReal; t < nItem; t++)
      if (simpleTag[t])
        for (NodeId m : members[t])
          itemTagTargets[cursor[m]++] = t;
  }

  // tag -> real items that consume it in a real recipe. Duplicates are kept;
  // the worklist dedups them.
  std::vector<uint> tagConsumerOffsets(nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.output[r] >= nReal)
      continue;
    for (NodeId j : graph.r2i.targetsOf(r))
      if (j >= nReal && j < nItem && simpleTag[j])
        tagConsumerOffsets[j]++;
  }
  prefixSum(tagConsumerOffsets);
  std::vector<NodeId> tagConsumerTargets(tagConsumerOffsets.back());
  {
    std::vector<uint> cursor(tagConsumerOffsets.begin(), tagConsumerOffsets.end() - 1);
    for (uint r = 0; r < nRecipe; r++) {
      if (graph.output[r] >= nReal)
        continue;
      const NodeId m = graph.output[r];
      for (NodeId j : graph.r2i.targetsOf(r))
        if (j >= nReal && j < nItem && simpleTag[j])
          tagConsumerTargets[cursor[j]++] = m;
    }
  }

  // ---- The universe of candidate pairs ----------------------------------
  // (a) witnesses for gating: w in the intersection of the amount-qualified
  //     input sources over every recipe of m.
  // (b) support pairs (z, w) for members of a common tag, so that the "all
  //     other members require w" rule has something to count.
  std::vector<PairKey> pairs;
  {
    std::vector<NodeId> acc, next, scratch;
    uint64_t witnessPairs = 0;
    for (NodeId m = 0; m < nReal && witnessPairs < MAX_WITNESS_PAIRS; m++) {
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
          intersectSorted(acc, scratch, next);
          acc.swap(next);
        }
        if (acc.empty())
          break;
      }
      for (NodeId w : acc) {
        if (w == m)
          continue;
        pairs.push_back(packPair(m, w));
        if (++witnessPairs >= MAX_WITNESS_PAIRS)
          break;
      }
    }
  }
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const std::vector<NodeId>& ms = members[t];
    for (NodeId z : ms) {
      if (!producible[z])
        continue;
      for (NodeId w : ms)
        if (z != w)
          pairs.push_back(packPair(z, w));
    }
  }

  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

  const uint32_t universe = (uint32_t) pairs.size();
  if (universe == 0)
    return;

  // Group the universe by witness. Sorting by (m, w) means a single scan fills
  // each witness row in ascending m, so idOf can binary search the row.
  std::vector<uint> byWOffsets(nItem, 0);
  for (PairKey key : pairs)
    byWOffsets[(uint32_t) (key & 0xFFFFFFFFu)]++;
  prefixSum(byWOffsets);
  std::vector<NodeId> byWTargets(universe);
  std::vector<NodeId> pairW(universe);
  {
    std::vector<uint> cursor(byWOffsets.begin(), byWOffsets.end() - 1);
    for (PairKey key : pairs) {
      const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
      const NodeId m = (NodeId) (key >> 32);
      const uint slot = cursor[w]++;
      byWTargets[slot] = m;
      pairW[slot] = w;
    }
  }
  pairs.clear();
  pairs.shrink_to_fit();

  auto idOf = [&](NodeId m, NodeId w) -> uint32_t {
    const uint lo = byWOffsets[w], hi = byWOffsets[w + 1];
    const auto begin = byWTargets.begin() + lo;
    const auto end = byWTargets.begin() + hi;
    const auto it = std::lower_bound(begin, end, m);
    if (it == end || *it != m)
      return UINT32_MAX;
    return (uint32_t) (it - byWTargets.begin());
  };

  // ---- Counters for the tag rule ----------------------------------------
  // cnt[t][i] counts members z != M[i] that currently require M[i]. The tag may
  // gate through M[i] exactly when the counter reaches |M| - 1; a member with
  // no recipe contributes nothing, so a leaf member always blocks the gate.
  std::vector<std::vector<uint32_t>> cnt(nItem);
  std::vector<uint32_t> threshold(nItem, 0);
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const std::vector<NodeId>& ms = members[t];
    cnt[t].assign(ms.size(), 0);
    threshold[t] = (uint32_t) ms.size() - 1;
    for (size_t idx = 0; idx < ms.size(); idx++) {
      uint32_t c = 0;
      for (size_t k = 0; k < ms.size(); k++)
        if (k != idx && producible[ms[k]])
          c++;
      cnt[t][idx] = c;
    }
  }

  auto valid = [&](NodeId m, NodeId w) -> bool {
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
        if (j >= nReal && j < nItem && simpleTag[j]) {
          const std::vector<NodeId>& ms = members[j];
          const auto it = std::lower_bound(ms.begin(), ms.end(), w);
          if (it != ms.end() && *it == w) {
            const size_t idx = (size_t) (it - ms.begin());
            if (cnt[j][idx] == threshold[j]) {
              ok = true;
              break;
            }
          }
        }
      }
      if (!ok)
        return false;
    }
    return true;
  };

  // ---- Greatest fixpoint ------------------------------------------------
  // Every pair starts alive and is killed once when it loses its justification.
  // A kill can only invalidate pairs that gate through a shared tag, so those
  // consumers are re-queued; the result is the greatest fixpoint.
  std::vector<uint8_t> alive(universe, 1);
  std::vector<uint32_t> queue(universe);
  std::vector<uint8_t> queued(universe, 0);
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

    const NodeId m = byWTargets[id];
    const NodeId w = pairW[id];
    if (valid(m, w))
      continue;

    // Kill (m, w). Every tag containing both loses one supporting member.
    // m and w are always real here: the universe is built from real items.
    alive[id] = 0;
    const auto ta = std::span(itemTagTargets.data() + itemTagOffsets[m],
                              itemTagOffsets[m + 1] - itemTagOffsets[m]);
    const auto tb = std::span(itemTagTargets.data() + itemTagOffsets[w],
                              itemTagOffsets[w + 1] - itemTagOffsets[w]);
    size_t i = 0, j = 0;
    while (i < ta.size() && j < tb.size()) {
      if (ta[i] < tb[j]) {
        i++;
      } else if (tb[j] < ta[i]) {
        j++;
      } else {
        const NodeId tag = ta[i];
        const std::vector<NodeId>& ms = members[tag];
        const auto it = std::lower_bound(ms.begin(), ms.end(), w);
        const size_t idx = (size_t) (it - ms.begin());
        const uint32_t c = --cnt[tag][idx];
        // cnt only ever decreases, so the drop from a full gate (threshold) to
        // a broken one happens at most once per (tag, witness).
        if (c + 1 == threshold[tag]) {
          for (uint e = tagConsumerOffsets[tag]; e < tagConsumerOffsets[tag + 1]; e++) {
            const uint32_t q = idOf(tagConsumerTargets[e], w);
            if (q != UINT32_MAX)
              push(q);
          }
        }
        i++;
        j++;
      }
    }
  }

  // ---- Keep one representative per sink SCC -----------------------------
  // On the alive edges inside a tag, every member reaches some sink SCC, and a
  // sink representative is substitutable for everything that reaches it. This
  // also keeps at least one member of every tag, including mutual-requirement
  // cycles, so no tag becomes unsatisfiable.
  std::vector<uint32_t> adjOffsets, adjTargets, cursor;
  std::vector<int32_t> comp;
  std::vector<uint32_t> repOfComp;
  std::vector<uint8_t> keep;
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const std::vector<NodeId>& ms = members[t];
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
      for (NodeId recipeNode : graph.i2r.targetsOf(t)) {
        const uint r = recipeNode - nItem;
        const auto inputs = graph.r2i.targetsOf(r);
        if (inputs.size() == 1 && inputs[0] == m)
          graph.tagEdgeDominated[r] = 1;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Real-recipe (composite) dominance pruning
// ---------------------------------------------------------------------------
// For a real item X with recipes recs, an edge R -> S means S dominates R: for
// every recipe r producing a real witness input Y of R, the composite
//
//   c_r = ceil(q / p_r) * v_r + v_R
//
// satisfies c_r <= v_S (replace the pair r, R by one S) or c_r <= 0 (the pair
// is a net loss). The keep set is one representative per sink SCC of the edge
// graph, exactly as in the tag pass, so every item keeps at least one recipe.
// A dominated recipe is only dropped by reachability when its witness input has
// no inventory and a replacement can actually be run.
//
// Unlike the tag pass, the relation is a pure cost comparison, so R and S may
// need disjoint workstations and the edge is built regardless. Replacing R by S
// is only valid when S is usable, which depends on the availability set of the
// query, so the pass records the workstations of the kept representatives R can
// reach and reachableSubgraph checks them. The tag pass does not need this
// because its `requires(m, w)` relation already says m cannot be crafted
// without w.

void computeRecipePruning(CraftingGraph& graph) noexcept {
  graph.recipeDominated.assign(graph.nRecipe, 0);
  graph.recipeGuardInput.assign(graph.nRecipe, UINT32_MAX);
  graph.recipeDominatorWorkstations.assign(graph.nRecipe, {});

  const uint nReal = graph.nReal;
  if (nReal == 0 || graph.nRecipe == 0)
    return;

  RecipeVectors vec;
  buildRecipeVectors(graph, vec);

  std::vector<uint32_t> recs;
  std::vector<std::vector<uint32_t>> adj;
  std::vector<NodeId> guard;

  std::vector<NodeId> cItems;
  std::vector<Amount> cCoeffs;
  std::vector<uint32_t> candidates;
  std::vector<uint> adjOffsets;
  std::vector<uint32_t> adjTargets;
  std::vector<int32_t> comp;
  std::vector<uint32_t> repOfComp;
  std::vector<uint8_t> keep;
  std::vector<std::vector<uint32_t>> members;
  std::vector<std::vector<NodeId>> compWs;

  for (NodeId X = 0; X < nReal; X++) {
    const auto recipeNodes = graph.i2r.targetsOf(X);
    if (recipeNodes.size() < 2)
      continue;
    // Comparing every pair of X's recipes costs O(k^2) time and can build a
    // k^2-edge adjacency. Leave an item with an absurd fan-out unpruned rather
    // than spend gigabytes on it.
    if (recipeNodes.size() > MAX_SIBLING_RECIPES)
      continue;

    recs.clear();
    for (NodeId recipeNode : recipeNodes)
      recs.push_back(recipeNode - graph.nItem);
    const uint k = (uint) recs.size();

    adj.assign(k, {});
    guard.assign(k, UINT32_MAX);

    for (uint i = 0; i < k; i++) {
      const uint R = recs[i];
      const auto inputs = graph.r2i.targetsOf(R);
      const auto weights = graph.r2i.weightsOf(R);
      for (size_t a = 0; a < inputs.size(); a++) {
        const NodeId Y = inputs[a];
        if (!graph.isRealItem(Y))
          continue;
        const Amount q = weights[a];
        const auto producers = graph.i2r.targetsOf(Y);

        // The composite has to be at least as good for every producer of Y, so
        // the inner loop is proportional to producers(Y). Skip a witness that
        // is itself produced by an absurd number of recipes; that just leaves
        // R without a guard.
        if (producers.size() > MAX_WITNESS_PRODUCERS)
          continue;

        // Every sibling is a candidate: the workstation condition is deferred
        // to query time, where the availability set is known. If no sibling
        // qualifies after the producer filter, try the next input rather than
        // giving up on R.
        candidates.clear();
        for (uint j = 0; j < k; j++)
          if (j != i)
            candidates.push_back(j);

        if (producers.empty()) {
          // Y cannot be produced. Unless the player holds stock (which the
          // query-time guard checks), R is unusable, so any sibling is at least
          // as good.
          for (uint32_t j : candidates)
            adj[i].push_back(j);
          guard[i] = Y;
          break;
        }

        for (NodeId producerNode : producers) {
          const uint r = producerNode - graph.nItem;
          const Amount p = graph.outputAmt[r];
          if (p <= 0) {
            candidates.clear();
            break;
          }
          Amount alpha = q / p;
          if (q % p != 0)
            alpha++;
          if (!buildComposite(alpha, vec, r, R, cItems, cCoeffs)) {
            candidates.clear();
            break;
          }

          const bool loss = leZero(cCoeffs);
          size_t kept = 0;
          for (size_t c = 0; c < candidates.size(); c++) {
            const uint32_t j = candidates[c];
            const uint S = recs[j];
            if (loss || leVector(cItems, cCoeffs, vec.itemsOf(S), vec.coeffsOf(S)))
              candidates[kept++] = j;
          }
          candidates.resize(kept);
          if (candidates.empty())
            break;
        }

        if (!candidates.empty()) {
          for (uint32_t j : candidates)
            adj[i].push_back(j);
          guard[i] = Y;
          break;  // one witness input is enough; stop scanning inputs
        }
      }
    }

    // One representative per sink SCC.
    adjOffsets.assign(k + 1, 0);
    for (uint i = 0; i < k; i++)
      adjOffsets[i + 1] = (uint) adj[i].size();
    for (uint i = 0; i < k; i++)
      adjOffsets[i + 1] += adjOffsets[i];
    adjTargets.resize(adjOffsets[k]);
    {
      std::vector<uint> cursor(adjOffsets.begin(), adjOffsets.end() - 1);
      for (uint i = 0; i < k; i++)
        for (uint32_t j : adj[i])
          adjTargets[cursor[i]++] = j;
    }
    const uint32_t nComp =
        markSinkRepresentatives(k, adjOffsets, adjTargets, comp, repOfComp, keep);

    // The union of the workstations of every kept representative a node can
    // reach. Those are the replacements reachableSubgraph may fall back on, so
    // it drops the node when one of them is available. Tarjan numbers
    // components in reverse topological order, so a component's successors are
    // already final when it is processed.
    members.assign(nComp, {});
    for (uint32_t a = 0; a < k; a++)
      members[comp[a]].push_back(a);
    compWs.assign(nComp, {});
    for (uint32_t c = 0; c < nComp; c++) {
      std::vector<NodeId>& ws = compWs[c];
      if (repOfComp[c] != UINT32_MAX) {
        const auto own = graph.workstations.targetsOf(recs[repOfComp[c]]);
        ws.insert(ws.end(), own.begin(), own.end());
      }
      for (uint32_t a : members[c])
        for (uint32_t j : adj[a])
          if ((uint32_t) comp[j] != c) {
            const std::vector<NodeId>& succ = compWs[comp[j]];
            ws.insert(ws.end(), succ.begin(), succ.end());
          }
      std::sort(ws.begin(), ws.end());
      ws.erase(std::unique(ws.begin(), ws.end()), ws.end());
    }

    for (uint i = 0; i < k; i++) {
      if (keep[i])
        continue;
      const uint r = recs[i];
      graph.recipeDominated[r] = 1;
      graph.recipeGuardInput[r] = guard[i];
      graph.recipeDominatorWorkstations[r] = compWs[comp[i]];
    }
  }
}

}  // namespace

void computePruning(CraftingGraph& graph) noexcept {
  computeTagPruning(graph);
  computeRecipePruning(graph);
  computePackPruning(graph);
}

void setTagPruningEnabled(bool enabled) noexcept {
  tagPruning = enabled;
}

bool isTagPruningEnabled() noexcept {
  return tagPruning;
}

void setRecipePruningEnabled(bool enabled) noexcept {
  recipePruning = enabled;
}

bool isRecipePruningEnabled() noexcept {
  return recipePruning;
}

}  // namespace aw
