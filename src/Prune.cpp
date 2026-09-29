// Dominance pruning for the canonical crafting graph.
//
// Four independent passes run once, at registration time:
//
//   1. Tag-edge pruning deletes synthetic `T <- m` edges whose member m is
//      cost-dominated by another member of T. It only reasons about real
//      recipes that consume the same resource they produce, and it keeps every
//      member a recipe of which can emit more than one unit at a time: such a
//      batch leaves surplus m that is a free way to satisfy T, so the edge may
//      still be worth keeping ("Batching guard" in computeTagPruning). In
//      nonoptimal mode (Options.h) it also computes the transitive closure of
//      the relation, which reaches members whose recipes only consume the
//      dominator through another variant, such as EnderIO's fused quartz.
//
//   2. Real-recipe (composite) pruning deletes a real recipe R: X <- Y, ...
//      when a sibling recipe S of X is provably at least as good after
//      inlining every producer of Y into R. The comparison is a sparse column
//      vector inequality, see docs/algorithm.typ.
//      S may need a workstation R does not, so that condition is checked at
//      query time instead of being folded into the preprocessing.
//
//   3. Direct (column) dominance pruning deletes a real recipe R: o X <- ...
//      when a sibling S of X satisfies v_S >= v_R componentwise, i.e. S yields
//      at least as much X and consumes no more of any input. Nothing is
//      inlined, so no guard input applies; only the replacement's workstation
//      is checked at query time. In nonoptimal mode the columns
//      are compared per unit of output (v / out) and only a strict win creates
//      an edge, so a slow but material-cheap recipe can beat a fast costly one
//      without collapsing equal-ratio recipes of different batch size.
//
//   4. Dominated-input substitution pruning deletes a real recipe R whose
//      column loses to a sibling S once one of R's inputs Y is paid for with a
//      resource w that cost-dominates it. It fills the gap the composite pass
//      leaves when the witness is a pseudo-item or when its production chain
//      needs more than one level, see docs/algorithm.typ.
//
// The passes can remove an edge that the player might usefully keep, because
// a stocked input can always be spent directly, and a recipe whose replacement
// needs an unavailable workstation must survive. The tag and composite passes
// therefore record the input the certificate relied on, and every real-recipe
// pass records the workstations of the replacements, so reachableSubgraph can
// keep a recipe whose guard is in the inventory or whose replacement cannot be
// run.

#include "Prune.h"

#include <algorithm>
#include <span>
#include <utility>

#include "aw/CraftingGraph.h"
#include "aw/Options.h"
#ifdef AW_PROFILE_PRUNING
#  include "aw/Profiler.h"
#endif

using namespace aw::detail;

