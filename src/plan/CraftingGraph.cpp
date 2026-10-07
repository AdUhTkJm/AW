#include "aw/plan/Options.h"
#include "aw/plan/ProfileStep.h"
#include "Prune.h"

#include <algorithm>
#include <array>
#include <climits>
#include <utility>


namespace aw {
CraftingGraph graph;
// Last parse error, or nullptr when the last registerCraftingGraph succeeded.
// The Java side reads this to turn a malformed blob into an exception.
const char *error;

namespace {

constexpr std::array<uint8_t, 3> kMagic = {'A', 'W', 'R'};
// Schema version, the fourth magic byte. v1 is a single anchor output per
// recipe. v2 adds a byproduct list, which is how a real multi-output recipe is
// represented without splitting it into independent single-output copies. v3
// adds the per-recipe cost, which is what lets a chanced recipe be folded into
// a batch of executions without the planner counting the batch as one step.
constexpr uint8_t kSchemaV1 = 0x01;
constexpr uint8_t kSchemaV2 = 0x02;
constexpr uint8_t kSchemaV3 = 0x03;

// The cost of a real recipe has to be at least one execution (a free real
// column would be unbounded under the objective cap), and an int is all the
// Java generator can produce. Anything outside that range is a corrupt blob.
constexpr int64_t kMaxRecipeCost = INT_MAX;

#define fail(msg, ...) { error = msg; return __VA_ARGS__; }

using uint = uint32_t;
using ulong = uint64_t;

class ByteReader {
  std::span<const std::byte> bytes;
  size_t pos = 0;
public:
  explicit ByteReader(std::span<const std::byte> bytes) noexcept: bytes(bytes) {}

  [[nodiscard]]
  bool atEnd() const noexcept { 
    return pos >= bytes.size();
  }

  uint8_t readByte() noexcept {
    if (atEnd())
      fail("unexpected EOF", 0);

    return std::to_integer<uint8_t>(bytes[pos++]);
  }

  uint readVarInt() noexcept {
    uint value = 0;
    for (size_t i = 0; i < 5; i++) {
      uint8_t next = readByte();
      value |= ((uint) (next & 0x7F)) << (7 * i);
      if ((next & 0x80) == 0)
        return value;
    }
    fail("varint longer than 5 bytes", 0);
  }

  int64_t readVarLong() noexcept {
    int64_t value = 0;
    for (size_t i = 0; i < 9; i++) {
      uint8_t next = readByte();
      if (i == 9 && (next & 0xFE) != 0)
        fail("varlong overflows 64 bits", 0);

      value |= ((int64_t) (next & 0x7F)) << (7 * i);
      if ((next & 0x80) == 0)
        return value;
    }
    fail("varlong longer than 9 bytes", 0);
  }

  uint readHandle() noexcept {
    uint handle = readVarInt();
    if (handle == 0)
      fail("resource handle 0 is invalid", 0);
    return handle;
  }
};

// Reads the magic and returns the schema version. Older blobs keep loading so
// existing dumps and the hand-built test fixtures stay valid: before v3 every
// real recipe cost exactly one execution and every tag edge was free.
uint8_t checkMagic(ByteReader& in) noexcept {
  for (std::uint8_t expected : kMagic) {
    if (in.readByte() != expected)
      fail("invalid magic bits", 0);
  }
  const uint8_t version = in.readByte();
  if (version != kSchemaV1 && version != kSchemaV2 && version != kSchemaV3)
    fail("unsupported recipe schema version", 0);
  return version;
}

// Everything the first pass learns, used to size the arrays the second pass
// fills. Recipe ids are handed out in file order.
struct Layout {
  uint nReal = 0;
  uint nOutput = 0;
  uint maxHandle = 0;
  // Maps `handle-1` to its number of recipes (recipes per item).
  aw::vector<uint> rpi;
  // Maps `recipe` to its number of items (items per recipe).
  aw::vector<uint> ipr;
  // Maps `recipe` to its number of outputs (anchor plus byproducts).
  aw::vector<uint> opr;
  // Maps `recipe` to its number of workstations.
  aw::vector<uint> wpr;

  void noteHandle(uint handle) noexcept {
    if (handle <= maxHandle)
      return;

    maxHandle = handle;
    if (rpi.size() < handle)
      rpi.resize(handle, 0);
  }
};

uint readWorkstations(ByteReader &in, Layout &layout) noexcept {
  uint count = in.readVarInt();
  if (count == 0)
    return 0;

  uint id = in.readHandle();
  if (id > layout.nReal)
    fail("workstation handle out of range", 0);
  layout.noteHandle(id);

  for (uint i = 1; i < count; ++i) {
    uint delta = in.readVarInt();
    if (id > UINT_MAX - delta)
      fail("workstation handle overflow", 0);
    id += delta;
    if (id > layout.nReal)
      fail("workstation handle out of range", 0);
    layout.noteHandle(id);
  }
  return count;
}

// We do a 2-pass parsing.
// Here we need to confirm some parameters that shapes the graph.
Layout scan(std::span<const std::byte> bytes) noexcept {
  ByteReader in(bytes);
  Layout layout;

  // Check magic bits. The version decides whether a cost precedes the anchor
  // amount and whether a byproduct list follows the workstations.
  const uint8_t version = checkMagic(in);

  // Check header.
  layout.nReal = in.readVarInt();
  layout.nOutput = in.readVarInt();

  uint output = 0;
  for (uint entry = 0; entry < layout.nOutput; entry++) {
    uint delta = in.readVarInt();
    if (delta == 0)
      fail("output handles must be strictly ascending", layout);
    if (output > UINT_MAX - delta)
      fail("output handle overflow", layout);
    
    output += delta;
    layout.noteHandle(output);

    uint nRecipe = in.readVarInt();
    layout.rpi[output - 1] += nRecipe;

    for (uint i = 0; i < nRecipe; i++) {
      if (version >= kSchemaV3) {
        // Only a real recipe is costed; the synthetic member edge of a
        // pseudo-resource is free, and the parser zeroes it below whatever
        // this holds. A pseudo anchor's value is therefore ignored, but it
        // still has to be readable.
        const int64_t cost = in.readVarLong();
        if (cost < 0 || cost > kMaxRecipeCost)
          fail("recipe cost out of range", layout);
      }

      in.readVarLong();  // anchor amount
      layout.wpr.push_back(readWorkstations(in, layout));

      // Byproducts are extra outputs of the same recipe. The first handle is
      // absolute and the rest are ascending deltas, exactly like the input
      // list below.
      uint nOutputs = 1;
      if (version >= kSchemaV2) {
        const uint nByproduct = in.readVarInt();
        nOutputs += nByproduct;
        uint byproduct = 0;
        for (uint j = 0; j < nByproduct; j++) {
          in.readVarLong();  // byproduct amount
          const uint delta = in.readVarInt();
          if (byproduct > UINT_MAX - delta)
            fail("byproduct handle overflow", layout);
          byproduct += delta;
          if (byproduct == 0)
            fail("resource handle 0 is not valid", layout);
          layout.noteHandle(byproduct);
          layout.rpi[byproduct - 1]++;
        }
      }
      layout.opr.push_back(nOutputs);

      uint nInput = in.readVarInt();
      layout.ipr.push_back(nInput);

      uint input = 0;
      for (uint j = 0; j < nInput; j++) {
        in.readVarLong();  // input amount
        uint delta = in.readVarInt();
        if (input > UINT_MAX - delta)
          fail("input handle overflow", layout);
        input += delta;
        if (input == 0)
          fail("resource handle 0 is not valid", layout);
        layout.noteHandle(input);
      }
    }
  }

  if (!in.atEnd())
    fail("trailing bytes after the last entry", layout);
  return layout;
}

void prefixSum(aw::vector<uint> &v) noexcept {
  v.insert(v.begin(), 0);
  for (size_t i = 0; i + 1 < v.size(); i++)
    v[i + 1] += v[i];
}

// Total order on recipes by the only thing the planner can see: the output
// item, the output amount, the amounts of every input and the cost. The cost is
// the last key so the order stays dominated by the column, as the prefix
// invariant on `output` expects.
struct RecipeKeyLess {
  const BaseCraftingGraph &g;

