// Post-solve tidying of tag conversions.
//
// A tag edge is free (see BaseCraftingGraph::cost) and a tag's balance row is
// only `produced(T) - consumed(T) >= b_T`, so nothing pushes the solver to keep
// a tag's production tight: it may leave every member edge at its domain cap at
// no cost. CP-SAT does exactly that on a large pack -- the plan for one ATM10
// item lists about 89700 conversions of items the plan never consumes, and no
// member edge is consumed even once -- so the response carries thousands of
// steps that do not happen in game.
//
// This pass cuts a tag's member edges back to what the plan actually consumes.
// Letting `e` range over the member edges of the tag T, and `y_e` be the count
// the trim keeps, the row reads
//
//   sum_e outputAmt_e * y_e + (production of T outside tag edges) >= consumed(T) - b_T
//
// so the member edges only have to cover `consumed(T) - b_T`, less whatever a
// real recipe produces of T on the side. `b_T` is 0 for a pseudo-resource that
// has no stock and is never the target; the general form is used anyway.
//
// Every `y_e` is capped at the solver's own count `x_e`. That makes each cut a
// pure reduction: no member of a tag is consumed more than before the trim, so
// balance rows other than the tag's cannot turn infeasible, and the tag's own
// row is what the requirement above already covers. Real recipes are never
// touched, so the objective the solver proved stays the number of real
// executions the plan reports.
//
// Which member edges keep the conversions is a free choice, and this pass
// settles it with a keep-first order: the demand is filled from the front and
// the tail is zeroed. Stocked members come first because a stocked member is
// available at time zero, so a kept conversion on one can be the entry of a
// cycle; then members the plan itself produces, so a kept conversion does not
// strand the output of a real recipe; then the largest count, which keeps the
// number of distinct conversions down. See `keepBefore`.
//
// The trim is still best effort. Cutting the conversions of a tag consumed
// inside a cycle can leave the group unable to start, and whether it does
// depends on which member edges were kept -- a stocked member is always a safe
// entry, an unstocked one is not. The pass therefore re-runs the fireability
// check `planCrafting` uses and reports nothing when the trimmed vector does
// not fire, so the caller can keep the solver's own answer. Correctness never
// depends on this file.

#include "aw/plan/Plan.h"

#include <algorithm>
#include <cstdint>
#include <span>

#include "aw/plan/Options.h"
#include "aw/utils/Helpers.h"

