// Eager startup entry cuts for 1- and 2-cycles.
//
// The balance model is a net condition over the whole plan, so a cycle can pay
// for its own start on paper. The classic shape is a pair of recipes that
// consume each other's output with a net surplus somewhere:
//
//   A x64 <- B x16      r0
//   B x1  <- A x1       r1
//
// The pair is net positive in A (+63 per round), so `r0:2 / r1:32` balances a
// request of 64 A, but the first firing of either needs 16 B or 1 A and neither
// exists. The post-solve fireability check rejects that plan; this pass makes
// the solver produce a startable one in the first place.
//
// It does so without a witness, because the entry inequality holds for *any*
// set of columns: name the group's members, force some *used* member to be the
// first to fire, and make that one pay the group's gross inputs out of the
// stock plus the net output of everything outside the group. So the enumeration
// only has to name groups worth cutting, and it names the cheap ones: every
// self-loop and every group of recipes that consume each other's output and
// sum to a positive net entry. Those are the cycles the balance is happy to run
// from nothing, and on real graphs they are where the solver gets stuck.
// Longer cycles are left to the post-solve group, which knows the real
// deadlock.
//
// The members of one group are the connected component of the mutual-consumption
// relation, not one pair at a time. A hub with many interchangeable partners --
// a synthetic tag node and one edge per member is the common case -- would
// otherwise pass each single-pair cut through a sibling: the cut on (P, T1) can
// always be satisfied by firing T2, even though nothing can fire at all. One
// group per component removes the siblings from the funding sum too. A
// component above `maxMembers` is skipped, since the union of a whole
// conversion soup would be neither affordable nor informative.
//
// All of this is a *filter on relevance*, never on soundness. Cutting a cycle
// the solver would never have used costs a row; missing one only means a
// rejected solve, exactly as before.
//
// A two-member component also gets a *joint* seed cut, which weighs two rows
// against one demand. The entry group prices one firing at a time, one row at a
// time, and an amplifier pair passes it and still cannot start: `A x64 <- B x16`
// needs sixteen B, `B x1 <- A x1` needs one A, and a shelf with eight A answers
// the second while the first is what the cycle actually costs. See
// `addPairSeedCuts` for the derivation and `solver::Options::SeedCut` for what
// is posted.

#include "StartupCut.h"

#include <cstdint>
#include <span>

namespace aw {
namespace {

// Ceiling on the candidate pairs examined. The scan expands each input slot
// into its producers, so a tag with tens of thousands of members could blow up
// quadratically. Running out stops the enumeration, which only drops cuts.
constexpr uint64_t STARTUP_SCAN_BUDGET = 4'000'000;

// Merged gross inputs of the recipe last handed to `merge`, indexed by item. A
// recipe may list the same item more than once, and its first firing pays the
// sum.
//
// `current` is bumped on every call, so merging the same recipe twice is safe.
// A stamp of the recipe id would not be: the second call would find its own
// earlier stamp on every item and report no inputs at all.
struct InputMerger {
  aw::vector<Amount> gross;
  aw::vector<uint32_t> gen;
  aw::vector<ItemId> touched;
  uint32_t current = 0;

  explicit InputMerger(ItemId nItem) noexcept: gross(nItem, 0), gen(nItem, 0) {}

  void merge(const BaseCraftingGraph &g, RecipeId r) noexcept {
    touched.clear();
    current++;
    const auto ins = g.inputsOf(r);
    const auto amts = g.inputAmountsOf(r);
    for (size_t k = 0; k < ins.size(); k++) {
      const ItemId item = ins[k];
      if (gen[item] != current) {
        gen[item] = current;
        gross[item] = 0;
        touched.push_back(item);
      }
      gross[item] += amts[k];
    }
  }

  Amount amountOf(ItemId item) const noexcept {
    return gen[item] == current ? gross[item] : 0;
  }

