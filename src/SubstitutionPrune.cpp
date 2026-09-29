#include "Prune.h"
#include "aw/Options.h"
#include <ankerl/unordered_dense.h>

namespace aw::detail {

struct ColumnView {
  const std::span<const NodeId> items;
  const std::span<const Amount> coeffs;

  ColumnView(const std::span<const NodeId> &items, const std::span<const Amount> &coeffs) noexcept:
    items(items), coeffs(coeffs) {}

  bool empty() const noexcept { return items.empty(); }
  size_t size() const noexcept { return items.size(); }

  Amount coeff(NodeId row) const noexcept;
};

Amount ColumnView::coeff(NodeId row) const noexcept {
  const auto it = std::lower_bound(items.begin(), items.end(), row);
  if (it == items.end() || *it != row)
    return 0;
  return coeffs[it - items.begin()];
}


// Adds `delta` to the coefficient of `row` of a sorted sparse column, writing
// the result to `outItems`/`outCoeffs`.
void addToColumn(const ColumnView &col,
                 NodeId row, Amount delta, aw::vector<NodeId> &outItems,
                 aw::vector<Amount> &outCoeffs) noexcept {
  outItems.clear();
  outCoeffs.clear();
  size_t i = 0;
  bool inserted = false;
  while (i < col.items.size()) {
    if (!inserted && row < col.items[i]) {
      if (delta != 0) {
        outItems.push_back(row);
        outCoeffs.push_back(delta);
      }
      inserted = true;
      continue;
    }
    if (col.items[i] == row) {
      const Amount c = col.coeffs[i] + delta;
      if (c != 0) {
        outItems.push_back(row);
        outCoeffs.push_back(c);
      }
      inserted = true;
      i++;
      continue;
    }
    outItems.push_back(col.items[i]);
    outCoeffs.push_back(col.coeffs[i]);
    i++;
  }
  if (!inserted && delta != 0) {
    outItems.push_back(row);
    outCoeffs.push_back(delta);
  }
}

// Context of one cost query: a memo over (item, dominator) pairs and a work
// budget. `costsRec(y, w)` is true when every way of producing one unit of y
// consumes at least one unit of w. A pseudo-item is a simple tag, so the cost
// is the AND over its members; a real item is the AND over its recipes, each of
// which needs one amount-qualified input that is w or itself costs w. Cycles
// are cut to `false`, which is the least fixpoint: it can only drop derivations,
// so it prunes less.
struct CostContext {
  const CraftingGraph *graph = nullptr;
  const aw::vector<uint8_t> simpleTag;
  const aw::vector<aw::vector<NodeId>> tagMembers;
  // Exact mode only: an item whose producers can emit a batch has a surplus a
  // plan can spend for free, so the per-unit bound is not enough. Requiring
  // every item on the chain to be unit-output removes the surplus.
  const aw::vector<uint8_t> unitOutput;
  ankerl::unordered_dense::map<uint64_t, uint8_t> memo ;  // 1=in progress, 2=false, 3=true
  uint64_t work;
  bool requireUnit = false;