  bool operator()(uint a, uint b) const noexcept {
    if (g.output[a] != g.output[b])
      return g.output[a] < g.output[b];
    if (g.outputAmt[a] != g.outputAmt[b])
      return g.outputAmt[a] < g.outputAmt[b];

    const auto oa = g.outputsOf(a);
    const auto ob = g.outputsOf(b);
    const auto owa = g.outputAmountsOf(a);
    const auto owb = g.outputAmountsOf(b);
    const size_t sharedOut = oa.size() < ob.size() ? oa.size() : ob.size();
    for (size_t i = 0; i < sharedOut; i++) {
      if (oa[i] != ob[i])
        return oa[i] < ob[i];
      if (owa[i] != owb[i])
        return owa[i] < owb[i];
    }
    if (oa.size() != ob.size())
      return oa.size() < ob.size();

    const auto ta = g.inputsOf(a);
    const auto tb = g.inputsOf(b);
    const auto wa = g.inputAmountsOf(a);
    const auto wb = g.inputAmountsOf(b);
    const size_t shared = ta.size() < tb.size() ? ta.size() : tb.size();
    for (size_t i = 0; i < shared; i++) {
      if (ta[i] != tb[i])
        return ta[i] < tb[i];
      if (wa[i] != wb[i])
        return wa[i] < wb[i];
    }
    if (ta.size() != tb.size())
      return ta.size() < tb.size();
    // The cost is part of the recipe's identity: two recipes with the same
    // column but different costs are not interchangeable, and merging them
    // would keep an arbitrary one of the two -- silently charging the folded
    // batch level as a plain recipe, or the other way round. Keeping both is
    // always sound; the direct-dominance pass drops the expensive twin for
    // free, because their columns are equal.
    return g.cost[a] < g.cost[b];
  }
};

// Removes duplicate entries, and unites entries that differ only on workstations.
void canonicalizeRecipes() noexcept {
  const uint nRecipe = graph.nRecipe;
  [[unlikely]]
  if (nRecipe < 2)
    return;

  // Sort recipes.
  aw::vector<uint> order(nRecipe);
  for (uint r = 0; r < nRecipe; r++)
    order[r] = r;
  std::sort(order.begin(), order.end(), RecipeKeyLess{graph});

  const RecipeKeyLess less { graph };
  // Deduplication and grouping.
  aw::vector<uint> rep(nRecipe, UINT32_MAX);
  for (uint i = 0; i < nRecipe;) {
    uint j = i + 1;
    while (j < nRecipe && !less(order[i], order[j]))
      j++;

    uint survivor = order[i];
    for (uint k = i; k < j; k++)
      if (order[k] < survivor)
        survivor = order[k];
    for (uint k = i; k < j; k++)
      rep[order[k]] = survivor;
    i = j;
  }

  uint newNRecipe = 0;
  aw::vector<uint> newId(nRecipe, UINT32_MAX);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      newId[r] = newNRecipe++;
  }
  if (newNRecipe == nRecipe)
    return;

  // Rebuild workstations.
  aw::vector<uint> wsOffsets(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++)
    wsOffsets[newId[rep[r]]] += graph.workstations.targetsOf(r).size();
  prefixSum(wsOffsets);

  aw::vector<ItemId> wsTargets(wsOffsets.back());
  aw::vector<uint> wsCursor(wsOffsets.begin(), wsOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    const uint dst = newId[rep[r]];
    for (ItemId station : graph.workstations.targetsOf(r))
      wsTargets[wsCursor[dst]++] = station;
  }

  aw::vector<uint> newWsOffsets(newNRecipe + 1, 0);
  aw::vector<ItemId> newWsTargets;
  newWsTargets.reserve(wsTargets.size());
  for (uint j = 0; j < newNRecipe; j++) {
    std::sort(wsTargets.begin() + wsOffsets[j], wsTargets.begin() + wsOffsets[j + 1]);
    for (uint e = wsOffsets[j]; e < wsOffsets[j + 1]; e++) {
      if (e > wsOffsets[j] && wsTargets[e] == wsTargets[e - 1])
        continue;
      newWsTargets.push_back_unchecked(wsTargets[e]);
    }
    newWsOffsets[j + 1] = newWsTargets.size();
  }
  graph.workstations.offsets = std::move(newWsOffsets);
  graph.workstations.targets = std::move(newWsTargets);

  // Rebuild item -> recipe edges. One edge per output, so a recipe appears in
  // the producer list of every item it outputs.
  aw::vector<uint> itemOffsets(graph.nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    for (ItemId item : graph.outputsOf(r))
      itemOffsets[item]++;
  }
  prefixSum(itemOffsets);

  aw::vector<ItemId> itemTargets(itemOffsets.back(), 0);
  aw::vector<Amount> itemWeights(itemOffsets.back(), 0);
  aw::vector<uint> itemCursor(itemOffsets.begin(), itemOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    const auto outs = graph.outputsOf(r);
    const auto amounts = graph.outputAmountsOf(r);
    for (size_t k = 0; k < outs.size(); k++) {
      const uint slot = itemCursor[outs[k]]++;
      itemTargets[slot] = newId[r];
      itemWeights[slot] = amounts[k];
    }
  }
  graph.i2r.offsets = std::move(itemOffsets);
  graph.i2r.targets = std::move(itemTargets);
  graph.i2r.weights = std::move(itemWeights);

  // Rebuild recipe -> output edges.
  aw::vector<uint> outOffsets(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      outOffsets[newId[r]] = (uint) graph.outputsOf(r).size();
  }
  prefixSum(outOffsets);

  aw::vector<ItemId> outTargets(outOffsets.back(), 0);
  aw::vector<Amount> outWeights(outOffsets.back(), 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    uint slot = outOffsets[newId[r]];
    const auto targets = graph.outputsOf(r);
    const auto weights = graph.outputAmountsOf(r);
    for (size_t k = 0; k < targets.size(); k++) {
      outTargets[slot] = targets[k];
      outWeights[slot] = weights[k];
      slot++;
    }
  }
  graph.r2o.offsets = std::move(outOffsets);
  graph.r2o.targets = std::move(outTargets);
  graph.r2o.weights = std::move(outWeights);

  // Rebuild recipe -> item edges.
  aw::vector<uint> inputOffsets(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      inputOffsets[newId[r]] = (uint) graph.inputsOf(r).size();
  }
  prefixSum(inputOffsets);

  aw::vector<ItemId> inputTargets(inputOffsets.back(), 0);
  aw::vector<Amount> inputWeights(inputOffsets.back(), 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    uint slot = inputOffsets[newId[r]];
    const auto targets = graph.inputsOf(r);
    const auto weights = graph.inputAmountsOf(r);
    for (size_t k = 0; k < targets.size(); k++) {
      inputTargets[slot] = targets[k];
      inputWeights[slot] = weights[k];
      slot++;
    }
  }
  graph.r2i.offsets = std::move(inputOffsets);
  graph.r2i.targets = std::move(inputTargets);
  graph.r2i.weights = std::move(inputWeights);

  aw::vector<ItemId> output(newNRecipe, 0);
  aw::vector<Amount> outputAmt(newNRecipe, 0);
  // The cost is part of the survival key, so every recipe merged into one
  // survivor agrees on it; copying the survivor's is exact, not a choice.
  aw::vector<Amount> cost(newNRecipe, 1);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    output[newId[r]] = graph.output[r];
    outputAmt[newId[r]] = graph.outputAmt[r];
    cost[newId[r]] = graph.cost[r];
  }
  graph.output = std::move(output);
  graph.outputAmt = std::move(outputAmt);
  graph.cost = std::move(cost);
  graph.nRecipe = newNRecipe;
}

// ---------------------------------------------------------------------------
// Single-use tag inlining
// ---------------------------------------------------------------------------
// See TagInlineMode in the header. Two shapes are unfolded:
//
//   * Many members, one consumer. A tag T with member edges `T <- m1 .. T <-
//     mn` and exactly one real consumer `R: a <- T, ...` that consumes T once
//     at amount 1 is replaced by the n real recipes `a <- m1, ...`: the tag
//     node, its n member edges and R disappear, and n recipes take their
//     place. The recipe count therefore drops by exactly one per inlined tag.
//
//   * One member, any consumers. A tag T with a single surviving member m is
//     exactly m, so every consumer can spend m directly: replacing each `T`
//     input by `m` preserves the amount, and the tag node and its one member
//     edge disappear. This is the shape pruning leaves behind when every other
//     member edge was marked dominated (or was dropped by reachability), and
//     it is the one that lets the consumer count stay arbitrary.
//
// The transform is a fixpoint, but the many-member branch cannot cascade into a
// product of member sets: the n copies of R are consumers of every *other* tag
// R used, so those tags stop being single-use and are never expanded. Each
// inlining strictly decreases the recipe count, so the loop terminates.

