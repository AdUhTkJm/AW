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

#include "aw/plan/Options.h"
#ifdef AW_PROFILE_PRUNING
#  include "aw/plan/Profiler.h"
#endif

using namespace aw::detail;
using uint = uint32_t;

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

namespace detail {

// Query-time re-pruning. See Prune.h for the contract. The subgraph's base
// graph is copied into a throwaway CraftingGraph because the passes are written
// against the full graph struct; only the base fields are read and only the
// pruning vectors are written, so the copy is the whole cost.
aw::vector<uint8_t> repruneSubgraph(const Subgraph &sub,
                                    std::span<const Amount> sourceInventory) noexcept {
#ifdef AW_PROFILE_PRUNING
  auto start = std::chrono::steady_clock::now();
#endif
  const uint nRecipe = sub.graph.nRecipe;
  aw::vector<uint8_t> drop(nRecipe, 0);
  if (!options.reprune.enabled || nRecipe == 0)
    return drop;

  CraftingGraph g;
  static_cast<BaseCraftingGraph &>(g) = sub.graph;
  // No workstations: every recipe that survived the walk is runnable with the
  // query's station set, so a dominator present in the subgraph is available by
  // construction and the composite/direct passes need no guard for it. An
  // explicitly empty CSR keeps `targetsOf` well defined for the unknown ones.
  g.workstations.offsets.assign(nRecipe + 1, 0);
  // The registration-time marks are not part of the subgraph, so the pass sees
  // a clean slate. Sizing them to nRecipe lets the pack pass skip the recipes
  // the other two just certified.
  g.tagEdgeDominated.assign(nRecipe, 0);

  const bool savedNonoptimal = options.nonoptimal;
  options.nonoptimal = !options.reprune.exact;

#ifdef AW_PROFILE_PRUNING
  if (options.outputRepruningProfile)
    std::fprintf(stderr, "[time/reprune] copy graph: %.6f s\n", aw::since(start));
#endif

  RecipeVectors vec(g);
#ifdef AW_PROFILE_PRUNING
  if (options.outputRepruningProfile)
    std::fprintf(stderr, "[time/reprune] construct recipe vectors: %.6f s\n", aw::since(start));
#endif

  computeRecipePruning(g, vec);
#ifdef AW_PROFILE_PRUNING
  if (options.outputRepruningProfile)
    std::fprintf(stderr, "[time/reprune] recipe pruning: %.6f s\n", aw::since(start));
#endif

  computeDirectDominancePruning(g, vec);
#ifdef AW_PROFILE_PRUNING
  if (options.outputRepruningProfile)
    std::fprintf(stderr, "[time/reprune] direct pruning: %.6f s\n", aw::since(start));
#endif

  computeDeadnodePruning(g);
#ifdef AW_PROFILE_PRUNING
  if (options.outputRepruningProfile)
    std::fprintf(stderr, "[time/reprune] dead node pruning: %.6f s\n", aw::since(start));
#endif

  computePackPruning(g);
#ifdef AW_PROFILE_PRUNING
  if (options.outputRepruningProfile)
    std::fprintf(stderr, "[time/reprune] pack pruning: %.6f s\n", aw::since(start));
#endif

  options.nonoptimal = savedNonoptimal;

  // Stock of a subgraph item, read through the remapping.
  const auto held = [&](NodeId subItem) noexcept -> Amount {
    if (subItem >= sub.itemOrigin.size())
      return 0;
    const NodeId source = sub.itemOrigin[subItem];
    return source < sourceInventory.size() ? sourceInventory[source] : 0;
  };

  for (uint r = 0; r < nRecipe; r++) {
    // Tag edges are the tag passes' business, and the query-time inliner has
    // already removed the reducible ones. Leaving them also keeps a tag
    // satisfiable if a pack certificate would otherwise have removed its last
    // member edge.
    if (g.output[r] >= g.nReal)
      continue;

    bool dropRecipe = false;
    // Composite: only when the witness input really has no stock, exactly as
    // the walk gates the registration-time mark.
    if (g.recipeDominated[r] && held(g.recipeGuardInput[r]) == 0)
      dropRecipe = true;
    // Direct: the dominator is present, so no further gate.
    if (g.recipeDirectDominated[r])
      dropRecipe = true;
    // Pack: keep it when any of the certificate's zero-stock items is stocked.
    if (!dropRecipe && g.packDominated[r]) {
      bool clear = true;
      for (NodeId item : g.packCertificates[r].zeroStock) {
        if (held(item) > 0) {
          clear = false;
          break;
        }
      }
      if (clear)
        dropRecipe = true;
    }
    drop[r] = dropRecipe ? 1 : 0;
  }
  return drop;
}

}  // namespace detail

}  // namespace aw
