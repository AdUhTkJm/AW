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

void buildStartupCuts(const Subgraph &sub, aw::vector<solver::Options::EntryGroup> &out,
                      uint32_t maxGroups, uint32_t maxMembers) noexcept {
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

  // Pass 2: one group per component, with the gross inputs of every member.
  for (uint32_t root = 0; root < n; root++) {
    if (!qualifies[root] || memberCount[root] > maxMembers)
      continue;
    if (out.size() >= maxGroups)
      return;
    solver::Options::EntryGroup group;
    group.columns.reserve(memberCount[root]);
    group.needs.reserve(memberCount[root]);
    for (uint32_t k = offset[root]; k < offset[root + 1]; k++) {
      const RecipeId r = members[k];
      group.columns.push_back(r);
      inputs.merge(g, r);
      inputs.appendNeeds(r, group.needs);
    }
    out.push_back(std::move(group));
  }
}

}  // namespace aw