struct MutableRecipe {
  ItemId out = 0;
  Amount outAmt = 0;
  // Underlying Minecraft executions per execution, exactly as
  // `BaseCraftingGraph::cost` describes. Every rewrite below copies it, so a
  // batch level keeps being charged its batch size after inlining.
  Amount cost = 1;
  // Extra outputs beyond the anchor, ascending by item and never containing
  // the anchor. The anchor stays in `out`/`outAmt` so the prefix invariant and
  // the recipe identity survive every rewrite.
  aw::vector<ItemId> outs;
  aw::vector<Amount> outAmts;
  aw::vector<ItemId> ws;
  aw::vector<ItemId> inputs;
  aw::vector<Amount> amounts;
  // Source recipe id, or UINT32_MAX for a recipe the query-time inliner made.
  uint32_t origin = UINT32_MAX;
};

// Writes one recipe's outputs -- anchor merged with the sorted byproduct list
// -- into a CSR slot, keeping the list ascending by item.
void writeRecipeOutputs(const MutableRecipe &rec, ItemId *targets, Amount *weights,
                        uint slot) noexcept {
  size_t k = 0;
  while (k < rec.outs.size() && rec.outs[k] < rec.out) {
    targets[slot] = rec.outs[k];
    weights[slot] = rec.outAmts[k];
    slot++;
    k++;
  }
  targets[slot] = rec.out;
  weights[slot] = rec.outAmt;
  slot++;
  while (k < rec.outs.size()) {
    targets[slot] = rec.outs[k];
    weights[slot] = rec.outAmts[k];
    slot++;
    k++;
  }
}

// The core fixpoint. `allowed` is consulted once, while the member sets are
// collected, and receives the index of a recipe in `recipes`. The query-time
// path accepts everything: it only ever hands in the recipes that survived
// reachability and pruning, so a tag's surviving member edges are exactly the
// members whose stock the player can still spend.
template <typename Allowed>
bool inlineSingleUseTagsCore(aw::vector<MutableRecipe> &recipes, uint nReal,
                             uint nItem, bool singleMemberRule, Allowed allowed) {
  if (nItem <= nReal || recipes.empty())
    return false;

  // A tag is "simple" when every one of its member edges is a single real input
  // of amount 1 with no workstation. A tag with any other kind of recipe is
  // left alone, so no nested or weighted tag ever has to be unfolded.
  aw::vector<uint8_t> simpleTag(nItem, 1);
  aw::vector<uint8_t> hasRecipe(nItem, 0);
  for (const MutableRecipe& rec : recipes) {
    if (rec.out < nReal)
      continue;
    hasRecipe[rec.out] = 1;
    if (!(rec.ws.empty() && rec.inputs.size() == 1 && rec.amounts[0] == 1 &&
          rec.inputs[0] < nReal))
      simpleTag[rec.out] = 0;
  }
  for (uint t = nReal; t < nItem; t++)
    if (!hasRecipe[t])
      simpleTag[t] = 0;

  aw::vector<aw::vector<ItemId>> members(nItem);
  for (uint r = 0; r < recipes.size(); r++) {
    const MutableRecipe& rec = recipes[r];
    if (rec.out < nReal || !simpleTag[rec.out] || !allowed(r))
      continue;
    members[rec.out].push_back(rec.inputs[0]);
  }

  // The consumer side of a tag: the real recipes that name it, grouped by tag.
  // A recipe lists each tag at most once, so a row's length is the tag's
  // consumer count. The relation is built once: neither rule can make a tag
  // newly eligible. The many-member rule only raises other tags' counts (its
  // copies keep the same inputs), and the single-member rule only replaces a
  // tag input by a real one, so the eligible set only shrinks and one sweep is
  // the whole fixpoint.
  aw::vector<uint> consOffsets(nItem + 1, 0);
  for (const MutableRecipe &rec : recipes) {
    if (rec.out >= nReal)
      continue;
    for (ItemId j : rec.inputs)
      if (j >= nReal)
        consOffsets[j + 1]++;
  }
  for (ItemId t = 0; t < nItem; t++)
    consOffsets[t + 1] += consOffsets[t];
  aw::vector<uint32_t> consRecipes(consOffsets.back());
  {
    aw::vector<uint> cursor(consOffsets.begin(), consOffsets.end() - 1);
    for (uint i = 0; i < recipes.size(); i++) {
      const MutableRecipe &rec = recipes[i];
      if (rec.out >= nReal)
        continue;
      for (ItemId j : rec.inputs)
        if (j >= nReal)
          consRecipes[cursor[j]++] = i;
    }
  }
  const auto consumersOf = [&](ItemId t) noexcept {
    return std::span<const uint32_t>(consRecipes.data() + consOffsets[t],
                                     consOffsets[t + 1] - consOffsets[t]);
  };

  // Recipe ids stay stable: both rules only mark, and the rewritten list is
  // built in one pass at the end.
  aw::vector<uint8_t> deleted(recipes.size(), 0);
  aw::vector<uint8_t> claimedConsumer(recipes.size(), 0);
  aw::vector<uint32_t> expansionTag(recipes.size(), UINT32_MAX);
  aw::vector<uint8_t> tagDeleted(nItem, 0);
  bool changed = false;

  // Rule 1: a tag with a single surviving member is that member, so every
  // consumer can spend it directly, keeping the amount. Substituting in place
  // cannot invalidate any other tag, so all of them are applied in one sweep.
  if (singleMemberRule) {
    for (ItemId t = nReal; t < nItem; t++) {
      if (!simpleTag[t] || members[t].size() != 1)
        continue;
      const auto consumers = consumersOf(t);
      if (consumers.empty())
        continue;
      const ItemId m = members[t][0];
      for (uint32_t consumer : consumers) {
        MutableRecipe &rec = recipes[consumer];
        const auto it = std::lower_bound(rec.inputs.begin(), rec.inputs.end(), t);
        if (it == rec.inputs.end() || *it != t)
          continue;
        const size_t slot = (size_t) (it - rec.inputs.begin());
        const Amount amount = rec.amounts[slot];
        rec.inputs.erase(rec.inputs.begin() + slot);
        rec.amounts.erase(rec.amounts.begin() + slot);
        const auto at = std::lower_bound(rec.inputs.begin(), rec.inputs.end(), m);
        const size_t pos = (size_t) (at - rec.inputs.begin());
        if (pos < rec.inputs.size() && rec.inputs[pos] == m) {
          rec.amounts[pos] += amount;
        } else {
          rec.inputs.insert(rec.inputs.begin() + pos, m);
          rec.amounts.insert(rec.amounts.begin() + pos, amount);
        }
      }
      tagDeleted[t] = 1;
      changed = true;
    }
  }

  // Rule 2: a many-member tag with one consumer, consumed once at amount 1, is
  // expanded into one copy of that consumer per member. Two eligible tags that
  // share the consumer cannot both be expanded -- the first copy set turns the
  // other into a multi-consumer tag -- so at most one tag is claimed per
  // consumer recipe.
  for (ItemId t = nReal; t < nItem; t++) {
    if (!simpleTag[t] || members[t].size() < 2)
      continue;
    const auto consumers = consumersOf(t);
    if (consumers.size() != 1)
      continue;
    const uint32_t consumer = consumers[0];
    if (claimedConsumer[consumer])
      continue;
    const MutableRecipe &rec = recipes[consumer];
    const auto it = std::lower_bound(rec.inputs.begin(), rec.inputs.end(), t);
    if (it == rec.inputs.end() || *it != t)
      continue;
    if (rec.amounts[(size_t) (it - rec.inputs.begin())] != 1)
      continue;
    claimedConsumer[consumer] = 1;
    expansionTag[consumer] = t;
    tagDeleted[t] = 1;
  }

  bool anyExpansion = false;
  for (uint i = 0; i < recipes.size() && !anyExpansion; i++)
    anyExpansion = claimedConsumer[i] != 0;
  if (!changed && !anyExpansion)
    return false;

  // Drop every member edge of an inlined tag.
  for (uint i = 0; i < recipes.size(); i++) {
    const ItemId out = recipes[i].out;
    if (out >= nReal && tagDeleted[out])
      deleted[i] = 1;
  }

  // The survivors, each claimed consumer replaced in place by one copy per
  // member. Expanding where the consumer sat -- rather than appending every
  // copy at the end -- keeps the list ordered by output: the input list is
  // ascending, the survivors keep their order, and a copy inherits its
  // consumer's output. Registration and the ordering note on
  // BaseCraftingGraph::output depend on that regularity.
  aw::vector<MutableRecipe> next;
  next.reserve(recipes.size());
  for (uint i = 0; i < recipes.size(); i++) {
    if (deleted[i])
      continue;
    if (!claimedConsumer[i]) {
      next.push_back(std::move(recipes[i]));
      continue;
    }
    const ItemId t = expansionTag[i];
    // The consumer without its tag input. Inputs are strictly ascending, so
    // dropping the one slot keeps them ordered.
    aw::vector<ItemId> newInputs;
    aw::vector<Amount> newAmounts;
    newInputs.reserve(recipes[i].inputs.size());
    newAmounts.reserve(recipes[i].inputs.size());
    for (size_t k = 0; k < recipes[i].inputs.size(); k++) {
      if (recipes[i].inputs[k] == t)
        continue;
      newInputs.push_back_unchecked(recipes[i].inputs[k]);
      newAmounts.push_back_unchecked(recipes[i].amounts[k]);
    }
    for (ItemId m : members[t]) {
      MutableRecipe copy;
      copy.out = recipes[i].out;
      copy.outAmt = recipes[i].outAmt;
      copy.cost = recipes[i].cost;
      copy.outs = recipes[i].outs;
      copy.outAmts = recipes[i].outAmts;
      copy.ws = recipes[i].ws;
      copy.inputs = newInputs;
      copy.amounts = newAmounts;
      const auto it = std::lower_bound(copy.inputs.begin(), copy.inputs.end(), m);
      const size_t pos = (size_t) (it - copy.inputs.begin());
      if (pos < copy.inputs.size() && copy.inputs[pos] == m) {
        copy.amounts[pos] += 1;
      } else {
        copy.inputs.insert(copy.inputs.begin() + pos, m);
        copy.amounts.insert(copy.amounts.begin() + pos, 1);
      }
      next.push_back(std::move(copy));
    }
  }
  recipes.swap(next);
  changed = true;
  return changed;
}

