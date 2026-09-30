#include "Prune.h"
#include "aw/plan/Options.h"
#include <algorithm>

#define CHECK_WORK [[unlikely]] if (work == 0) break

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

// Turns a count vector into a CSR offset vector in place.
void prefixSum(aw::vector<uint32_t> &v) noexcept {
  v.insert(v.begin(), 0);
  for (uint32_t i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
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

// The cost relation. `costs(y, w)` is true when every way of producing one
// unit of y consumes at least one unit of w. A simple tag is the AND over its
// members; a real item is the AND over its recipes, each of which needs one
// amount-qualified input that is w or itself costs w. Cycles are cut, so the
// relation is a least fixpoint and its true set is small.
//
// No step of the recursion changes the witness w, so the relation factorises
// into independent rows, one per w. The pass solves a row by propagating
// upward from w instead of searching downward from every query. Only pairs
// that are actually true are visited, so the (large) negative bulk of the
// relation is never materialised. The closure is:
//
//   * w enters its row at level 0;
//   * when an item enters, every real recipe consuming it as a qualified input
//     becomes satisfied -- the recursion needs only one such input per recipe;
//   * a real item enters once all of its recipes are satisfied, an eligible
//     simple tag once all of its members have entered;
//   * the level counts derivation height, so stopping at options.maxCostDepth
//     is exactly the old depth cap.
//
// A row is computed once, kept as its sorted true-member list, and reused by
// every later query with the same witness. The top-down version instead had to
// memoise every false pair it visited, which on the large packs was millions of
// entries against a few thousand true ones.
struct CostContext {
  const CraftingGraph *graph = nullptr;
  aw::vector<aw::vector<NodeId>> tagMembers;

  // member -> real recipes that consume it as an amount-qualified input. A
  // synthetic tag recipe is left out: a tag is the AND over its members, not
  // over its own recipe.
  aw::vector<uint32_t> qualOffsets;   // nItem + 1
  aw::vector<uint32_t> qualRecipes;   // E
  // Real member -> eligible simple tags containing it.
  aw::vector<uint32_t> memberTagOffsets;  // nReal + 1
  aw::vector<uint32_t> memberTagTargets;
  aw::vector<uint32_t> recipeCount;   // nReal
  aw::vector<uint8_t> realEligible;   // nReal

  // Scratch of the row being computed, stamped with `gen` so it is reset for
  // free.
  aw::vector<uint32_t> stamp;         // nItem
  aw::vector<uint32_t> level;         // nItem
  aw::vector<uint32_t> recipeSat;     // nRecipe
  aw::vector<uint32_t> rrStamp;       // nReal
  aw::vector<uint32_t> rrLeft;        // nReal
  aw::vector<uint32_t> rmStamp;       // nItem
  aw::vector<uint32_t> rmLeft;        // nItem
  aw::vector<NodeId> queue;
  aw::vector<NodeId> *curRow = nullptr;
  uint32_t gen = 0;

  // Solver rows: the sorted true members of each witness already solved.
  aw::vector<uint8_t> rowDone;             // nItem
  aw::vector<aw::vector<NodeId>> rowTrue;  // nItem

  // Temporary vectors, extracted to avoid repeated vector construction/destruction.
  aw::vector<NodeId> tmpItems, nextItems;
  aw::vector<Amount> tmpCoeffs, nextCoeffs;

  uint64_t work;

  CostContext(const CraftingGraph &graph, uint64_t work)
      : graph(&graph), work(work) {}

  void build(bool requireUnit, const aw::vector<uint8_t> &unitOutput) noexcept;
};

// Precomputes the two inverted indices the row closure walks: recipes per
// amount-qualified input, and eligible simple tags per real member.
void CostContext::build(bool requireUnit, const aw::vector<uint8_t> &unitOutput) noexcept {
  const CraftingGraph &g = *graph;

  aw::vector<uint8_t> tagEligible(g.nItem, 0);
  for (NodeId t = g.nReal; t < g.nItem; t++) {
    const aw::vector<NodeId> &ms = tagMembers[t];
    if (ms.empty())
      continue;
    bool ok = true;
    for (NodeId z : ms) {
      if (requireUnit && !unitOutput[z]) {
        ok = false;
        break;
      }
    }
    tagEligible[t] = ok;
  }

  // Recipes per amount-qualified input. Skip tags.
  qualOffsets.assign(g.nItem, 0);
  for (uint r = 0; r < g.nRecipe; r++) {
    if (g.output[r] >= g.nReal)
      continue;
    const Amount out = g.outputAmt[r];
    const auto inputs = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++) {
      if (weights[k] >= out)
        qualOffsets[inputs[k]]++;
    }
  }
  prefixSum(qualOffsets);
  qualRecipes.resize(qualOffsets.back());
  aw::vector<uint32_t> cursor(qualOffsets.begin(), qualOffsets.end() - 1);
  for (uint r = 0; r < g.nRecipe; r++) {
    if (g.output[r] >= g.nReal)
      continue;
    const Amount out = g.outputAmt[r];
    const auto inputs = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++) {
      if (weights[k] >= out)
        qualRecipes[cursor[inputs[k]]++] = r;
    }
  }

  // Eligible tags per real member.
  aw::vector<uint32_t> counts(g.nReal, 0);
  for (NodeId t = g.nReal; t < g.nItem; t++) {
    if (tagEligible[t])
      for (NodeId z : tagMembers[t])
        counts[z]++;
  }

  memberTagOffsets = counts;
  prefixSum(memberTagOffsets);
  memberTagTargets.resize(memberTagOffsets.back());
  cursor.assign(memberTagOffsets.begin(), memberTagOffsets.end() - 1);
  for (NodeId t = g.nReal; t < g.nItem; t++) {
    if (tagEligible[t])
      for (NodeId z : tagMembers[t])
        memberTagTargets[cursor[z]++] = t;
  }

  recipeCount.assign(g.nReal, 0);
  realEligible.assign(g.nReal, 0);
  for (NodeId y = 0; y < g.nReal; y++) {
    recipeCount[y] = (uint32_t) g.i2r.targetsOf(y).size();
    realEligible[y] = !requireUnit || unitOutput[y] != 0;
  }

  stamp.assign(g.nItem, 0);
  level.assign(g.nItem, 0);
  recipeSat.assign(g.nRecipe, 0);
  rrStamp.assign(g.nReal, 0);
  rrLeft.assign(g.nReal, 0);
  rmStamp.assign(g.nItem, 0);
  rmLeft.assign(g.nItem, 0);
  rowDone.assign(g.nItem, 0);
  rowTrue.assign(g.nItem, {});
}

