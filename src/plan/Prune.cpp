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
#include "aw/plan/ProfileStep.h"
#include "aw/utils/Helpers.h"
#include "aw/utils/Scc.h"

using namespace aw::detail;
using uint = uint32_t;

namespace aw::detail {

// For a recipe:
//   +outputAmt at every output row (anchor and byproducts);
//   -inputAmt at input rows.
//
// Equal rows are summed, so a recipe that eats its own output collapses that row.
RecipeVectors::RecipeVectors(const BaseCraftingGraph &graph) noexcept {
  const uint nRecipe = graph.nRecipe;
  offsets.assign(nRecipe + 1, 0);
  aw::vector<std::pair<ItemId, Amount>> scratch;
  for (uint r = 0; r < nRecipe; r++) {
    scratch.clear();
    const auto outs = graph.outputsOf(r);
    const auto outAmts = graph.outputAmountsOf(r);
    for (size_t k = 0; k < outs.size(); k++)
      scratch.emplace_back(outs[k], outAmts[k]);
    const auto inputs = graph.inputsOf(r);
    const auto weights = graph.inputAmountsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      scratch.emplace_back(inputs[k], -weights[k]);

    std::sort(scratch.begin(), scratch.end(),
              [](const auto &a, const auto &b) noexcept { return a.first < b.first; });

    // Sum equal rows and drop zero coefficients.
    for (size_t k = 0; k < scratch.size();) {
      const ItemId item = scratch[k].first;
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

  const uint32_t nComp = aw::computeSccs(adjOffsets, adjTargets, comp);

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
                          aw::vector<aw::vector<ItemId>> &compWs) noexcept {
  const uint32_t nComp = (uint32_t) compWs.size();
  aw::vector<aw::vector<uint32_t>> members(nComp);
  for (uint32_t a = 0; a < adj.size(); a++)
    members[comp[a]].push_back(a);
  for (uint32_t c = 0; c < nComp; c++) {
    aw::vector<ItemId> &ws = compWs[c];
    if (repOfComp[c] != UINT32_MAX) {
      const auto own = graph.workstations.targetsOf(recs[repOfComp[c]]);
      ws.insert(ws.end(), own.begin(), own.end());
    }
    for (uint32_t a : members[c])
      for (uint32_t j : adj[a])
        if ((uint32_t) comp[j] != c) {
          const aw::vector<ItemId> &succ = compWs[comp[j]];
          ws.insert(ws.end(), succ.begin(), succ.end());
        }
    std::sort(ws.begin(), ws.end());
    ws.erase(std::unique(ws.begin(), ws.end()), ws.end());
  }
}

}

namespace aw {
namespace {

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
void compareNormalized(std::span<const ItemId> ii, std::span<const Amount> ic,
                       Amount outI, std::span<const ItemId> ji,
                       std::span<const Amount> jc, Amount outJ, bool &iLeJ,
                       bool &jLeI) noexcept {
  iLeJ = true;
  jLeI = true;
  size_t a = 0, b = 0;
  while (a < ii.size() || b < ji.size()) {
    ItemId next = UINT32_MAX;
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
// Direct (column) dominance pruning
// ---------------------------------------------------------------------------
// A recipe r producing a real item X is dominated when a sibling recipe s of X
// has a componentwise larger column: v_s >= v_r over every item. Because both
// produce X, that is exactly `out_s >= out_r` and `in_s(j) <= in_r(j)` for
// every input j, plus `c_s <= c_r`: replacing one execution of r by one of s
// keeps every balance at least as high and the objective no larger, so no
// optimal plan needs r. With a uniform cost the extra condition always holds,
// so nothing is dropped that the old test kept.
//
// In nonoptimal mode the columns are compared after normalizing
// their output amount to 1 instead of per execution (`compareNormalized`). That
// subsumes the raw comparison for recipes that do not consume their own output,
// but it compares fractional executions: a slow recipe that is cheaper per unit
// can then dominate a fast one that consumes more per unit. For the player this
// trades steps for materials, so it is a deliberate deviation from optimality.
// The deviation is a material one only: the relaxed rule still requires the
// dominator to cost no more per execution, so a heterogeneous cost cannot turn
// it into "drop the cheap route for the expensive one".
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
  aw::vector<uint8_t> selfConsuming;
  aw::vector<aw::vector<uint32_t>> adj;
  aw::vector<uint> adjOffsets;
  aw::vector<uint32_t> adjTargets;
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  aw::vector<aw::vector<ItemId>> compWs;

  for (ItemId X = 0; X < nReal; X++) {
    const auto siblings = graph.producersOf(X);
    if (siblings.size() < 2)
      continue;
    // The same budget as the composite pass: a quadratic scan of an item with
    // an absurd fan-out is not worth the memory it would need.
    if (siblings.size() > options.maxSiblingRecipes)
      continue;

    recs.clear();
    outAmt.clear();
    recs.reserve(siblings.size());
    outAmt.reserve(siblings.size());
    for (RecipeId r : siblings) {
      recs.push_back_unchecked(r);
      // The comparison normalizes and filters by the amount of the shared item,
      // which is not the anchor when the recipe outputs it as a byproduct.
      outAmt.push_back_unchecked(graph.producedAmountOf(r, X));
    }
    const uint k = (uint) recs.size();

    // A sibling that consumes X itself can only run once X is already
    // available, so it is not a replacement for another producer of X:
    // dropping that producer can cut the only grounded route to X and leave
    // the sibling stranded behind a cycle. Such a sibling is never offered as
    // a dominator (it can still be dominated by a grounded one). This mirrors
    // the composite pass; without it the direct pass can strand X, and
    // `pruneSeedUnreachable` then removes the stranded dominator and reports a
    // false INFEASIBLE. It is a cheap one-step guard and does not catch longer
    // cycles back to X.
    selfConsuming.assign(k, 0);
    for (uint j = 0; j < k; j++) {
      const auto ins = graph.inputsOf(recs[j]);
      const auto amts = graph.inputAmountsOf(recs[j]);
      for (size_t a = 0; a < ins.size(); a++)
        if (ins[a] == X && amts[a] > 0) {
          selfConsuming[j] = 1;
          break;
        }
    }

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
          //
          // One execution of j stands in for one of i, so it must not cost
          // more; with a uniform cost both flags are always true.
          if (iLeJ && !jLeI && graph.cost[j] <= graph.cost[i] && !selfConsuming[j])
            adj[i].push_back(j);
          if (jLeI && !iLeJ && graph.cost[i] <= graph.cost[j] && !selfConsuming[i])
            adj[j].push_back(i);
        } else {
          // out_i <= out_j is necessary for v_i <= v_j, so it skips most pairs
          // when the outputs differ; it is not sufficient on its own.
          if (graph.cost[j] <= graph.cost[i] && outAmt[i] <= outAmt[j] &&
              leVector(ii, ic, ji, jc) && !selfConsuming[j])
            adj[i].push_back(j);
          if (graph.cost[i] <= graph.cost[j] && outAmt[j] <= outAmt[i] &&
              leVector(ji, jc, ii, ic) && !selfConsuming[i])
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
  for (ItemId X = 0; X < nReal; X++) {
    const auto siblings = graph.producersOf(X);
    if (siblings.empty())
      continue;
    bool anyKept = false;
    for (RecipeId r : siblings) {
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
    const RecipeId r = siblings.front();
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
  AW_PROFILE_BEGIN();
  computeTagPruning(graph);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "tag pruning");

  // Both recipe passes compare the raw column vectors, so they share one
  // construction.
  RecipeVectors vec(graph);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "construct recipe vectors");

  computeRecipePruning(graph, vec);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "recipe pruning");

  computeDirectDominancePruning(graph, vec);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "direct dominance pruning");

  computeSubstitutionPruning(graph, vec);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "substitution pruning");

  computeDeadnodePruning(graph);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "dead node pruning");

  computePackPruning(graph);
  AW_PROFILE_END(options.outputPruningProfile, "[time]", "pack pruning");
}

namespace detail {

// Query-time re-pruning. See Prune.h for the contract. The subgraph's base
// graph is copied into a throwaway CraftingGraph because the passes are written
// against the full graph struct; only the base fields are read and only the
// pruning vectors are written, so the copy is the whole cost.
aw::vector<uint8_t> repruneSubgraph(const Subgraph &sub,
                                    std::span<const Amount> sourceInventory) noexcept {
  AW_PROFILE_BEGIN();
  const uint nRecipe = sub.graph.nRecipe;
  aw::vector<uint8_t> drop(nRecipe, 0);

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
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reprune]", "copy graph");

  RecipeVectors vec(g);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reprune]", "construct recipe vectors");

  computeRecipePruning(g, vec);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reprune]", "recipe pruning");

  computeDirectDominancePruning(g, vec);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reprune]", "direct pruning");

  computeDeadnodePruning(g);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reprune]", "dead node pruning");

  computePackPruning(g);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reprune]", "pack pruning");

  options.nonoptimal = savedNonoptimal;

  // Stock of a subgraph item, read through the remapping.
  const auto held = [&](ItemId subItem) noexcept -> Amount {
    if (subItem >= sub.itemOrigin.size())
      return 0;
    const ItemId source = sub.itemOrigin[subItem];
    return source < sourceInventory.size() ? sourceInventory[source] : 0;
  };

  // Only real recipes can be dropped, and they are a prefix of the recipe list,
  // so the loop stops at the first tag edge and leaves `drop` zeroed for the
  // tag edges. Tag edges are the tag passes' business, and the query-time
  // inliner has already removed the reducible ones. Leaving them also keeps a
  // tag satisfiable if a pack certificate would otherwise have removed its last
  // member edge. See the ordering note on BaseCraftingGraph::output.
  for (uint r = 0; r < nRecipe && g.output[r] < g.nReal; r++) {
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
      for (ItemId item : g.packCertificates[r].zeroStock) {
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