// Flattens `graph`'s recipes into the mutable form.
void extractRecipes(const CraftingGraph& graph, aw::vector<MutableRecipe> &out) {
  out.clear();
  out.reserve(graph.nRecipe);
  for (uint r = 0; r < graph.nRecipe; r++) {
    MutableRecipe rec;
    rec.out = graph.output[r];
    rec.outAmt = graph.outputAmt[r];
    rec.cost = graph.cost[r];
    const auto outs = graph.outputsOf(r);
    const auto outAmts = graph.outputAmountsOf(r);
    rec.outs.reserve(outs.size());
    rec.outAmts.reserve(outAmts.size());
    for (size_t k = 0; k < outs.size(); k++) {
      if (outs[k] == rec.out)
        continue;
      rec.outs.push_back_unchecked(outs[k]);
      rec.outAmts.push_back_unchecked(outAmts[k]);
    }
    const auto ws = graph.workstations.targetsOf(r);
    rec.ws.assign(ws.begin(), ws.end());
    const auto inputs = graph.inputsOf(r);
    const auto amounts = graph.inputAmountsOf(r);
    rec.inputs.assign(inputs.begin(), inputs.end());
    rec.amounts.assign(amounts.begin(), amounts.end());
    out.push_back(std::move(rec));
  }
}

// Rebuilds every CSR field from a rewritten recipe list, then merges the
// duplicates the rewrite may have produced.
void rebuildFromRecipes(CraftingGraph& graph, aw::vector<MutableRecipe> &recipes) {
  const uint nItem = graph.nItem;
  const uint newNRecipe = (uint) recipes.size();

  // Rebuild item -> recipe. One edge per output.
  aw::vector<uint> itemOffsets(nItem, 0);
  for (const MutableRecipe& rec : recipes) {
    itemOffsets[rec.out]++;
    for (ItemId o : rec.outs)
      itemOffsets[o]++;
  }
  prefixSum(itemOffsets);
  graph.i2r.offsets = std::move(itemOffsets);
  graph.i2r.targets.resize(graph.i2r.offsets.back());
  graph.i2r.weights.resize(graph.i2r.offsets.back());
  {
    aw::vector<uint> cursor(graph.i2r.offsets.begin(), graph.i2r.offsets.end() - 1);
    for (uint r = 0; r < newNRecipe; r++) {
      const uint anchorSlot = cursor[recipes[r].out]++;
      graph.i2r.targets[anchorSlot] = r;
      graph.i2r.weights[anchorSlot] = recipes[r].outAmt;
      for (size_t k = 0; k < recipes[r].outs.size(); k++) {
        const uint slot = cursor[recipes[r].outs[k]]++;
        graph.i2r.targets[slot] = r;
        graph.i2r.weights[slot] = recipes[r].outAmts[k];
      }
    }
  }

  // Rebuild recipe -> output.
  aw::vector<uint> outOffsets(newNRecipe + 1, 0);
  for (uint r = 0; r < newNRecipe; r++)
    outOffsets[r + 1] = outOffsets[r] + 1 + (uint) recipes[r].outs.size();
  graph.r2o.offsets = std::move(outOffsets);
  graph.r2o.targets.resize(graph.r2o.offsets.back());
  graph.r2o.weights.resize(graph.r2o.offsets.back());
  for (uint r = 0; r < newNRecipe; r++)
    writeRecipeOutputs(recipes[r], graph.r2o.targets.data(), graph.r2o.weights.data(),
                       graph.r2o.offsets[r]);

  // Rebuild recipe -> item.
  graph.r2i.offsets.assign(newNRecipe + 1, 0);
  for (uint r = 0; r < newNRecipe; r++)
    graph.r2i.offsets[r + 1] = graph.r2i.offsets[r] + (uint) recipes[r].inputs.size();
  graph.r2i.targets.resize(graph.r2i.offsets.back());
  graph.r2i.weights.resize(graph.r2i.offsets.back());
  for (uint r = 0; r < newNRecipe; r++) {
    uint slot = graph.r2i.offsets[r];
    for (size_t k = 0; k < recipes[r].inputs.size(); k++) {
      graph.r2i.targets[slot] = recipes[r].inputs[k];
      graph.r2i.weights[slot] = recipes[r].amounts[k];
      slot++;
    }
  }

  // Rebuild workstations.
  graph.workstations.offsets.assign(newNRecipe + 1, 0);
  for (uint r = 0; r < newNRecipe; r++)
    graph.workstations.offsets[r + 1] =
        graph.workstations.offsets[r] + (uint) recipes[r].ws.size();
  graph.workstations.targets.resize(graph.workstations.offsets.back());
  for (uint r = 0; r < newNRecipe; r++) {
    uint slot = graph.workstations.offsets[r];
    for (ItemId w : recipes[r].ws)
      graph.workstations.targets[slot++] = w;
  }

  graph.output.resize(newNRecipe);
  graph.outputAmt.resize(newNRecipe);
  graph.cost.resize(newNRecipe);
  for (uint r = 0; r < newNRecipe; r++) {
    graph.output[r] = recipes[r].out;
    graph.outputAmt[r] = recipes[r].outAmt;
    graph.cost[r] = recipes[r].cost;
  }
  graph.nRecipe = newNRecipe;

  canonicalizeRecipes();
}

// True when every balance row of the recipe's column is <= 0: for each output
// `o`, the recipe consumes at least as much of `o` as it produces. A row that
// is not an output is a pure input, hence <= 0 already. Such a column is
// dominated by doing nothing -- deleting one execution keeps every balance at
// least as high and strictly lowers the step count -- so no optimal plan uses
// it. With byproducts the test has to cover every output row, not just the
// anchor: a recipe can be a net loss on its anchor and a net gain on a
// byproduct.
bool columnIsNonPositive(const MutableRecipe& rec) noexcept {
  const auto consumed = [&](ItemId item) noexcept {
    Amount sum = 0;
    for (size_t k = 0; k < rec.inputs.size(); k++)
      if (rec.inputs[k] == item)
        sum += rec.amounts[k];
    return sum;
  };
  if (rec.outAmt > consumed(rec.out))
    return false;
  for (size_t k = 0; k < rec.outs.size(); k++)
    if (rec.outAmts[k] > consumed(rec.outs[k]))
      return false;
  return true;
}

// The same test read off the CSR form, without materializing the recipes.
bool columnIsNonPositiveAt(const CraftingGraph& graph, uint r) noexcept {
  const auto outs = graph.outputsOf(r);
  const auto outAmts = graph.outputAmountsOf(r);
  const auto inputs = graph.inputsOf(r);
  const auto amounts = graph.inputAmountsOf(r);
  for (size_t o = 0; o < outs.size(); o++) {
    Amount consumed = 0;
    for (size_t k = 0; k < inputs.size(); k++)
      if (inputs[k] == outs[o])
        consumed += amounts[k];
    if (outAmts[o] > consumed)
      return false;
  }
  return true;
}