// Adds x to the row being solved, at the given derivation level. Items already
// in the row keep the level of their first (shallowest) arrival.
void addRowItem(CostContext &ctx, NodeId x, uint32_t level) noexcept {
  if (ctx.stamp[x] == ctx.gen)
    return;
  ctx.stamp[x] = ctx.gen;
  ctx.level[x] = level;
  ctx.queue.push_back(x);
  ctx.curRow->push_back(x);
}

// Solves the row of witness w by upward closure. Every queue and work step
// that runs out of budget stops the closure early; the row is then left
// incomplete, which can only report fewer costs and so prunes less.
void computeRow(CostContext &ctx, NodeId w) noexcept {
  ctx.rowDone[w] = 1;
  aw::vector<NodeId> &row = ctx.rowTrue[w];
  row.clear();
  ctx.curRow = &row;
  ctx.gen++;
  ctx.queue.clear();
  ctx.stamp[w] = ctx.gen;
  ctx.level[w] = 0;
  ctx.queue.push_back(w);

  const CraftingGraph &g = *ctx.graph;
  for (size_t head = 0; head < ctx.queue.size(); head++) {
    if (ctx.work == 0)
      return;
    
    ctx.work--;
    const NodeId x = ctx.queue[head];
    const uint32_t lx = ctx.level[x];
    if (lx >= options.maxCostDepth)
      continue;
    const uint32_t next = lx + 1;

    // Any qualified input of a recipe satisfies that recipe outright.
    for (uint32_t e = ctx.qualOffsets[x]; e < ctx.qualOffsets[x + 1]; e++) {
      if (ctx.work == 0)
        return;
      
      ctx.work--;
      const uint32_t r = ctx.qualRecipes[e];
      if (ctx.recipeSat[r] == ctx.gen)
        continue;
      ctx.recipeSat[r] = ctx.gen;
      const NodeId y = g.output[r];
      if (ctx.rrStamp[y] != ctx.gen) {
        ctx.rrStamp[y] = ctx.gen;
        ctx.rrLeft[y] = ctx.recipeCount[y];
      }
      if (ctx.rrLeft[y] == 0)
        continue;
      ctx.rrLeft[y]--;
      if (ctx.rrLeft[y] == 0 && ctx.realEligible[y])
        addRowItem(ctx, y, next);
    }

    // A tag needs every member, so a real member only moves its counter.
    if (x >= g.nReal)
      continue;
    for (uint32_t e = ctx.memberTagOffsets[x]; e < ctx.memberTagOffsets[x + 1]; e++) {
      if (ctx.work == 0)
        return;
      
      ctx.work--;
      const NodeId t = ctx.memberTagTargets[e];
      if (ctx.rmStamp[t] != ctx.gen) {
        ctx.rmStamp[t] = ctx.gen;
        ctx.rmLeft[t] = (uint32_t) ctx.tagMembers[t].size();
      }
      if (ctx.rmLeft[t] == 0)
        continue;
      ctx.rmLeft[t]--;
      if (ctx.rmLeft[t] == 0)
        addRowItem(ctx, t, next);
    }
  }
  std::sort(row.begin(), row.end());
}

