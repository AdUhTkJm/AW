#include "Prune.h"
#include "aw/plan/Options.h"
#include "aw/utils/Helpers.h"
#include <cstdio>

namespace aw::detail {

FILE *f = fopen("temp/a.txt", "w");

// NOTE: This is linked against other translation units.
// We don't use it in this particular file, but we cannot delete it.
bool leVector(std::span<const ItemId> ci, std::span<const Amount> cc,
              std::span<const ItemId> si, std::span<const Amount> sc) noexcept {
  size_t i = 0, j = 0;
  const size_t c_size = ci.size();
  const size_t s_size = si.size();

  // Shared part, where both sides have elements.
  while (i < c_size && j < s_size) {
    const ItemId c_id = ci[i];
    const ItemId s_id = si[j];

    if (c_id == s_id) {
      if (cc[i] > sc[j])
        return false;
      ++i;
      ++j;
    } else if (c_id < s_id) {
      if (cc[i] > 0)
        return false;
      ++i;
    } else {
      if (sc[j] < 0)
        return false;
      ++j;
    }
  }

  // Now the remaining items for c and s.
  while (i < c_size) {
    if (cc[i] > 0)
      return false;
    ++i;
  }
  while (j < s_size) {
    if (sc[j] < 0)
      return false;
    ++j;
  }

  return true;
}

[[gnu::always_inline]]
inline bool leVectorRaw(const ItemId *ci, const Amount *cc, size_t c_size,
                 const ItemId *si, const Amount *sc, size_t s_size) noexcept {
  size_t i = 0, j = 0;

  // Shared part, where both sides have elements.
  while (i < c_size && j < s_size) {
    const ItemId cid = ci[i];
    const ItemId sid = si[j];

    if (cid == sid) {
      if (cc[i] > sc[j])
        return false;
      ++i;
      ++j;
    } else if (cid < sid) {
      if (cc[i] > 0)
        return false;
      ++i;
    } else {
      if (sc[j] < 0)
        return false;
      ++j;
    }
  }

  // Now the remaining items for c and s.
  while (i < c_size) {
    if (cc[i] > 0)
      return false;
    ++i;
  }
  while (j < s_size) {
    if (sc[j] < 0)
      return false;
    ++j;
  }

  return true;
}

bool leZero(std::span<const Amount> cc) noexcept {
  for (Amount c : cc)
    if (c > 0)
      return false;
  return true;
}

// Builds `c = alpha * v_r + v_R`, sorted and unique, into `outItems` / `outCoeffs`.
// Returns false on integer overflow, which is treated as "cannot prove
// dominance" by the caller.
bool buildComposite(int64_t alpha, const RecipeVectors &vec, uint r, uint R,
                    aw::vector<ItemId> &outItems,
                    aw::vector<Amount> &outCoeffs) noexcept {
  const auto ri = vec.itemsOf(r);
  const auto rc = vec.coeffsOf(r);
  const auto Ri = vec.itemsOf(R);
  const auto Rc = vec.coeffsOf(R);
  outItems.clear();
  outCoeffs.clear();

  size_t i = 0, j = 0;
  while (i < ri.size() || j < Ri.size()) {
    ItemId next = UINT32_MAX;
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
void computeRecipePruning(CraftingGraph &graph, const RecipeVectors &vec) noexcept {
  graph.recipeDominated.assign(graph.nRecipe, 0);
  graph.recipeGuardInput.assign(graph.nRecipe, UINT32_MAX);
  graph.recipeDominatorWorkstations.assign(graph.nRecipe, {});

  const uint nReal = graph.nReal;
  if (nReal == 0 || graph.nRecipe == 0)
    return;

  aw::vector<uint32_t> recs;
  aw::vector<aw::vector<uint32_t>> adj;
  aw::vector<ItemId> guard;

  aw::vector<ItemId> cItems;
  aw::vector<Amount> cCoeffs;
  aw::vector<uint32_t> candidates;
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
    // Comparing every pair of X's recipes costs O(k^2) time and can build a
    // k^2-edge adjacency. Leave an item with an absurd fan-out unpruned rather
    // than spend gigabytes on it.
    if (siblings.size() > options.maxSiblingRecipes)
      continue;

    recs.clear();
    recs.reserve(siblings.size());
    for (RecipeId r : siblings)
      recs.push_back_unchecked(r);
    const uint k = (uint) recs.size();

    adj.assign(k, {});
    guard.assign(k, UINT32_MAX);
    candidates.reserve(k);

    for (uint i = 0; i < k; i++) {
      const uint R = recs[i];
      const auto inputs = graph.inputsOf(R);
      const auto weights = graph.inputAmountsOf(R);
      for (size_t a = 0; a < inputs.size(); a++) {
        const ItemId Y = inputs[a];
        if (!graph.isRealItem(Y))
          continue;
        const Amount q = weights[a];
        const auto producers = graph.producersOf(Y);

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

        for (RecipeId r : producers) {
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
            auto sBegin = vec.items.data() + vec.offsets[S];
            auto sCoeffBegin = vec.coeffs.data() + vec.offsets[S];
            const auto size = vec.offsets[S + 1] - vec.offsets[S];
            if (loss || leVectorRaw(cItems.data(), cCoeffs.data(), cItems.size(), sBegin, sCoeffBegin, size))
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

}