  // One need per distinct input item of the merged recipe.
  void appendNeeds(RecipeId r, aw::vector<solver::Options::EntryNeed> &needs) const noexcept {
    for (ItemId item : touched)
      needs.push_back(solver::Options::EntryNeed{item, r, gross[item]});
  }
};

// Net column sums over a small set of recipes, indexed by item.
//
// `gen` distinguishes "not touched by this sum" from a sum that is legitimately
// zero, which a plain zero test cannot do.
struct NetSum {
  aw::vector<Amount> net;
  aw::vector<uint32_t> gen;
  aw::vector<ItemId> touched;
  uint32_t current = 0;

  explicit NetSum(ItemId nItem) noexcept: net(nItem, 0), gen(nItem, 0) {}

  void begin() noexcept {
    current++;
    touched.clear();
  }

  void addRecipe(const BaseCraftingGraph &g, RecipeId r) noexcept {
    const auto outs = g.outputsOf(r);
    const auto outAmts = g.outputAmountsOf(r);
    for (size_t k = 0; k < outs.size(); k++)
      bump(outs[k], outAmts[k]);
    const auto ins = g.inputsOf(r);
    const auto inAmts = g.inputAmountsOf(r);
    for (size_t k = 0; k < ins.size(); k++)
      bump(ins[k], -inAmts[k]);
  }

  // True when some item has a positive net, i.e. the summed column creates
  // something rather than only moving it around.
  bool anyPositive() const noexcept {
    for (ItemId item : touched)
      if (net[item] > 0)
        return true;
    return false;
  }

 private:
  void bump(ItemId item, Amount delta) noexcept {
    if (gen[item] != current) {
      gen[item] = current;
      net[item] = 0;
      touched.push_back(item);
    }
    net[item] += delta;
  }
};

// Disjoint sets over the recipes, for the mutual-consumption components.
struct DisjointSets {
  aw::vector<uint32_t> parent;

  explicit DisjointSets(uint32_t n) noexcept: parent(n) {
    for (uint32_t i = 0; i < n; i++)
      parent[i] = i;
  }

  uint32_t find(uint32_t node) noexcept {
    uint32_t root = node;
    while (parent[root] != root)
      root = parent[root];
    while (parent[node] != root) {
      const uint32_t next = parent[node];
      parent[node] = root;
      node = next;
    }
    return root;
  }

  void unite(uint32_t a, uint32_t b) noexcept {
    const uint32_t rootA = find(a);
    const uint32_t rootB = find(b);
    if (rootA != rootB)
      parent[rootB] = rootA;
  }
};

// Does `consumer` eat anything `producer` makes? That is the second half of a
// mutually-consuming pair.
bool consumesProduced(const BaseCraftingGraph &g, RecipeId consumer, RecipeId producer) noexcept {
  for (ItemId item : g.inputsOf(consumer))
    if (g.producedAmountOf(producer, item) > 0)
      return true;
  return false;
}

// Merged gross inputs of one recipe, copied out of `InputMerger`, so that both
// members of a cycle can be weighed at once: the merger only holds the recipe
// it was last handed. `items` and `amounts` are parallel.
struct InputSnapshot {
  aw::vector<ItemId> items;
  aw::vector<Amount> amounts;