  CostContext(const CraftingGraph &graph, uint64_t work,
    aw::vector<uint8_t> &&simpleTag, aw::vector<aw::vector<NodeId>> &&tagMembers, aw::vector<uint8_t> &&unitOutput
  ):
    graph(&graph), simpleTag(simpleTag), tagMembers(tagMembers), unitOutput(unitOutput), work(work) {}
};

bool costs(CostContext &ctx, NodeId y, NodeId w, uint32_t depth) noexcept {
  if (ctx.work == 0)
    return false;
  ctx.work--;
  if (y == w)
    return true;
  if (depth == 0)
    return false;

  const uint64_t key = ((uint64_t) y << 32) | (uint64_t) w;
  const auto it = ctx.memo.find(key);
  if (it != ctx.memo.end()) {
    if (it->second == 1)
      return false;  // on the current path: the least fixpoint has no cycle
    return it->second == 3;
  }
  const bool memoize = ctx.memo.size() < options.maxCostMemo;
  if (memoize)
    ctx.memo[key] = 1;

  const CraftingGraph &graph = *ctx.graph;
  bool result;
  if (y >= graph.nReal) {
    result = ctx.simpleTag[y] != 0;
    if (result) {
      for (NodeId z : ctx.tagMembers[y]) {
        if (ctx.requireUnit && !ctx.unitOutput[z]) {
          result = false;
          break;
        }
        if (!costs(ctx, z, w, depth - 1)) {
          result = false;
          break;
        }
      }
    }
  } else {
    if (ctx.requireUnit && !ctx.unitOutput[y])
      return false;
    const auto recipes = graph.i2r.targetsOf(y);
    // A raw material is gathered, so it does not consume a crafted w.
    result = !recipes.empty();
    for (NodeId recipeNode : recipes) {
      if (!result)
        break;
      const uint r = recipeNode - graph.nItem;
      const Amount out = graph.outputAmt[r];
      const auto inputs = graph.r2i.targetsOf(r);
      const auto weights = graph.r2i.weightsOf(r);
      bool ok = false;
      for (size_t k = 0; k < inputs.size(); k++) {
        if (weights[k] < out)
          continue;
        const NodeId j = inputs[k];
        if (j == w || costs(ctx, j, w, depth - 1)) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        result = false;
        break;
      }
    }
  }

  if (memoize)
    ctx.memo[key] = result ? 3 : 2;
  return result;
}

// True when the column `cur` can be raised above the sibling column `s` by
// moving demand from some of `cur`'s inputs onto a cost dominator. `collapses`
// receives the inputs that were moved. The first row where `cur` exceeds `s` is
// covered by moving `min(q_Y, deficit)` units from an input Y with
// `costsRec(Y, d)`, then the next deficit is handled the same way.
bool substitutionDominates(CostContext &ctx, const ColumnView &cur, const ColumnView &s, uint32_t depth,
                           uint64_t &work, aw::vector<NodeId> &collapses) noexcept {
  if (work == 0)
    return false;
  work--;

  // The first row where `cur` exceeds `s`.
  NodeId d = UINT32_MAX;
  size_t i = 0, j = 0;
  while (i < cur.items.size() || j < s.items.size()) {
    NodeId next = UINT32_MAX;
    if (i < cur.items.size())
      next = std::min(next, cur.items[i]);
    if (j < s.items.size())
      next = std::min(next, s.items[j]);
    const Amount cu =
        (i < cur.items.size() && cur.items[i] == next) ? cur.coeffs[i++] : 0;
    const Amount sv = (j < s.items.size() && s.items[j] == next) ? s.coeffs[j++] : 0;
    if (cu > sv) {
      d = next;
      break;
    }
  }
  
  if (d == UINT32_MAX)
    return !collapses.empty();
  if (depth == 0)
    return false;
  // A produced row cannot be fixed by paying with a dominator, and a dominator
  // is always a real item, so a deficit on a pseudo-item row is out of reach.
  int cd = cur.coeff(d);
  if (cd > 0 || d >= ctx.graph->nReal)
    return false;

  const Amount deficit = cd - s.coeff(d);
  if (deficit <= 0)
    return false;

  aw::vector<NodeId> tmpItems, nextItems;
  aw::vector<Amount> tmpCoeffs, nextCoeffs;

  for (size_t k = 0; k < cur.size(); k++) {
    const NodeId y = cur.items[k];
    const Amount coeff = cur.coeffs[k];
    if (coeff >= 0 || y == d)
      continue;
    if (!costs(ctx, y, d, options.maxCostDepth))
      continue;
    Amount t = std::min(-coeff, deficit);
    // Moving t units of y onto d must not push y's own row into deficit.
    const Amount room = s.coeff(y) - coeff;
    if (t > room)
      t = room;
    if (t <= 0)
      continue;

    addToColumn(cur, y, t, tmpItems, tmpCoeffs);
    addToColumn(ColumnView(tmpItems, tmpCoeffs), d, -t, nextItems, nextCoeffs);

    const size_t before = collapses.size();
    collapses.push_back_unchecked(y);
    if (substitutionDominates(ctx, ColumnView(nextItems, nextCoeffs), s,
                              depth - 1, work, collapses))
      return true;
    collapses.resize(before);
  }
  return false;
}

// `compGuards[c]` is the union of the guards on every edge of every member of
// component c and of every component reachable from it. Tarjan numbers the
// components in reverse topological order, so a component's successors are
// already final.
void computeCompGuards(const aw::vector<aw::vector<uint32_t>> &adj,
                       const aw::vector<int32_t> &comp,
                       const aw::vector<aw::vector<NodeId>> &edgeGuards,
                       aw::vector<aw::vector<NodeId>> &compGuards) noexcept {
  const uint32_t nComp = (uint32_t) compGuards.size();
  aw::vector<aw::vector<uint32_t>> members(nComp);
  for (uint32_t a = 0; a < adj.size(); a++)
    members[comp[a]].push_back(a);
  for (uint32_t c = 0; c < nComp; c++) {
    aw::vector<NodeId> &g = compGuards[c];
    for (uint32_t a : members[c]) {
      const aw::vector<NodeId> &own = edgeGuards[a];
      g.insert(g.end(), own.begin(), own.end());
    }
    for (uint32_t a : members[c])
      for (uint32_t j : adj[a])
        if ((uint32_t) comp[j] != c) {
          const aw::vector<NodeId> &succ = compGuards[comp[j]];
          g.insert(g.end(), succ.begin(), succ.end());
        }
    std::sort(g.begin(), g.end());
    g.erase(std::unique(g.begin(), g.end()), g.end());
  }
}

// Expands a collapsed input into the real items whose stock must be zero for
// the substitution to apply: the item itself, or every member of a tag. A tag
// wider than the cap is refused, because a catch-all tag is not worth a
// per-recipe guard list.
bool collectGuards(const CraftingGraph &graph, NodeId y,
                   aw::vector<NodeId> &out) noexcept {
  out.clear();
  if (y < graph.nReal) {
    out.reserve(1);
    out.push_back_unchecked(y);
    return true;
  }
  const auto recipes = graph.i2r.targetsOf(y);
  if (recipes.size() > options.maxSubstitutionGuardItems)
    return false;
  // At most one guard per tag member recipe.
  out.reserve(recipes.size());
  for (NodeId recipeNode : recipes) {
    const auto inputs = graph.r2i.targetsOf(recipeNode - graph.nItem);
    if (inputs.size() != 1)
      return false;
    out.push_back_unchecked(inputs[0]);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

void computeSubstitutionPruning(CraftingGraph &graph, const RecipeVectors &vec) noexcept {
  graph.recipeSubstituted.assign(graph.nRecipe, 0);
  graph.recipeSubstitutedGuards.assign(graph.nRecipe, {});
  graph.recipeSubstitutedDominatorWorkstations.assign(graph.nRecipe, {});

  const uint nReal = graph.nReal;
  const uint nItem = graph.nItem;
  if (nReal == 0 || graph.nRecipe == 0)
    return;

  // Simple tags and their members, matching the tag pass's definition: every
  // synthetic recipe has exactly one real input and no workstation.
  aw::vector<uint8_t> simpleTag(nItem, 0);
  aw::vector<aw::vector<NodeId>> tagMembers(nItem);
  for (NodeId t = nReal; t < nItem; t++) {
    const auto recipes = graph.i2r.targetsOf(t);
    if (recipes.empty())
      continue;
    aw::vector<NodeId> ms;
    ms.reserve(recipes.size());
    bool simple = true;
    for (NodeId recipeNode : recipes) {
      const uint r = recipeNode - graph.nItem;
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
    // A catch-all tag is neither worth a per-recipe guard list nor cheap to
    // walk, so it is left out of the cost relation entirely.
    if (ms.size() > options.maxSubstitutionGuardItems)
      continue;
    simpleTag[t] = 1;
    tagMembers[t] = std::move(ms);
  }

  // Exact mode needs the batching guard; nonoptimal mode waives it, as the tag
  // pass does.
  aw::vector<uint8_t> unitOutput;
  if (!options.nonoptimal) {
    unitOutput.assign(nItem, 1);
    for (NodeId m = 0; m < nReal; m++)
      for (NodeId recipeNode : graph.i2r.targetsOf(m))
        if (graph.outputAmt[recipeNode - graph.nItem] != 1) {
          unitOutput[m] = 0;
          break;
        }
  }

  CostContext ctx(graph, options.maxSubstitutionCostWork,
    std::move(simpleTag), std::move(tagMembers), std::move(unitOutput)
  );
  ctx.requireUnit = !options.nonoptimal;

  uint64_t work = options.maxSubstitutionWork;
  size_t guardTotal = 0;

  aw::vector<uint32_t> recs;
  aw::vector<aw::vector<uint32_t>> adj;
  aw::vector<aw::vector<NodeId>> edgeGuards;
  aw::vector<uint> adjOffsets;
  aw::vector<uint32_t> adjTargets;
  aw::vector<int32_t> comp;
  aw::vector<uint32_t> repOfComp;
  aw::vector<uint8_t> keep;
  aw::vector<aw::vector<NodeId>> compWs;
  aw::vector<aw::vector<NodeId>> compGuards;

  aw::vector<NodeId> collapses, resolved, tmpGuards;
  // The recursive search abandons any branch that would exceed the depth cap.
  collapses.reserve(options.maxSubstitutionDepth);

  for (NodeId X = 0; X < nReal && work != 0; X++) {
    const auto recipeNodes = graph.i2r.targetsOf(X);
    if (recipeNodes.size() < 2)
      continue;
    if (recipeNodes.size() > options.maxSiblingRecipes)
      continue;

    recs.clear();
    recs.reserve(recipeNodes.size());
    for (NodeId recipeNode : recipeNodes)
      recs.push_back_unchecked(recipeNode - graph.nItem);
    const uint k = (uint) recs.size();

    adj.assign(k, {});
    edgeGuards.assign(k, {});

    for (uint i = 0; i < k && work != 0; i++) {
      const uint R = recs[i];
      const auto uItems = vec.itemsOf(R);
      const auto uCoeffs = vec.coeffsOf(R);
      for (uint j = 0; j < k && work != 0; j++) {
        if (i == j)
          continue;
        const uint S = recs[j];
        collapses.clear();
        if (!substitutionDominates(ctx, ColumnView(uItems, uCoeffs),
                                   ColumnView(vec.itemsOf(S), vec.coeffsOf(S)),
                                   options.maxSubstitutionDepth, work, collapses))
          continue;
        // Resolve the guards; a collapsed tag too wide to list makes the edge
        // unusable, so it is dropped along with the rest of this attempt.
        tmpGuards.clear();
        bool guardOk = true;
        for (NodeId y : collapses) {
          if (!collectGuards(graph, y, resolved)) {
            guardOk = false;
            break;
          }
          tmpGuards.insert(tmpGuards.end(), resolved.begin(), resolved.end());
        }
        if (!guardOk)
          continue;
        adj[i].push_back(j);
        aw::vector<NodeId> &eg = edgeGuards[i];
        eg.insert(eg.end(), tmpGuards.begin(), tmpGuards.end());
      }
      aw::vector<NodeId> &eg = edgeGuards[i];
      std::sort(eg.begin(), eg.end());
      eg.erase(std::unique(eg.begin(), eg.end()), eg.end());
    }

    bool any = false;
    for (uint i = 0; i < k && !any; i++)
      any = !adj[i].empty();
    if (!any)
      continue;

    buildAdjacency(adj, adjOffsets, adjTargets);
    const uint32_t nComp =
        markSinkRepresentatives(k, adjOffsets, adjTargets, comp, repOfComp, keep);
    compWs.assign(nComp, {});
    compositeWorkstations(graph, adj, comp, repOfComp, recs, compWs);
    compGuards.assign(nComp, {});
    computeCompGuards(adj, comp, edgeGuards, compGuards);

    for (uint i = 0; i < k; i++) {
      if (keep[i])
        continue;
      const aw::vector<NodeId> &guards = compGuards[comp[i]];
      // A path union wider than the per-recipe cap would make the query-time
      // guard scan unbounded, so the recipe is left alive instead.
      if (guards.size() > options.maxSubstitutionGuardItems ||
          guardTotal + guards.size() > options.maxSubstitutionGuardTotal)
        continue;
      const uint r = recs[i];
      graph.recipeSubstituted[r] = 1;
      graph.recipeSubstitutedGuards[r] = guards;
      graph.recipeSubstitutedDominatorWorkstations[r] = compWs[comp[i]];
      guardTotal += guards.size();
    }
  }
}

}