// Resolves one cost query against the row of w, computing that row on first
// use.
bool costs(CostContext &ctx, NodeId y, NodeId w) noexcept {
  if (y == w)
    return true;
  if (!ctx.rowDone[w])
    computeRow(ctx, w);
  const aw::vector<NodeId> &row = ctx.rowTrue[w];
  return std::binary_search(row.begin(), row.end(), y);
}

// True when the column `cur` can be raised above the sibling column `s` by
// moving demand from some of `cur`'s inputs onto a cost dominator. `collapses`
// receives the inputs that were moved. The first row where `cur` exceeds `s` is
// covered by moving `min(q_Y, deficit)` units from an input Y with
// `costs(Y, d)`, then the next deficit is handled the same way.
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

  ctx.tmpItems.clear();
  ctx.nextCoeffs.clear();
  for (size_t k = 0; k < cur.size(); k++) {
    const NodeId y = cur.items[k];
    const Amount coeff = cur.coeffs[k];
    if (coeff >= 0 || y == d)
      continue;
    if (!costs(ctx, y, d))
      continue;
    Amount t = std::min(-coeff, deficit);
    // Moving t units of y onto d must not push y's own row into deficit.
    const Amount room = s.coeff(y) - coeff;
    if (t > room)
      t = room;
    if (t <= 0)
      continue;

    addToColumn(cur, y, t, ctx.tmpItems, ctx.tmpCoeffs);
    addToColumn(ColumnView(ctx.tmpItems, ctx.tmpCoeffs), d, -t, ctx.nextItems, ctx.nextCoeffs);

    const size_t before = collapses.size();
    collapses.push_back_unchecked(y);
    if (substitutionDominates(ctx, ColumnView(ctx.nextItems, ctx.nextCoeffs), s,
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

  // Collect tags and members. TODO: can we reuse it from tag prune?
  aw::vector<aw::vector<NodeId>> tagMembers(nItem);
  for (NodeId t = nReal; t < nItem; t++) {
    const auto recipes = graph.i2r.targetsOf(t);
    if (recipes.empty())
      continue;
    aw::vector<NodeId> ms;
    ms.reserve(recipes.size());
    for (NodeId recipeNode : recipes) {
      const uint r = recipeNode - graph.nItem;
      const auto inputs = graph.r2i.targetsOf(r);
      ms.push_back_unchecked(inputs[0]);
    }
    std::sort(ms.begin(), ms.end());
    ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
    // A catch-all tag is neither worth a per-recipe guard list nor cheap to
    // walk, so it is left out of the cost relation entirely.
    if (ms.size() > options.maxSubstitutionGuardItems)
      continue;
    tagMembers[t] = std::move(ms);
  }

  // Batching guard on exact mode.
  aw::vector<uint8_t> unitOutput;
  if (!options.nonoptimal) {
    unitOutput.assign(nItem, 1);
    for (NodeId m = 0; m < nReal; m++) {
      for (NodeId recipeNode : graph.i2r.targetsOf(m)) {
        if (graph.outputAmt[recipeNode - graph.nItem] != 1) {
          unitOutput[m] = 0;
          break;
        }
      }
    }
  }

  CostContext ctx(graph, options.maxSubstitutionCostWork);
  ctx.tagMembers = std::move(tagMembers);
  ctx.build(!options.nonoptimal, unitOutput);

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

  recs.reserve(options.maxSiblingRecipes);
  for (NodeId X = 0; X < nReal; X++) {
    CHECK_WORK;
    const auto recipeNodes = graph.i2r.targetsOf(X);
    if (recipeNodes.size() < 2)
      continue;
    if (recipeNodes.size() > options.maxSiblingRecipes)
      continue;

    recs.clear();
    for (NodeId recipeNode : recipeNodes)
      recs.push_back_unchecked(recipeNode - graph.nItem);
    const uint k = recs.size();

    adj.assign(k, {});
    edgeGuards.assign(k, {});

    for (uint i = 0; i < k; i++) {
      CHECK_WORK;
      const uint R = recs[i];
      const auto uItems = vec.itemsOf(R);
      const auto uCoeffs = vec.coeffsOf(R);
      for (uint j = 0; j < k; j++) {
        CHECK_WORK;
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