// Flow normalization, before any pass reads an amount: an input that consumes
// nothing per firing (`amount <= 0`) contributes 0 to every balance row, so
// removing it changes no plan -- `produced(i) - consumed(i) >= rhs(i)` is the
// same either way. It cannot be kept, though: a non-positive amount is not a
// flow, and the comparison harnesses' planners reject it outright, so the two
// spellings of "free input" would hand three engines two different graphs. The
// corpora reach this through AE2's entropy recipes, whose fluid ingredient
// carries no amount at all (`data/ae2/recipe/entropy/cool/water_ice.json`).
//
// Runs before dropUselessRecipes so that self-consumption never sums a
// degenerate amount, and ends in canonicalizeRecipes(): dropping an input can
// make recipes identical, and uniting their workstations is part of the
// normalization.
//
// Returns true when anything was removed; the rebuild canonicalizes for us, so
// the caller can skip its own call in that case.
bool dropNonPositiveInputs(CraftingGraph& graph) noexcept {
  const auto degenerate = [&](uint r) noexcept {
    for (Amount amount : graph.inputAmountsOf(r))
      if (amount <= 0)
        return true;
    return false;
  };

  bool any = false;
  for (uint r = 0; r < graph.nRecipe && !any; r++)
    any = degenerate(r);
  if (!any)
    return false;

  aw::vector<MutableRecipe> recipes;
  extractRecipes(graph, recipes);
  for (MutableRecipe& rec : recipes) {
    size_t kept = 0;
    for (size_t k = 0; k < rec.inputs.size(); k++) {
      if (rec.amounts[k] <= 0)
        continue;
      if (kept != k) {
        rec.inputs[kept] = rec.inputs[k];
        rec.amounts[kept] = rec.amounts[k];
      }
      kept++;
    }
    rec.inputs.resize(kept);
    rec.amounts.resize(kept);
  }
  rebuildFromRecipes(graph, recipes);
  return true;
}

// Drops the recipes that can never appear in an optimal plan, before any pass
// sees the graph. Two classes of real recipe qualify:
//
//   * A recipe whose whole column is <= 0 -- on every output it produces no
//     more than it consumes -- is dominated by doing nothing, so no optimal
//     plan uses it, for any target and any inventory. This needs no stock or
//     workstation guard. It covers the synthetic self-loops and any real
//     recipe that eats its own output at least as fast as it makes it.
//   * A recipe with no workstation can never be entered by the reachability
//     walk, so it is unreachable.
//
// A synthetic tag edge outputs a pseudo-resource, so it is exempt from both:
// its empty workstation set is deliberate, and the tag pass owns its structure.
//
// Returns true when anything was dropped; the rebuild ends with
// canonicalizeRecipes(), so the caller can skip its own call in that case.
bool dropUselessRecipes(CraftingGraph& graph) noexcept {
  const auto netLoss = [&](const MutableRecipe& rec) noexcept {
    return rec.out < graph.nReal && columnIsNonPositive(rec);
  };
  const auto netLossAt = [&](uint r) noexcept {
    return graph.output[r] < graph.nReal && columnIsNonPositiveAt(graph, r);
  };

  bool any = false;
  for (uint r = 0; r < graph.nRecipe && !any; r++) {
    any = netLossAt(r) ||
          (graph.output[r] < graph.nReal && graph.workstations.targetsOf(r).empty());
  }
  if (!any)
    return false;

  aw::vector<MutableRecipe> recipes;
  extractRecipes(graph, recipes);
  size_t kept = 0;
  for (size_t i = 0; i < recipes.size(); i++) {
    const MutableRecipe& rec = recipes[i];
    if (netLoss(rec) || (rec.out < graph.nReal && rec.ws.empty()))
      continue;
    // Guard the self-move: `kept == i` for every recipe that survives, and a
    // self-move-assignment may legally empty the source vector.
    if (kept != i)
      recipes[kept] = std::move(recipes[i]);
    kept++;
  }
  recipes.resize(kept);
  rebuildFromRecipes(graph, recipes);
  return true;
}

// Registration-time inliner: flatten every single-use tag, using every member
// edge, before the dominance passes run.
void inlineSingleUseTags(CraftingGraph& graph) noexcept {
  aw::vector<MutableRecipe> recipes;
  extractRecipes(graph, recipes);
  if (!inlineSingleUseTagsCore(recipes, graph.nReal, graph.nItem,
                               options.inlineSingleMemberTags,
                               [](uint) { return true; }))
    return;
  rebuildFromRecipes(graph, recipes);
}

// ---------------------------------------------------------------------------
// Query-time reachability
// ---------------------------------------------------------------------------

// State and pruning logic for one reachableSubgraph query. `itemSeen` /
// `recipeSeen` are what the current walk keeps; `disabled` forces a recipe
// out of the walk without touching the flags, so a post-pass can rebuild the
// subgraph.
struct ReachQuery {
  const Handle output;
  const std::span<const Amount> inventory;

  // Option switches read once, because prunedOut runs once per walked edge.
  const bool pruneTag = options.tagPruning;
  const bool pruneRecipe = options.recipePruning;
  const bool pruneDirect = options.directPruning;
  const bool pruneSubstitution = options.substitutionPruning;

  // Workstation availability as a bitset (in fact a byteset).
  aw::vector<uint8_t> allowed;
  aw::vector<uint8_t> itemSeen;
  // `expanded` is disjoint from `itemSeen` on purpose: an item is put in the
  // subgraph as soon as it is any walked recipe's output, but its producers
  // are only visited once some walked recipe *consumes* it. Keeping the two
  // facts apart is what makes a byproduct that a later recipe needs pull in
  // its own producers. See walk().
  aw::vector<uint8_t> expanded;
  aw::vector<uint8_t> recipeSeen;
  aw::vector<uint8_t> disabled;
  aw::vector<ItemId> queue;

  ReachQuery(Handle output, std::span<const Handle> workstations,
             std::span<const Amount> inventory) noexcept
      : output(output), inventory(inventory), allowed(graph.nItem, 0),
        itemSeen(graph.nItem, 0), expanded(graph.nItem, 0),
        recipeSeen(graph.nRecipe, 0), disabled(graph.nRecipe, 0) {
    for (Handle handle : workstations) {
      if (handle >= 1 && handle <= graph.nItem)
        allowed[handle - 1] = 1;
    }
    // Enqueued once per item at most, so nItem is a hard bound; the capacity
    // survives the clear() at the top of every walk.
    queue.reserve(graph.nItem);
  }

  // Stock of a source item, 0 when the inventory does not cover it.
  Amount held(ItemId item) const noexcept {
    return item < inventory.size() ? inventory[item] : 0;
  }

  // True when one of `stations` is available in this query.
  bool anyStationAvailable(std::span<const ItemId> stations) const noexcept {
    for (ItemId station : stations)
      if (station < allowed.size() && allowed[station])
        return true;
    return false;
  }

  // True when a query-time pruning rule drops `recipe` from the walk.
  bool prunedOut(uint recipe) const noexcept {
    // A dominated tag edge is dropped unless the player actually holds the
    // member, in which case the free stock can still be spent on the tag.
    if (pruneTag && recipe < graph.tagEdgeDominated.size() &&
        graph.tagEdgeDominated[recipe]) {
      const auto inputs = graph.inputsOf(recipe);
      if (inputs.empty() || held(inputs[0]) == 0)
        return true;
    }

    // A composite-dominated real recipe is likewise only dropped when the
    // witness input has no stock, so held stock can still be spent through
    // it. The replacement may need a workstation this query does not have,
    // so it is only dropped when one of the dominator's workstations is
    // available. Otherwise the player would lose the only route.
    if (pruneRecipe && recipe < graph.recipeDominated.size() &&
        graph.recipeDominated[recipe] && held(graph.recipeGuardInput[recipe]) == 0 &&
        recipe < graph.recipeDominatorWorkstations.size() &&
        anyStationAvailable(graph.recipeDominatorWorkstations[recipe]))
      return true;

    // A direct-dominated real recipe has a sibling with a componentwise
    // larger column, so one execution of the sibling replaces it with no
    // worse balance. Nothing is inlined, so stock never makes it
    // preferable, but the replacement may need a workstation this query
    // does not have.
    if (pruneDirect && recipe < graph.recipeDirectDominated.size() &&
        graph.recipeDirectDominated[recipe] &&
        anyStationAvailable(graph.recipeDirectDominatorWorkstations[recipe]))
      return true;

    // A substituted real recipe pays for one of its inputs with a
    // cost-dominating resource, so it is only dropped when none of the
    // collapsed inputs can be spent from stock and one of the replacements
    // can run. The collapsed tag members make the guard a set.
    if (pruneSubstitution && recipe < graph.recipeSubstituted.size() &&
        graph.recipeSubstituted[recipe] &&
        recipe < graph.recipeSubstitutedGuards.size()) {
      bool stocked = false;
      for (ItemId guard : graph.recipeSubstitutedGuards[recipe]) {
        if (held(guard) != 0) {
          stocked = true;
          break;
        }
      }
      if (!stocked && recipe < graph.recipeSubstitutedDominatorWorkstations.size() &&
          anyStationAvailable(graph.recipeSubstitutedDominatorWorkstations[recipe]))
        return true;
    }
    return false;
  }