namespace aw {
namespace {

// Saturating helpers for the gross totals below. Every one of them is a sum of
// `amount * executions` over a plan, which fits in int64 for any plan that fits
// in memory; a saturated total only makes the pass decline to trim, because a
// requirement that cannot be computed exactly is left alone.
int64_t satAdd(int64_t lhs, int64_t rhs) noexcept {
  int64_t sum = 0;
  return addOverflow(lhs, rhs, sum) || sum < 0 ? INT64_MAX : sum;
}

int64_t satMul(int64_t lhs, int64_t rhs) noexcept {
  int64_t product = 0;
  return mulOverflow(lhs, rhs, product) || product < 0 ? INT64_MAX : product;
}

// `lhs - rhs`, floored at 0. Every use is a demand less something that can only
// lower it, so a negative difference is simply no demand.
int64_t satSub(int64_t lhs, int64_t rhs) noexcept {
  return rhs > lhs ? 0 : lhs - rhs;
}

int64_t ceilDiv(int64_t numerator, int64_t denominator) noexcept {
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// A synthetic member edge: `T <- m`, one real item in, one tag out, free.
//
// Anything else is left alone. A weighted or multi-input edge would not fit the
// per-edge arithmetic below, an edge with a byproduct must not be trimmed at all
// (dropping its executions would drop that byproduct's production with them,
// without a row to pay for it), and a non-positive output amount is malformed
// rather than a conversion. The reader only ever emits the simple shape, so this
// is a guard against a malformed blob rather than a real case.
bool isTagEdge(const BaseCraftingGraph &g, RecipeId r) noexcept {
  if (g.output[r] < g.nReal || g.cost[r] != 0 || g.outputAmt[r] <= 0)
    return false;
  if (g.outputsOf(r).size() != 1 || g.inputsOf(r).size() != 1)
    return false;
  return g.inputsOf(r)[0] < g.nReal && g.inputAmountsOf(r)[0] == 1;
}

// Gross amounts the plan moves, one entry per subgraph item.
struct PlanTotals {
  // Everything the plan produces of an item, from every recipe.
  aw::vector<int64_t> produced;
  // Everything the plan consumes of an item, from every recipe.
  aw::vector<int64_t> consumed;
  // The part of `produced` that comes from the tag edges this pass may cut.
  // `produced[t] - tagProduced[t]` is the production a trim cannot change.
  aw::vector<int64_t> tagProduced;
};

PlanTotals planTotals(const BaseCraftingGraph &g, std::span<const int64_t> exec) noexcept {
  PlanTotals totals;
  totals.produced.assign(g.nItem, 0);
  totals.consumed.assign(g.nItem, 0);
  totals.tagProduced.assign(g.nItem, 0);

  for (RecipeId r = 0; r < g.nRecipe; r++) {
    const int64_t times = exec[r];
    if (times <= 0)
      continue;
    const bool tagEdge = isTagEdge(g, r);
    const auto outputs = g.outputsOf(r);
    const auto outputAmounts = g.outputAmountsOf(r);
    for (size_t k = 0; k < outputs.size(); k++) {
      const int64_t amount = satMul(outputAmounts[k], times);
      totals.produced[outputs[k]] = satAdd(totals.produced[outputs[k]], amount);
      if (tagEdge)
        totals.tagProduced[outputs[k]] = satAdd(totals.tagProduced[outputs[k]], amount);
    }
    const auto inputs = g.inputsOf(r);
    const auto weights = g.inputAmountsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      totals.consumed[inputs[k]] =
          satAdd(totals.consumed[inputs[k]], satMul(weights[k], times));
  }
  return totals;
}

// The production a tag's member edges still have to cover: its gross
// consumption, less the stock the player already holds, less what recipes other
// than tag edges produce of it. The solver's row for the tag is
// `produced - consumed >= b`, so this is exactly the member edges' share of it.
int64_t tagRequirement(const PlanTotals &totals, std::span<const int64_t> b,
                       ItemId tag) noexcept {
  const int64_t fixed = satSub(totals.produced[tag], totals.tagProduced[tag]);
  const int64_t demand = satSub(totals.consumed[tag], fixed);
  if (b[tag] < 0)
    return satSub(demand, b[tag] == INT64_MIN ? INT64_MAX : -b[tag]);
  return satAdd(demand, b[tag]);
}

// One member edge of a tag, with what the keep-first order needs to rank it.
struct MemberEdge {
  RecipeId recipe = 0;
  // Executions the solver chose. The trim only ever lowers this.
  int64_t count = 0;
  // Tag units one execution produces.
  Amount outputAmt = 1;
  // Units of the member the player holds before the plan runs.
  int64_t stock = 0;
  // Units of the member the plan itself produces.
  int64_t produced = 0;
};

// Keep-first order. The demand is filled from the front, so whatever is first
// here is what the response shows.
bool keepBefore(const MemberEdge &lhs, const MemberEdge &rhs) noexcept {
  // Stock first: available at time zero, so a kept conversion on it can start a
  // cycle that the tag is part of.
  const bool lhsStocked = lhs.stock > 0;
  const bool rhsStocked = rhs.stock > 0;
  if (lhsStocked != rhsStocked)
    return lhsStocked;
  if (lhs.stock != rhs.stock)
    return lhs.stock > rhs.stock;
  // Then what the plan produces anyway, so the trim does not strand it.
  const bool lhsMade = lhs.produced > 0;
  const bool rhsMade = rhs.produced > 0;
  if (lhsMade != rhsMade)
    return lhsMade;
  if (lhs.produced != rhs.produced)
    return lhs.produced > rhs.produced;
  // Then the larger count, which tends to put the whole demand on one edge.
  if (lhs.count != rhs.count)
    return lhs.count > rhs.count;
  return lhs.recipe < rhs.recipe;
}

// Fills `requirement` units of tag production from the front of `edges` and
// zeroes the rest, writing the kept counts into `out`. Returns true when some
// count changed. `edges` must be sorted keep-first and hold every tag producer
// of one tag with a positive count.
bool trimTag(const MemberEdge *edges, size_t edgeCount, int64_t requirement,
             aw::vector<int64_t> &out) noexcept {
  int64_t remaining = requirement;
  bool changed = false;
  for (size_t k = 0; k < edgeCount; k++) {
    const MemberEdge &edge = edges[k];
    int64_t keep = 0;
    if (remaining > 0) {
      const int64_t capacity = satMul(edge.count, edge.outputAmt);
      if (remaining >= capacity) {
        // The edge is entirely consumed by the demand: keep all of it.
        keep = edge.count;
        remaining -= capacity;
      } else {
        // `remaining < count * outputAmt`, so the ceiling division does not
        // reach past what the solver fired.
        keep = std::min(edge.count, ceilDiv(remaining, edge.outputAmt));
        remaining = 0;
      }
    }
    if (keep != edge.count) {
      out[edge.recipe] = keep;
      changed = true;
    }
  }
  return changed;
}

// The member edges of `tag` that the solver fired, in keep-first order.
void collectMemberEdges(const Subgraph &sub, const PlanTotals &totals,
                        std::span<const Amount> invSrc, std::span<const int64_t> exec,
                        ItemId tag, aw::vector<MemberEdge> &edges) {
  const BaseCraftingGraph &g = sub.graph;
  edges.clear();
  for (RecipeId r : g.producersOf(tag)) {
    if (exec[r] <= 0 || !isTagEdge(g, r))
      continue;
    const ItemId member = g.inputsOf(r)[0];
    const ItemId source = sub.itemOrigin[member];
    MemberEdge edge;
    edge.recipe = r;
    edge.count = exec[r];
    edge.outputAmt = g.outputAmt[r];
    edge.stock = source < invSrc.size() ? invSrc[source] : 0;
    edge.produced = totals.produced[member];
    edges.push_back(edge);
  }
  std::sort(edges.begin(), edges.end(), keepBefore);
}

}  // namespace

aw::vector<int64_t> tidyTagConversions(const Subgraph &sub, std::span<const Amount> invSrc,
                                       std::span<const int64_t> b,
                                       std::span<const int64_t> exec) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  if (!options.tagTidy || g.nRecipe == 0 || exec.size() != g.nRecipe || b.size() != g.nItem)
    return {};