namespace aw::detail {

// For a recipe:
//   +outputAmt at output rows;
//   -inputAmt at input rows.
//
// Equal rows are summed, so a recipe that eats its own output collapses that row.
RecipeVectors::RecipeVectors(const BaseCraftingGraph &graph) noexcept {
  const uint nRecipe = graph.nRecipe;
  offsets.assign(nRecipe + 1, 0);
  aw::vector<std::pair<NodeId, Amount>> scratch;
  for (uint r = 0; r < nRecipe; r++) {
    scratch.clear();
    scratch.emplace_back(graph.output[r], graph.outputAmt[r]);
    const auto inputs = graph.r2i.targetsOf(r);
    const auto weights = graph.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      scratch.emplace_back(inputs[k], -weights[k]);

    std::sort(scratch.begin(), scratch.end(),
              [](const auto &a, const auto &b) noexcept { return a.first < b.first; });

    // Sum equal rows and drop zero coefficients.
    for (size_t k = 0; k < scratch.size();) {
      const NodeId item = scratch[k].first;
      Amount coeff = 0;
      while (k < scratch.size() && scratch[k].first == item) {
        coeff += scratch[k].second;
        k++;
      }
      if (coeff != 0) {
        items.push_back(item);
        coeffs.push_back(coeff);
      }
    }
    offsets[r + 1] = items.size();
  }
}

// Fills `adjOffsets` / `adjTargets` from a per-node adjacency list.
void buildAdjacency(const aw::vector<aw::vector<uint32_t>> &adj,
                    aw::vector<uint> &adjOffsets,
                    aw::vector<uint32_t> &adjTargets) noexcept {
  const uint k = (uint) adj.size();
  adjOffsets.assign(k + 1, 0);
  for (uint i = 0; i < k; i++)
    adjOffsets[i + 1] = (uint) adj[i].size();
  for (uint i = 0; i < k; i++)
    adjOffsets[i + 1] += adjOffsets[i];
  adjTargets.resize(adjOffsets[k]);
  aw::vector<uint> cursor(adjOffsets.begin(), adjOffsets.end() - 1);
  for (uint i = 0; i < k; i++)
    for (uint32_t j : adj[i])
      adjTargets[cursor[i]++] = j;
}

uint32_t markSinkRepresentatives(uint k, const aw::vector<uint> &adjOffsets,
                                 const aw::vector<uint32_t> &adjTargets,
                                 aw::vector<int32_t> &comp,
                                 aw::vector<uint32_t> &repOfComp,
                                 aw::vector<uint8_t> &keep) noexcept {
  keep.assign(k, 0);
  comp.assign(k, -1);
  repOfComp.clear();
  if (k == 0)
    return 0;

  // Iterative Tarjan, so a deep graph cannot overflow the stack.
  aw::vector<int32_t> disc(k, -1), low(k, 0);
  aw::vector<uint8_t> onStack(k, 0);
  aw::vector<uint32_t> tstack, callNode, callEdge;
  // A DFS stack never holds more than one frame per node.
  tstack.reserve(k);
  callNode.reserve(k);
  callEdge.reserve(k);
  int32_t timer = 0;
  uint32_t nComp = 0;
  for (uint32_t s = 0; s < k; s++) {
    if (disc[s] != -1)
      continue;
    disc[s] = low[s] = timer++;
    tstack.push_back_unchecked(s);
    onStack[s] = 1;
    callNode.push_back_unchecked(s);
    callEdge.push_back_unchecked(adjOffsets[s]);
    while (!callNode.empty()) {
      const uint32_t v = callNode.back();
      uint32_t &edge = callEdge.back();
      if (edge < adjOffsets[v + 1]) {
        const uint32_t u = adjTargets[edge++];
        if (disc[u] == -1) {
          disc[u] = low[u] = timer++;
          tstack.push_back_unchecked(u);
          onStack[u] = 1;
          callNode.push_back_unchecked(u);
          callEdge.push_back_unchecked(adjOffsets[u]);
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

  aw::vector<uint8_t> isSink(nComp, 1);
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


// Fills `compWs[c]` with the union of the workstations of every sink-SCC
// representative reachable from component c, assuming Tarjan numbered the
// components in reverse topological order (an edge a -> b across components
// has comp[b] < comp[a]). `compWs` must already be sized to the component
// count. Those representatives are the replacements a dropped recipe can fall
// back on, so the query keeps it only when one of their stations is available.
void compositeWorkstations(const CraftingGraph &graph,
                          const aw::vector<aw::vector<uint32_t>> &adj,
                          const aw::vector<int32_t> &comp,
                          const aw::vector<uint32_t> &repOfComp,
                          const aw::vector<uint32_t> &recs,
                          aw::vector<aw::vector<NodeId>> &compWs) noexcept {
  const uint32_t nComp = (uint32_t) compWs.size();
  aw::vector<aw::vector<uint32_t>> members(nComp);
  for (uint32_t a = 0; a < adj.size(); a++)
    members[comp[a]].push_back(a);
  for (uint32_t c = 0; c < nComp; c++) {
    aw::vector<NodeId> &ws = compWs[c];
    if (repOfComp[c] != UINT32_MAX) {
      const auto own = graph.workstations.targetsOf(recs[repOfComp[c]]);
      ws.insert(ws.end(), own.begin(), own.end());
    }
    for (uint32_t a : members[c])
      for (uint32_t j : adj[a])
        if ((uint32_t) comp[j] != c) {
          const aw::vector<NodeId> &succ = compWs[comp[j]];
          ws.insert(ws.end(), succ.begin(), succ.end());
        }
    std::sort(ws.begin(), ws.end());
    ws.erase(std::unique(ws.begin(), ws.end()), ws.end());
  }
}

}

namespace aw {
namespace {

using uint = uint32_t;

void prefixSum(aw::vector<uint> &v) noexcept {
  v.insert(v.begin(), 0);
  for (size_t i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
}

// Returns true when a * b does not fit in int64_t. `out` is only written when
// it fits.
bool mulOverflow(int64_t a, int64_t b, int64_t &out) noexcept {
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

bool addOverflow(int64_t a, int64_t b, int64_t &out) noexcept {
  if (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)
    return true;
  out = a + b;
  return false;
}

// Builds `c = alpha * v_r + v_R`, sorted and unique, into `outItems` / `outCoeffs`.
// Returns false on integer overflow, which is treated as "cannot prove
// dominance" by the caller.
bool buildComposite(int64_t alpha, const RecipeVectors &vec, uint r, uint R,
                    aw::vector<NodeId> &outItems,
                    aw::vector<Amount> &outCoeffs) noexcept {
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

// Compares two sibling columns after normalizing their output amount to 1: a
// recipe that yields `out` units has the per-unit column `v / out`. `iLeJ` is
// set when every coefficient of i / outI is at most the matching coefficient
// of j / outJ, and `jLeI` is the reverse; both set means the per-unit columns
// are equal. Cross multiplying turns the test into `c_i * outJ <= c_j * outI`,
// which avoids the division and stays exact in integers. A row present in only
// one column compares against 0, as in leVector. On overflow both stay false,
// which the caller treats as "cannot prove dominance".
//
// This is the nonoptimal relaxation of direct dominance: it compares
// fractional executions, so it can drop a recipe whose one execution is not
// replaceable by one execution of a sibling, e.g.
//
//   X x1 <- (nothing)      per unit: no input
//   X x6 <- essence x3     per unit: 0.5 essence
//
// The second is dominated per unit by the first although six X need six
// executions of it.
void compareNormalized(std::span<const NodeId> ii, std::span<const Amount> ic,
                       Amount outI, std::span<const NodeId> ji,
                       std::span<const Amount> jc, Amount outJ, bool &iLeJ,
                       bool &jLeI) noexcept {
  iLeJ = true;
  jLeI = true;
  size_t a = 0, b = 0;
  while (a < ii.size() || b < ji.size()) {
    NodeId next = UINT32_MAX;
    if (a < ii.size())
      next = std::min(next, ii[a]);
    if (b < ji.size())
      next = std::min(next, ji[b]);

    const Amount ci = (a < ii.size() && ii[a] == next) ? ic[a++] : 0;
    const Amount cj = (b < ji.size() && ji[b] == next) ? jc[b++] : 0;
    Amount lhs = 0, rhs = 0;
    if (mulOverflow(ci, outJ, lhs) || mulOverflow(cj, outI, rhs)) {
      iLeJ = false;
      jLeI = false;
      return;
    }
    if (lhs > rhs)
      iLeJ = false;
    if (rhs > lhs)
      jLeI = false;
    if (!iLeJ && !jLeI)
      return;
  }
}

// ---------------------------------------------------------------------------
// Tag-member dominance pruning
// ---------------------------------------------------------------------------
// A tag (pseudo-resource) T is a set of members, encoded as one synthetic
// `T <- m` recipe per member. Serving T through a member m is pointless when
// another member w is strictly cheaper, because then the tag edge `T <- m` can
// be replaced by `T <- w`. The relation used here is `requires(m, w)`: for
// every recipe r of m there is an amount-qualified input that is either w
// itself, a real item that in turn requires w, or a simple tag all of whose
// producible members require w. It implies the per-unit cost of m exceeds that
// of w. See docs/algorithm.typ.
//
// The real-item and all-members steps are the transitive closure of the
// relation, and they are only taken in nonoptimal mode: in exact mode the input
// has to be w itself, or a tag that literally contains w and whose other
// members all require w. Both modes are computed as one greatest fixpoint over
// the pair universe built below, where a tag pair is an AND over its members.
//
// A tag is "simple" when every one of its recipes is a member edge: exactly one
// input, no workstation, and a real member. Everything the Java writer emits
// looks like that. A malformed-but-parseable tag is left unpruned, which is
// conservative.

using PairKey = uint64_t;

constexpr PairKey packPair(NodeId m, NodeId w) noexcept {
  return ((PairKey) m << 32) | (PairKey) w;
}

void intersectSorted(const aw::vector<NodeId> &a, const aw::vector<NodeId> &b,
                     aw::vector<NodeId> &out) noexcept {
  out.clear();
  // The intersection of two sorted sets is no longer than the shorter one.
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
          intersectSorted(acc, scratch, next);
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
// In nonoptimal mode the inline count is floored instead of
// rounded up, so a producer that overshoots q is treated as if the excess came
// for free and more recipes are dominated.
//
// Unlike the tag pass, the relation is a pure cost comparison, so R and S may
// need disjoint workstations and the edge is built regardless. Replacing R by S
// is only valid when S is usable, which depends on the availability set of the
// query, so the pass records the workstations of the kept representatives R can
// reach and reachableSubgraph checks them. The tag pass does not need this
// because its `requires(m, w)` relation already says m cannot be crafted
// without w.

void computeRecipePruning(CraftingGraph &graph, const RecipeVectors &vec) noexcept {
  graph.recipeDominated.assign(graph.nRecipe, 0);
  graph.recipeGuardInput.assign(graph.nRecipe, UINT32_MAX);
  graph.recipeDominatorWorkstations.assign(graph.nRecipe, {});

  const uint nReal = graph.nReal;
  if (nReal == 0 || graph.nRecipe == 0)
    return;

  aw::vector<uint32_t> recs;
  aw::vector<aw::vector<uint32_t>> adj;
  aw::vector<NodeId> guard;

  aw::vector<NodeId> cItems;
  aw::vector<Amount> cCoeffs;
  aw::vector<uint32_t> candidates;
  aw::vector<uint> adjOffsets;
  aw::vector<uint32_t> adjTargets;
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  aw::vector<aw::vector<NodeId>> compWs;

  for (NodeId X = 0; X < nReal; X++) {
    const auto recipeNodes = graph.i2r.targetsOf(X);
    if (recipeNodes.size() < 2)
      continue;
    // Comparing every pair of X's recipes costs O(k^2) time and can build a
    // k^2-edge adjacency. Leave an item with an absurd fan-out unpruned rather
    // than spend gigabytes on it.
    if (recipeNodes.size() > options.maxSiblingRecipes)
      continue;

    recs.clear();
    recs.reserve(recipeNodes.size());
    for (NodeId recipeNode : recipeNodes)
      recs.push_back_unchecked(recipeNode - graph.nItem);
    const uint k = (uint) recs.size();

    adj.assign(k, {});
    guard.assign(k, UINT32_MAX);
    candidates.reserve(k);

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
        if (producers.size() > options.maxWitnessProducers)
          continue;

        // Every sibling is a candidate: the workstation condition is deferred
        // to query time, where the availability set is known. If no sibling
        // qualifies after the producer filter, try the next input rather than
        // giving up on R.
        candidates.clear();
        for (uint j = 0; j < k; j++)
          if (j != i)
            candidates.push_back_unchecked(j);

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
          if (!options.nonoptimal && q % p != 0)
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
    buildAdjacency(adj, adjOffsets, adjTargets);
    const uint32_t nComp =
        markSinkRepresentatives(k, adjOffsets, adjTargets, comp, repOfComp, keep);

    // The union of the workstations of every kept representative a node can
    // reach. Those are the replacements reachableSubgraph may fall back on, so
    // it drops the node when one of them is available. Tarjan numbers
    // components in reverse topological order, so a component's successors are
    // already final when it is processed.
    compWs.assign(nComp, {});
    compositeWorkstations(graph, adj, comp, repOfComp, recs, compWs);

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

// ---------------------------------------------------------------------------
// Direct (column) dominance pruning
// ---------------------------------------------------------------------------
// A recipe r producing a real item X is dominated when a sibling recipe s of X
// has a componentwise larger column: v_s >= v_r over every item. Because both
// produce X, that is exactly `out_s >= out_r` and `in_s(j) <= in_r(j)` for
// every input j. Replacing one execution of r by one of s keeps every balance
// at least as high while the step count is unchanged, so no optimal plan needs
// r.
//
// In nonoptimal mode the columns are compared after normalizing
// their output amount to 1 instead of per execution (`compareNormalized`). That
// subsumes the raw comparison for recipes that do not consume their own output,
// but it compares fractional executions: a slow recipe that is cheaper per unit
// can then dominate a fast one that consumes more per unit. For the player this
// trades steps for materials, so it is a deliberate deviation from optimality.
//
// Unlike the composite pass nothing is inlined, so no guard input is recorded
// and held stock never makes r preferable. The comparison is a partial order on
// columns (on per-unit columns in nonoptimal mode), so keeping one
// representative per sink SCC keeps exactly the maximal columns. The raw
// relation puts equal columns in one SCC; the relaxed one is strict and leaves
// equal per-unit columns of different batch size as separate sinks. Either way
// the relation is transitive, so every representative above r dominates r
// directly, and the union of their workstations is a valid replacement set. As
// in the composite pass the dominator may need a workstation r does not, so
// availability is deferred to the query.
void computeDirectDominancePruning(CraftingGraph &graph,
                                   const RecipeVectors &vec) noexcept {
  graph.recipeDirectDominated.assign(graph.nRecipe, 0);
  graph.recipeDirectDominatorWorkstations.assign(graph.nRecipe, {});

  const uint nReal = graph.nReal;
  if (nReal == 0 || graph.nRecipe == 0)
    return;

  aw::vector<uint32_t> recs;
  aw::vector<Amount> outAmt;
  aw::vector<aw::vector<uint32_t>> adj;
  aw::vector<uint> adjOffsets;
  aw::vector<uint32_t> adjTargets;
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  aw::vector<aw::vector<NodeId>> compWs;

  for (NodeId X = 0; X < nReal; X++) {
    const auto recipeNodes = graph.i2r.targetsOf(X);
    if (recipeNodes.size() < 2)
      continue;
    // The same budget as the composite pass: a quadratic scan of an item with
    // an absurd fan-out is not worth the memory it would need.
    if (recipeNodes.size() > options.maxSiblingRecipes)
      continue;

    recs.clear();
    outAmt.clear();
    recs.reserve(recipeNodes.size());
    outAmt.reserve(recipeNodes.size());
    for (NodeId recipeNode : recipeNodes) {
      const uint r = recipeNode - graph.nItem;
      recs.push_back_unchecked(r);
      outAmt.push_back_unchecked(graph.outputAmt[r]);
    }
    const uint k = (uint) recs.size();

    // adj[i] holds every sibling j whose column dominates i's, so the sink SCC
    // representatives are the maximal columns.
    adj.assign(k, {});
    for (uint i = 0; i < k; i++) {
      const auto ii = vec.itemsOf(recs[i]);
      const auto ic = vec.coeffsOf(recs[i]);
      for (uint j = i + 1; j < k; j++) {
        const auto ji = vec.itemsOf(recs[j]);
        const auto jc = vec.coeffsOf(recs[j]);

        if (options.nonoptimal) {
          // Per-unit-output comparison. A non-positive output amount has no
          // meaningful normalization, so such a pair stays incomparable.
          bool iLeJ = false, jLeI = false;
          if (outAmt[i] > 0 && outAmt[j] > 0)
            compareNormalized(ii, ic, outAmt[i], ji, jc, outAmt[j], iLeJ, jLeI);
          // Only a strict per-unit win creates an edge. Equal per-unit
          // columns differ only in batch size, and an integer target may need
          // the finer batch (a wall x1 and a wall x6 from the same per-unit
          // ratio are not interchangeable when one wall is wanted), so they
          // stay incomparable and both survive.
          if (iLeJ && !jLeI)
            adj[i].push_back(j);
          if (jLeI && !iLeJ)
            adj[j].push_back(i);
        } else {
          // out_i <= out_j is necessary for v_i <= v_j, so it skips most pairs
          // when the outputs differ; it is not sufficient on its own.
          if (outAmt[i] <= outAmt[j] && leVector(ii, ic, ji, jc))
            adj[i].push_back(j);
          if (outAmt[j] <= outAmt[i] && leVector(ji, jc, ii, ic))
            adj[j].push_back(i);
        }
      }
    }

    buildAdjacency(adj, adjOffsets, adjTargets);
    const uint32_t nComp =
        markSinkRepresentatives(k, adjOffsets, adjTargets, comp, repOfComp, keep);
    compWs.assign(nComp, {});
    compositeWorkstations(graph, adj, comp, repOfComp, recs, compWs);

    for (uint i = 0; i < k; i++) {
      if (keep[i])
        continue;
      const uint r = recs[i];
      graph.recipeDirectDominated[r] = 1;
      graph.recipeDirectDominatorWorkstations[r] = compWs[comp[i]];
    }
  }
}

void computeDeadnodePruning(CraftingGraph &graph) noexcept {
  const uint nReal = graph.nReal;
  const bool haveComposite = graph.recipeDominated.size() == graph.nRecipe &&
                             graph.recipeDominatorWorkstations.size() == graph.nRecipe;
  const bool haveDirect = graph.recipeDirectDominated.size() == graph.nRecipe;
  const bool haveSubstitution =
      graph.recipeSubstituted.size() == graph.nRecipe &&
      graph.recipeSubstitutedGuards.size() == graph.nRecipe;
  for (NodeId X = 0; X < nReal; X++) {
    const auto recipeNodes = graph.i2r.targetsOf(X);
    if (recipeNodes.empty())
      continue;
    bool anyKept = false;
    for (NodeId recipeNode : recipeNodes) {
      const uint r = recipeNode - graph.nItem;
      const bool composite = haveComposite && graph.recipeDominated[r];
      const bool direct = haveDirect && graph.recipeDirectDominated[r];
      const bool substituted = haveSubstitution && graph.recipeSubstituted[r];
      if (!composite && !direct && !substituted) {
        anyKept = true;
        break;
      }
    }
    if (anyKept)
      continue;
    const uint r = recipeNodes.front() - graph.nItem;
    if (haveComposite) {
      graph.recipeDominated[r] = 0;
      graph.recipeGuardInput[r] = UINT32_MAX;
      graph.recipeDominatorWorkstations[r].clear();
    }
    if (haveDirect)
      graph.recipeDirectDominated[r] = 0;
    if (haveSubstitution) {
      graph.recipeSubstituted[r] = 0;
      graph.recipeSubstitutedGuards[r].clear();
      graph.recipeSubstitutedDominatorWorkstations[r].clear();
    }
  }
}

}  // namespace

void prune(CraftingGraph &graph) noexcept {
#ifdef AW_PROFILE_PRUNING
  auto start = std::chrono::steady_clock::now();
#endif
  computeTagPruning(graph);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] tag pruning: %.6f s\n", aw::since(start));
#endif

  // Both recipe passes compare the raw column vectors, so they share one
  // construction.
  RecipeVectors vec(graph);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] construct recipe vectors: %.6f s\n", aw::since(start));
#endif

  computeRecipePruning(graph, vec);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] recipe pruning: %.6f s\n", aw::since(start));
#endif

  computeDirectDominancePruning(graph, vec);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] direct dominance pruning: %.6f s\n", aw::since(start));
#endif

  detail::computeSubstitutionPruning(graph, vec);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] substitution pruning: %.6f s\n", aw::since(start));
#endif

  computeDeadnodePruning(graph);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] dead node pruning: %.6f s\n", aw::since(start));
#endif

  detail::computePackPruning(graph);
#ifdef AW_PROFILE_PRUNING
  if (options.outputPruningProfile)
    std::fprintf(stderr, "[time] pack pruning: %.6f s\n", aw::since(start));
#endif
}

}  // namespace aw