  // Ordinary BFS from the target. Every item is expanded at most once, so the
  // walk is O(V + E) per pass.
  void walk() noexcept {
    std::fill(itemSeen.begin(), itemSeen.end(), 0);
    std::fill(expanded.begin(), expanded.end(), 0);
    std::fill(recipeSeen.begin(), recipeSeen.end(), 0);
    queue.clear();

    const ItemId start = CraftingGraph::itemNode(output);
    itemSeen[start] = 1;
    expanded[start] = 1;
    queue.push_back_unchecked(start);

    for (size_t q = 0; q < queue.size(); q++) {
      const ItemId item = queue[q];
      const bool real = graph.isRealItem(item);

      for (RecipeId recipe : graph.producersOf(item)) {
        if (recipeSeen[recipe] || disabled[recipe] || prunedOut(recipe))
          continue;

        // A pseudo-resource's synthetic recipes don't need workstation.
        // They exist only to unfold the pseudo-resource into one of its real
        // members.
        if (real && !anyStationAvailable(graph.workstations.targetsOf(recipe)))
          continue;

        recipeSeen[recipe] = 1;
        // Every output row of a walked recipe belongs to the subgraph, but an
        // output is only *expanded* -- its own producers visited -- when a
        // walked recipe consumes it: a surplus output must not pull in its
        // other producers by itself.
        for (ItemId out : graph.outputsOf(recipe))
          itemSeen[out] = 1;
        for (ItemId input : graph.inputsOf(recipe)) {
          // Consumed, so its producers matter, however it first got here. A
          // byproduct of one walked recipe is a legitimate input of another
          // (think of a recipe that returns every sibling variant), and not
          // expanding it hides the only route that can seed it: the subgraph
          // then holds an unstartable cycle, which `pruneSeedUnreachable` and
          // the fireability check turn into a false INFEASIBLE.
          itemSeen[input] = 1;
          if (!expanded[input]) {
            expanded[input] = 1;
            queue.push_back_unchecked(input);
          }
        }
      }
    }
  }

  // Pack certificates. A certified recipe is dropped when every recipe of its
  // pack is present in this subgraph and the pack's zero-stock items really
  // are out of stock. Removing every such recipe at once is sound: each one
  // is certified against the walk above, and an optimal plan of that subgraph
  // avoids all of them, so the optimum is unchanged.
  void prunePackCertificates() noexcept {
    if (!options.pack.enabled || graph.packDominated.size() != graph.nRecipe)
      return;
    const uint nRecipe = graph.nRecipe;
    bool any = false;
    aw::vector<uint8_t> drop(nRecipe, 0);
    for (uint recipe = 0; recipe < nRecipe; recipe++) {
      if (!recipeSeen[recipe] || !graph.packDominated[recipe])
        continue;
      const PackCertificate &cert = graph.packCertificates[recipe];
      bool ok = true;
      for (uint32_t support : cert.support) {
        if (support >= nRecipe || !recipeSeen[support]) {
          ok = false;
          break;
        }
      }
      if (!ok)
        continue;
      for (ItemId item : cert.zeroStock) {
        if (held(item) > 0) {
          ok = false;
          break;
        }
      }
      if (!ok)
        continue;
      drop[recipe] = 1;
      any = true;
    }
    if (any) {
      for (uint recipe = 0; recipe < nRecipe; recipe++)
        disabled[recipe] |= drop[recipe];
      walk();
    }
  }

  // Satellite elimination. We have to do it per-query, so not always a net
  // gain.
  void pruneSatellites() noexcept {
    if (!options.satellite.enabled)
      return;
    constexpr uint MAX_SATELLITE_ROUNDS = 4;
    const uint nRecipe = graph.nRecipe;
    const ItemId target = CraftingGraph::itemNode(output);
    aw::vector<uint8_t> drop(nRecipe, 0);
    for (uint round = 0; round < MAX_SATELLITE_ROUNDS; round++) {
      std::fill(drop.begin(), drop.end(), 0);
      if (!computeSatellitePruning(graph, target, itemSeen, recipeSeen, inventory, drop))
        break;
      for (uint recipe = 0; recipe < nRecipe; recipe++)
        disabled[recipe] |= drop[recipe];
      walk();
    }
  }

  // Remove uncraftable items/recipes from stock.
  void pruneSeedUnreachable() noexcept {
    if (!options.seedPruning)
      return;
    const uint nItem = graph.nItem;
    const uint nRecipe = graph.nRecipe;

    // Consumer CSR over the surviving recipes: one entry per input slot, so
    // an item listed twice in one recipe's inputs is counted twice.
    aw::vector<uint> consOffsets(nItem + 1, 0);
    for (uint r = 0; r < nRecipe; r++) {
      if (!recipeSeen[r])
        continue;
      for (ItemId input : graph.inputsOf(r))
        consOffsets[input + 1]++;
    }
    for (ItemId item = 0; item < nItem; item++)
      consOffsets[item + 1] += consOffsets[item];
    aw::vector<uint32_t> consRecipes(consOffsets.back());
    aw::vector<uint> cursor(consOffsets.begin(), consOffsets.end() - 1);
    for (uint r = 0; r < nRecipe; r++) {
      if (!recipeSeen[r])
        continue;
      for (ItemId input : graph.inputsOf(r))
        consRecipes[cursor[input]++] = r;
    }
    
    // missing[r] is the number of input slots not attained yet; UINT32_MAX
    // marks a recipe that is already outside the walk.
    aw::vector<uint32_t> missing(nRecipe, UINT32_MAX);
    for (uint r = 0; r < nRecipe; r++)
      if (recipeSeen[r])
        missing[r] = (uint32_t) graph.inputsOf(r).size();

    aw::vector<uint8_t> obtainable(nItem, 0);
    // Holds reachable items and recipes.
    aw::vector<ItemId> itemStack;
    aw::vector<uint32_t> recipeStack;
    itemStack.reserve(nItem);
    recipeStack.reserve(nRecipe);
    for (ItemId item = 0; item < nItem; item++) {
      if (held(item) > 0) {
        obtainable[item] = 1;
        itemStack.push_back_unchecked(item);
      }
    }
    // A recipe with no inputs is always fireable.
    for (uint r = 0; r < nRecipe; r++) {
      if (recipeSeen[r] && missing[r] == 0)
        recipeStack.push_back_unchecked(r);
    }

    while (!itemStack.empty() || !recipeStack.empty()) {
      while (!itemStack.empty()) {
        const ItemId item = itemStack.back();
        itemStack.pop_back();
        for (uint slot = consOffsets[item], end = consOffsets[item + 1]; slot < end; slot++) {
          const uint32_t r = consRecipes[slot];
          if (missing[r] != UINT32_MAX && --missing[r] == 0)
            recipeStack.push_back_unchecked(r);
        }
      }
      while (!recipeStack.empty()) {
        const uint32_t r = recipeStack.back();
        recipeStack.pop_back();
        // Every output of a fireable recipe is obtained, not just the anchor:
        // a byproduct comes from the same execution, and missing it would
        // disable every consumer of that byproduct.
        for (ItemId out : graph.outputsOf(r)) {
          if (!obtainable[out]) {
            obtainable[out] = 1;
            itemStack.push_back_unchecked(out);
          }
        }
      }
    }

    bool changed = false;
    for (uint r = 0; r < nRecipe; r++) {
      if (!recipeSeen[r] || missing[r] == 0)
        continue;
      disabled[r] = 1;
      recipeSeen[r] = 0;
      changed = true;
    }
    if (changed)
      walk();
  }
};

// Collect the surviving recipes as an explicit, rewritable list.
aw::vector<MutableRecipe> collectSurvivingRecipes(const aw::vector<uint8_t> &recipeSeen) {
  const uint nRecipe = graph.nRecipe;
  aw::vector<MutableRecipe> built;
  built.reserve(nRecipe);
  for (uint r = 0; r < nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    MutableRecipe rec;
    rec.origin = r;
    rec.out = graph.output[r];
    rec.outAmt = graph.outputAmt[r];
    rec.cost = graph.cost[r];
    const auto outs = graph.outputsOf(r);
    const auto outAmts = graph.outputAmountsOf(r);
    rec.outs.reserve(outs.size());
    rec.outAmts.reserve(outAmts.size());
    for (size_t k = 0; k < outs.size(); k++) {
      if (outs[k] == rec.out)
        continue;
      rec.outs.push_back_unchecked(outs[k]);
      rec.outAmts.push_back_unchecked(outAmts[k]);
    }
    // Workstations are not kept in a Subgraph: reachability already filtered
    // by the caller's station set, and every synthesized variant inherits
    // them. They are still copied here so the inliner can tell a synthetic
    // tag edge from a real recipe.
    const auto ws = graph.workstations.targetsOf(r);
    rec.ws.assign(ws.begin(), ws.end());
    const auto inputs = graph.inputsOf(r);
    const auto amounts = graph.inputAmountsOf(r);
    rec.inputs.assign(inputs.begin(), inputs.end());
    rec.amounts.assign(amounts.begin(), amounts.end());
    built.push_back(std::move(rec));
  }
  return built;
}