  const PlanTotals totals = planTotals(g, exec);

  // Real recipes are a prefix of the recipe list, so the tags are the items
  // above `nReal`. Only a tag that the plan produces more of than it consumes
  // (or holds stock of) has anything to cut.
  bool surplus = false;
  for (ItemId tag = g.nReal; tag < g.nItem && !surplus; tag++) {
    if (totals.tagProduced[tag] == 0)
      continue;
    surplus = totals.tagProduced[tag] > tagRequirement(totals, b, tag);
  }
  if (!surplus)
    return {};

  aw::vector<int64_t> out(exec.begin(), exec.end());
  aw::vector<MemberEdge> edges;
  bool changed = false;
  for (ItemId tag = g.nReal; tag < g.nItem; tag++) {
    if (totals.tagProduced[tag] == 0)
      continue;
    const int64_t requirement = tagRequirement(totals, b, tag);
    if (totals.tagProduced[tag] <= requirement)
      continue;
    collectMemberEdges(sub, totals, invSrc, exec, tag, edges);
    changed |= trimTag(edges.data(), edges.size(), requirement, out);
  }
  if (!changed)
    return {};

  // A trim can only lower counts, but lowering the wrong member edge can leave
  // a cyclic group without an entry. Hand the caller the solver's own vector
  // rather than a plan that no longer fires.
  if (!planIsFireable(sub, invSrc, std::span<const int64_t>(out.data(), out.size()), nullptr))
    return {};
  return out;
}

}  // namespace aw