  void take(const InputMerger &inputs) noexcept {
    items.assign(inputs.touched.begin(), inputs.touched.end());
    amounts.clear();
    amounts.reserve(items.size());
    for (ItemId item : items)
      amounts.push_back(inputs.amountOf(item));
  }
};

// `x * y` when both are positive and the product fits in int64, otherwise
// false. A cut that cannot be built is skipped, which only leaves the model
// weaker.
bool mulFits(Amount x, Amount y, int64_t &out) noexcept {
  if (x <= 0 || y <= 0 || x > INT64_MAX / y)
    return false;
  out = x * y;
  return true;
}

// Add `weight` on `row` to a cut's terms, merging with a term that already
// names the row. A cycle whose two recipes share an item puts that item on both
// sides of the derivation, and two separate terms would have it counted once.
void appendTerm(aw::vector<solver::Options::SeedTerm> &terms, ItemId row, Amount weight) noexcept {
  for (solver::Options::SeedTerm &term : terms)
    if (term.row == row) {
      term.weight += weight;
      return;
    }
  terms.push_back(solver::Options::SeedTerm{row, weight});
}

// The joint seed cuts of the ordered pair (a, b) of a two-recipe cycle: `a` eats
// `e` that `b` makes, and `b` eats `f` that `a` makes.
//
// The derivation is about the *first* firing of `a`. Let `m` be the number of
// `b` firings before it. Whatever is on the shelf of `e` then is at most
// `stock_e + out_e` plus what those `m` firings made of it, and `a`'s firing has
// to be paid out of it:
//
//   eIn(a) <= stock_e + out_e + m * eOut(b).                    (1)
//
// Each of those `m` firings ate `fIn(b)` units of `f`, and `a` has not fired
// yet, so all of that came from the shelf:
//
//   m * fIn(b) <= stock_f + out_f.                              (2)
//
// Eliminating `m` between the two (scale (2) by `eOut(b) / fIn(b)` and
// substitute into (1)) gives the integer inequality
//
//   fIn(b) * eIn(a) <= fIn(b) * (stock_e + out_e)
//                    + eOut(b) * (stock_f + out_f),
//
// posted with demand `fIn(b) * eIn(a)`, weight `fIn(b)` on `e` and weight
// `eOut(b)` on `f`, and triggered by `a` being used at all. The mirror image,
// derived from the first firing of `b` instead, has the same demand and weights
// `eIn(a)` on `f` and `fOut(a)` on `e`; the two items do not play symmetric
// roles, so both directions are worth posting.
//
// Two over-estimates keep it sound with no assumption about the plan: `stock +
// positive outside net` ignores whatever the outside world consumes, and both
// derivations ignore any `e` or `f` the partner's own firings return, which
// only happens for a recipe that eats what it makes. Under-stating the shelf
// can only weaken the cut, never make it wrong.
void addPairSeedCuts(const BaseCraftingGraph &g, RecipeId a, RecipeId b, InputMerger &inputs,
                     aw::vector<solver::Options::SeedCut> &out, uint32_t maxSeeds) noexcept {
  inputs.merge(g, a);
  InputSnapshot aIn;
  aIn.take(inputs);
  inputs.merge(g, b);
  InputSnapshot bIn;
  bIn.take(inputs);

  for (size_t i = 0; i < aIn.items.size(); i++) {
    const ItemId e = aIn.items[i];
    const Amount eIn = aIn.amounts[i];
    const Amount eOut = g.producedAmountOf(b, e);
    if (eOut <= 0)
      continue;
    for (size_t k = 0; k < bIn.items.size(); k++) {
      const ItemId f = bIn.items[k];
      const Amount fIn = bIn.amounts[k];
      const Amount fOut = g.producedAmountOf(a, f);
      if (fOut <= 0)
        continue;
      int64_t demand = 0;
      if (!mulFits(eIn, fIn, demand))
        continue;
      for (int direction = 0; direction < 2; direction++) {
        if (out.size() >= maxSeeds)
          return;
        solver::Options::SeedCut cut;
        cut.members.push_back(a);
        cut.members.push_back(b);
        cut.demand = demand;
        if (direction == 0) {
          cut.trigger = a;
          appendTerm(cut.terms, e, fIn);
          appendTerm(cut.terms, f, eOut);
        } else {
          cut.trigger = b;
          appendTerm(cut.terms, f, eIn);
          appendTerm(cut.terms, e, fOut);
        }
        out.push_back(std::move(cut));
      }
    }
  }
}

// `r` is a self-loop (it eats something it makes) whose column has a positive
// net entry, so the balance can run it without buying anything first. A
// self-loop with no positive entry only destroys value and the balance already
// keeps it small.
bool isGainSelfLoop(const BaseCraftingGraph &g, RecipeId r, const InputMerger &inputs) noexcept {
  const auto outs = g.outputsOf(r);
  const auto outAmts = g.outputAmountsOf(r);
  bool loop = false;
  bool gain = false;
  for (size_t k = 0; k < outs.size(); k++) {
    const Amount in = inputs.amountOf(outs[k]);
    if (in > 0)
      loop = true;
    if (outAmts[k] > in)
      gain = true;
  }
  return loop && gain;
}

// Union `r` with every partner it consumes from and feeds back into. The `p > r`
// guard handles each unordered pair once, since the relation is symmetric.
// Returns false when the scan budget runs out.
bool unitePartners(const BaseCraftingGraph &g, RecipeId r, const InputMerger &inputs,
                   DisjointSets &sets, aw::vector<int32_t> &pairSeen, NetSum &netSum,
                   uint64_t &scan) noexcept {
  for (ItemId item : inputs.touched) {
    for (RecipeId p : g.producersOf(item)) {
      if (p <= r || pairSeen[p] == (int32_t) r)
        continue;
      pairSeen[p] = (int32_t) r;
      if (++scan > STARTUP_SCAN_BUDGET)
        return false;
      if (!consumesProduced(g, p, r))
        continue;
      netSum.begin();
      netSum.addRecipe(g, r);
      netSum.addRecipe(g, p);
      if (netSum.anyPositive())
        sets.unite(r, p);
    }
  }
  return true;
}

}  // namespace

void buildStartupCuts(const Subgraph &sub, aw::vector<solver::Options::EntryGroup> &groups,
                      aw::vector<solver::Options::SeedCut> &seeds, uint32_t maxGroups,
                      uint32_t maxMembers) noexcept {
  const BaseCraftingGraph &g = sub.graph;
  const uint32_t n = g.nRecipe;
  const ItemId m = g.nItem;
  if (n == 0 || m == 0 || maxGroups == 0 || maxMembers == 0)
    return;

  DisjointSets sets(n);
  aw::vector<uint8_t> selfLoop = aw::vector<uint8_t>::zeroes(n);
  aw::vector<int32_t> pairSeen(n, -1);
  InputMerger inputs(m);
  NetSum netSum(m);
  uint64_t scan = 0;

  // Pass 1: the mutual-consumption components and the self-loops.
  for (RecipeId r = 0; r < n; r++) {
    inputs.merge(g, r);
    if (inputs.touched.empty())
      continue;
    selfLoop[r] = isGainSelfLoop(g, r, inputs) ? 1 : 0;
    if (!unitePartners(g, r, inputs, sets, pairSeen, netSum, scan))
      break;  // partial components: fewer cuts, never a wrong one
  }

  // A component is a cycle worth cutting when it has two members or a self-loop.
  // A lone column with neither is just a recipe.
  aw::vector<uint32_t> memberCount(n, 0);
  aw::vector<uint8_t> qualifies = aw::vector<uint8_t>::zeroes(n);
  for (RecipeId r = 0; r < n; r++)
    memberCount[sets.find(r)]++;
  for (RecipeId r = 0; r < n; r++) {
    const uint32_t root = sets.find(r);
    if (memberCount[root] >= 2 || selfLoop[r])
      qualifies[root] = 1;
  }

  // Gather the members of the qualifying components by root.
  aw::vector<uint32_t> offset(n + 1, 0);
  for (uint32_t root = 0; root < n; root++)
    offset[root + 1] = offset[root] + (qualifies[root] ? memberCount[root] : 0);
  aw::vector<uint32_t> members(offset[n]);
  aw::vector<uint32_t> cursor(offset.begin(), offset.end() - 1);
  for (RecipeId r = 0; r < n; r++) {
    const uint32_t root = sets.find(r);
    if (qualifies[root])
      members[cursor[root]++] = r;
  }

  // Pass 2: one group per component, with the gross inputs of every member,
  // plus the joint seed cut of the two-member ones. The two lists have their
  // own budgets, so one running out does not stop the other.
  for (uint32_t root = 0; root < n; root++) {
    if (!qualifies[root] || memberCount[root] > maxMembers)
      continue;
    const uint32_t first = offset[root];
    const uint32_t count = memberCount[root];
    if (groups.size() < maxGroups) {
      solver::Options::EntryGroup group;
      group.columns.reserve(count);
      group.needs.reserve(count);
      for (uint32_t k = first; k < first + count; k++) {
        const RecipeId r = members[k];
        group.columns.push_back(r);
        inputs.merge(g, r);
        inputs.appendNeeds(r, group.needs);
      }
      groups.push_back(std::move(group));
    }
    if (count == 2)
      addPairSeedCuts(g, members[first], members[first + 1], inputs, seeds, maxGroups);
  }
}

}  // namespace aw