// A flattened tag has neither producers nor consumers left. Drop it so it
// does not become an empty balance row in the plan matrix.
void dropUnusedItems(aw::vector<uint8_t> &itemSeen, Handle output,
                     const aw::vector<MutableRecipe> &built) {
  aw::vector<uint8_t> used(graph.nItem, 0);
  used[CraftingGraph::itemNode(output)] = 1;
  for (const MutableRecipe &rec : built) {
    used[rec.out] = 1;
    for (ItemId outs : rec.outs)
      used[outs] = 1;
    for (ItemId input : rec.inputs)
      used[input] = 1;
  }
  for (ItemId item = 0; item < graph.nItem; item++)
    if (!used[item])
      itemSeen[item] = 0;
}

// Fill the subgraph from the surviving items and the recipe list: remap
// source item ids to dense subgraph ids and build every CSR array.
Subgraph assembleSubgraph(const aw::vector<uint8_t> &itemSeen,
                          const aw::vector<MutableRecipe> &built) {
  Subgraph result;
  const uint nItem = graph.nItem;

  // Fill the remapping between source graph and subgraph.
  // Use UINT32_MAX for empty entries.
  aw::vector<ItemId> itemMap(nItem, UINT32_MAX);
  // At most one entry per source item.
  result.itemOrigin.reserve(nItem);
  // Note that we can compute the amount of real items in subgraph alongside
  // the way.
  uint subreal = 0;
  for (ItemId item = 0; item < nItem; item++) {
    if (itemSeen[item]) {
      itemMap[item] = result.itemOrigin.size();
      result.itemOrigin.push_back_unchecked(item);
      if (item < graph.nReal)
        subreal++;
    }
  }

  const uint subItems = result.itemOrigin.size();
  const uint subRecipes = built.size();
  result.recipeOrigin.resize(subRecipes);
  for (uint i = 0; i < subRecipes; i++)
    result.recipeOrigin[i] = built[i].origin;

  BaseCraftingGraph &sub = result.graph;
  sub.nReal = subreal;
  sub.nItem = subItems;
  sub.nRecipe = subRecipes;

  // Fill item -> recipe. One edge per output, grouped by output item.
  sub.i2r.offsets.assign(subItems + 1, 0);
  for (const MutableRecipe &rec : built) {
    sub.i2r.offsets[itemMap[rec.out] + 1]++;
    for (ItemId o : rec.outs)
      sub.i2r.offsets[itemMap[o] + 1]++;
  }
  for (uint i = 0; i + 1 < sub.i2r.offsets.size(); i++)
    sub.i2r.offsets[i + 1] += sub.i2r.offsets[i];
  sub.i2r.targets.resize(sub.i2r.offsets.back());
  sub.i2r.weights.resize(sub.i2r.offsets.back());
  {
    aw::vector<uint> cursor(sub.i2r.offsets.begin(), sub.i2r.offsets.end() - 1);
    for (uint i = 0; i < subRecipes; i++) {
      const uint anchorSlot = cursor[itemMap[built[i].out]]++;
      sub.i2r.targets[anchorSlot] = i;
      sub.i2r.weights[anchorSlot] = built[i].outAmt;
      for (size_t k = 0; k < built[i].outs.size(); k++) {
        const uint slot = cursor[itemMap[built[i].outs[k]]]++;
        sub.i2r.targets[slot] = i;
        sub.i2r.weights[slot] = built[i].outAmts[k];
      }
    }
  }

  sub.output.resize(subRecipes);
  sub.outputAmt.resize(subRecipes);
  sub.cost.resize(subRecipes);
  for (uint i = 0; i < subRecipes; i++) {
    sub.output[i] = itemMap[built[i].out];
    sub.outputAmt[i] = built[i].outAmt;
    sub.cost[i] = built[i].cost;
  }

  // Fill recipe -> output. `itemMap` is monotonic, so mapping the sorted
  // byproduct list preserves the ascending order the accessors rely on.
  sub.r2o.offsets.assign(subRecipes + 1, 0);
  for (uint i = 0; i < subRecipes; i++)
    sub.r2o.offsets[i + 1] = sub.r2o.offsets[i] + 1 + (uint) built[i].outs.size();
  sub.r2o.targets.resize(sub.r2o.offsets.back());
  sub.r2o.weights.resize(sub.r2o.offsets.back());
  for (uint i = 0; i < subRecipes; i++) {
    const MutableRecipe &rec = built[i];
    const ItemId anchor = itemMap[rec.out];
    uint slot = sub.r2o.offsets[i];
    size_t k = 0;
    while (k < rec.outs.size() && itemMap[rec.outs[k]] < anchor) {
      sub.r2o.targets[slot] = itemMap[rec.outs[k]];
      sub.r2o.weights[slot] = rec.outAmts[k];
      slot++;
      k++;
    }
    sub.r2o.targets[slot] = anchor;
    sub.r2o.weights[slot] = rec.outAmt;
    slot++;
    while (k < rec.outs.size()) {
      sub.r2o.targets[slot] = itemMap[rec.outs[k]];
      sub.r2o.weights[slot] = rec.outAmts[k];
      slot++;
      k++;
    }
  }

  // Fill recipe -> item.
  sub.r2i.offsets.assign(subRecipes + 1, 0);
  for (uint i = 0; i < subRecipes; i++)
    sub.r2i.offsets[i + 1] = sub.r2i.offsets[i] + (uint) built[i].inputs.size();
  sub.r2i.targets.resize(sub.r2i.offsets.back());
  sub.r2i.weights.resize(sub.r2i.offsets.back());
  for (uint i = 0; i < subRecipes; i++) {
    uint cursor = sub.r2i.offsets[i];
    for (size_t j = 0; j < built[i].inputs.size(); j++) {
      sub.r2i.targets[cursor] = itemMap[built[i].inputs[j]];
      sub.r2i.weights[cursor++] = built[i].amounts[j];
    }
  }

  return result;
}

}  // namespace

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept {
  clearCraftingError();

  // No need to go on to pass 2 if we already found an error.
  Layout layout = scan(bytes);
  if (error != nullptr)
    return;

  const uint nRecipe = layout.ipr.size();
  graph.nReal = layout.nReal;
  graph.nItem = layout.maxHandle;
  graph.nRecipe = nRecipe;

  prefixSum(layout.rpi);
  graph.i2r.offsets = std::move(layout.rpi);
  graph.i2r.targets.resize(graph.i2r.offsets.back());
  graph.i2r.weights.resize(graph.i2r.offsets.back());

  prefixSum(layout.opr);
  graph.r2o.offsets = std::move(layout.opr);
  graph.r2o.targets.resize(graph.r2o.offsets.back());
  graph.r2o.weights.resize(graph.r2o.offsets.back());

  prefixSum(layout.ipr);
  graph.r2i.offsets = std::move(layout.ipr);
  graph.r2i.targets.resize(graph.r2i.offsets.back());
  graph.r2i.weights.resize(graph.r2i.offsets.back());

  prefixSum(layout.wpr);
  graph.workstations.offsets = std::move(layout.wpr);
  graph.workstations.targets.resize(graph.workstations.offsets.back());

  graph.output.resize(nRecipe);
  graph.outputAmt.resize(nRecipe);
  graph.cost.resize(nRecipe);

  // Start filling the grpah.
  ByteReader in(bytes);
  const uint8_t version = checkMagic(in);
  [[maybe_unused]]
  const uint nReal = in.readVarInt();
  const uint nOutput = in.readVarInt();

  // This tracks the next free slot in each row.
  aw::vector<uint> itemCursor(graph.i2r.offsets.begin(), graph.i2r.offsets.end() - 1);

  // Scratch for one recipe's outputs (anchor plus byproducts) before they are
  // sorted, deduplicated and written to `r2o`.
  aw::vector<std::pair<ItemId, Amount>> outputs;
  outputs.reserve(4);

  uint output = 0;
  uint recipe = 0;
  for (uint entry = 0; entry < nOutput; entry++) {
    output += in.readVarInt();
    const uint nRecipe = in.readVarInt();

    for (uint i = 0; i < nRecipe; ++i) {
      // Cost part. Before v3 every real recipe cost exactly one execution and
      // every tag edge was free, which is the default the assignment below
      // keeps.
      Amount cost = 1;
      if (version >= kSchemaV3)
        cost = in.readVarLong();

      const Amount anchorAmt = in.readVarLong();

      uint nWs = in.readVarInt();
      uint wsSlot = graph.workstations.offsets[recipe];
      uint ws = 0;
      for (uint k = 0; k < nWs; ++k) {
        if (k == 0)
          ws = in.readHandle();
        else
          ws += in.readVarInt();
        graph.workstations.targets[wsSlot++] = ws - 1;
      }

      outputs.clear();
      outputs.emplace_back(output - 1, anchorAmt);
      if (version >= kSchemaV2) {
        const uint nByproduct = in.readVarInt();
        uint byproduct = 0;
        for (uint j = 0; j < nByproduct; j++) {
          const Amount amt = in.readVarLong();
          byproduct += in.readVarInt();
          outputs.emplace_back(byproduct - 1, amt);
        }
      }

      // Sort by item and merge a repeated row (an input may also be an output,
      // and a malformed blob may name the anchor twice).
      std::sort(outputs.begin(), outputs.end());
      size_t kept = 0;
      for (size_t k = 0; k < outputs.size();) {
        const ItemId item = outputs[k].first;
        Amount sum = 0;
        while (k < outputs.size() && outputs[k].first == item) {
          sum += outputs[k].second;
          k++;
        }
        outputs[kept++] = {item, sum};
      }
      outputs.resize(kept);

      graph.output[recipe] = output - 1;
      graph.outputAmt[recipe] = anchorAmt;
      // A real column is charged at least one execution: a free producer would
      // not be bounded by the objective cap, and the whole solver relies on
      // `costed column => c_r x_r <= cap`. A tag edge is free by construction,
      // whatever the blob claims, so its stored value is discarded rather than
      // trusted.
      graph.cost[recipe] =
          output - 1 < layout.nReal ? std::max<Amount>(cost, 1) : 0;

      uint r2oSlot = graph.r2o.offsets[recipe];
      for (const auto &[item, amt] : outputs) {
        graph.r2o.targets[r2oSlot] = item;
        graph.r2o.weights[r2oSlot] = amt;
        r2oSlot++;

        const uint itemSlot = itemCursor[item]++;
        graph.i2r.targets[itemSlot] = recipe;
        graph.i2r.weights[itemSlot] = amt;
      }

      uint nInput = in.readVarInt();

      uint recipeSlot = graph.r2i.offsets[recipe];
      uint input = 0;
      for (uint j = 0; j < nInput; j++) {
        Amount inputAmt = in.readVarLong();
        input += in.readVarInt();
        graph.r2i.targets[recipeSlot] = input - 1;
        graph.r2i.weights[recipeSlot] = inputAmt;
        recipeSlot++;
      }

      recipe++;
    }
  }

  // Degenerate input amounts are not flow, then recipes that are net losses on
  // their own output and real recipes with no workstation can never appear in an
  // optimal plan. All three are handled before the passes see the graph, and
  // every rebuild canonicalizes for us, so the explicit call is only needed when
  // neither pass touched the graph.
  const bool normalized = dropNonPositiveInputs(graph);
  const bool dropped = dropUselessRecipes(graph);
  if (!normalized && !dropped)
    canonicalizeRecipes();
  const TagInlineMode inlineMode = options.tagInlining;
  // PRE flattens every single-use tag before the dominance passes see the
  // graph. The query-time spot (QUERY_TIME / BOTH) runs later, inside
  // reachableSubgraph, on the recipes that survived reachability and pruning.
  if (inlineMode == TagInlineMode::PRE_PRUNE || inlineMode == TagInlineMode::BOTH)
    inlineSingleUseTags(graph);
  prune(graph);
}

const char *getCraftingError() noexcept {
  return error;
}

void clearCraftingError() noexcept {
  error = nullptr;
}

const CraftingGraph &getCraftingGraph() noexcept {
  return graph;
}

Subgraph reachableSubgraph(Handle output, std::span<const Handle> workstations,
                           std::span<const Amount> inventory) noexcept {
  if (output == 0 || output > graph.nItem)
    return {};

  AW_PROFILE_BEGIN();
  ReachQuery query(output, workstations, inventory);
  query.walk();
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "reachability walk");
  query.prunePackCertificates();
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "pack certificates");
  query.pruneSeedUnreachable();
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "seed unreachable");
  query.pruneSatellites();
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "satellite elimination");

  aw::vector<MutableRecipe> built = collectSurvivingRecipes(query.recipeSeen);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "collect surviving recipes");

  // A recipe reached through one of its byproducts still outputs its anchor,
  // and every output row has to exist in the subgraph. The walk only visits
  // items it needs, so mark the remaining outputs of the surviving recipes
  // here, before any assembly or inlining reads `itemSeen`.
  for (const MutableRecipe &rec : built) {
    query.itemSeen[rec.out] = 1;
    for (ItemId o : rec.outs)
      query.itemSeen[o] = 1;
  }
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "mark recipe outputs");

  // Query-time single-use inlining. At this point `recipeSeen` already
  // reflects tag pruning and the inventory guard, so a tag's surviving member
  // edges are exactly the members the player can still spend -- including a
  // dominated member that is in stock. Folding them into the consumer removes
  // the tag node without dropping any option, and without the subgraph growth
  // the registration-time flattening has.
  if (options.tagInlining == TagInlineMode::QUERY_TIME ||
      options.tagInlining == TagInlineMode::BOTH) {
    inlineSingleUseTagsCore(built, graph.nReal, graph.nItem,
                            options.inlineSingleMemberTags, [](uint) { return true; });
    dropUnusedItems(query.itemSeen, output, built);
  }
  AW_PROFILE_END(options.outputRepruningProfile, "[time/reach]", "inline single-use tags");

  // Query-time re-pruning. The passes are written against a whole crafting
  // graph, so the live subgraph is assembled once as a probe, the newly
  // dominated recipes are found, and the survivors are assembled again. This
  // runs after the inliner so the composite relation sees real inputs where a
  // tag used to be.
  if (!options.flash ? options.reprune.enabled : options.reprune.enabledOnFlash) {
    const Subgraph probe = assembleSubgraph(query.itemSeen, built);
    const aw::vector<uint8_t> drop = aw::detail::repruneSubgraph(probe, inventory);
    size_t kept = 0;
    bool any = false;
    for (size_t i = 0; i < built.size(); i++) {
      if (i < drop.size() && drop[i]) {
        any = true;
        continue;
      }
      if (kept != i)
        built[kept] = std::move(built[i]);
      kept++;
    }
    built.resize(kept);
    if (any)
      dropUnusedItems(query.itemSeen, output, built);
  }
  AW_PROFILE_END(options.outputRepruningProfile, "[time/pass]", "re-pruning");

  Subgraph result = assembleSubgraph(query.itemSeen, built);
  AW_PROFILE_END(options.outputRepruningProfile, "[time/pass]", "assemble subgraph");

  const auto runPass = [&](bool enabled, const char *label, auto computeFn) {
    if (!enabled)
      return;

    AW_PROFILE_BEGIN();
    const ItemId subTarget = result.translate(CraftingGraph::itemNode(output));
    if (subTarget != UINT32_MAX) {
      aw::vector<uint8_t> removeMask(result.graph.nRecipe, 0);
      if (computeFn(result, subTarget, inventory, removeMask)) {
        size_t idx = 0;
        auto newEnd = std::remove_if(built.begin(), built.end(), [&](const auto&) {
          return (idx < removeMask.size()) && removeMask[idx++];
        });
        built.erase(newEnd, built.end());

        dropUnusedItems(query.itemSeen, output, built);
        result = assembleSubgraph(query.itemSeen, built);
      }
    }
    AW_PROFILE_END(options.outputRepruningProfile, "[time/pass]", label);
  };

  runPass(options.variantClass.enabled, "variant class", computeVariantClassPruning);
  runPass(options.tagExclusive.enabled, "tag exclusive", computeTagExclusivePruning);
  for (int i = 0; i < 30; i++)
    runPass(options.variantFold.enabled, "variant fold", computeVariantFoldPruning);
  runPass(options.satellite.enabled, "closed island", computeClosedIslandPruning);
  return result;
}


ItemId Subgraph::translate(ItemId sourceNode) const noexcept {
  const auto begin = itemOrigin.begin();
  const auto end = itemOrigin.end();
  const auto it = std::lower_bound(begin, end, sourceNode);
  if (it == end || *it != sourceNode)
    return UINT32_MAX;
  return it - begin;
}

} // namespace aw
