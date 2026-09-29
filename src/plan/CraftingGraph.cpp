#include "aw/plan/Options.h"
#include "Prune.h"

#include <array>
#include <climits>


namespace aw {
CraftingGraph graph;
// Last parse error, or nullptr when the last registerCraftingGraph succeeded.
// The Java side reads this to turn a malformed blob into an exception.
const char *error;

namespace {

constexpr std::array<uint8_t, 4> kMagic = {'A', 'W', 'R', 0x01};

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

void checkMagic(ByteReader& in) noexcept {
  for (std::uint8_t expected : kMagic) {
    if (in.readByte() != expected)
      fail("invalid magic bits");
  }
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

  // Check magic bits.
  checkMagic(in);

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
      in.readVarLong();  // output amount
      layout.wpr.push_back(readWorkstations(in, layout));

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
// item, the output amount and the amounts of every input.
struct RecipeKeyLess {
  const BaseCraftingGraph &g;

  bool operator()(uint a, uint b) const noexcept {
    if (g.output[a] != g.output[b])
      return g.output[a] < g.output[b];
    if (g.outputAmt[a] != g.outputAmt[b])
      return g.outputAmt[a] < g.outputAmt[b];

    const auto ta = g.r2i.targetsOf(a);
    const auto tb = g.r2i.targetsOf(b);
    const auto wa = g.r2i.weightsOf(a);
    const auto wb = g.r2i.weightsOf(b);
    const size_t shared = ta.size() < tb.size() ? ta.size() : tb.size();
    for (size_t i = 0; i < shared; i++) {
      if (ta[i] != tb[i])
        return ta[i] < tb[i];
      if (wa[i] != wb[i])
        return wa[i] < wb[i];
    }
    return ta.size() < tb.size();
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

  aw::vector<NodeId> wsTargets(wsOffsets.back());
  aw::vector<uint> wsCursor(wsOffsets.begin(), wsOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    const uint dst = newId[rep[r]];
    for (NodeId station : graph.workstations.targetsOf(r))
      wsTargets[wsCursor[dst]++] = station;
  }

  aw::vector<uint> newWsOffsets(newNRecipe + 1, 0);
  aw::vector<NodeId> newWsTargets;
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

  // Rebuild item -> recipe edges.
  aw::vector<uint> itemOffsets(graph.nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      itemOffsets[graph.output[r]]++;
  }
  prefixSum(itemOffsets);

  aw::vector<NodeId> itemTargets(newNRecipe, 0);
  aw::vector<Amount> itemWeights(newNRecipe, 0);
  aw::vector<uint> itemCursor(itemOffsets.begin(), itemOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    const uint slot = itemCursor[graph.output[r]]++;
    itemTargets[slot] = graph.nItem + newId[r];
    itemWeights[slot] = graph.outputAmt[r];
  }
  graph.i2r.offsets = std::move(itemOffsets);
  graph.i2r.targets = std::move(itemTargets);
  graph.i2r.weights = std::move(itemWeights);

  // Rebuild recipe -> item edges.
  aw::vector<uint> inputOffsets(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      inputOffsets[newId[r]] = (uint) graph.r2i.targetsOf(r).size();
  }
  prefixSum(inputOffsets);

  aw::vector<NodeId> inputTargets(inputOffsets.back(), 0);
  aw::vector<Amount> inputWeights(inputOffsets.back(), 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    uint slot = inputOffsets[newId[r]];
    const auto targets = graph.r2i.targetsOf(r);
    const auto weights = graph.r2i.weightsOf(r);
    for (size_t k = 0; k < targets.size(); k++) {
      inputTargets[slot] = targets[k];
      inputWeights[slot] = weights[k];
      slot++;
    }
  }
  graph.r2i.offsets = std::move(inputOffsets);
  graph.r2i.targets = std::move(inputTargets);
  graph.r2i.weights = std::move(inputWeights);

  aw::vector<NodeId> output(newNRecipe, 0);
  aw::vector<Amount> outputAmt(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] != r)
      continue;
    output[newId[r]] = graph.output[r];
    outputAmt[newId[r]] = graph.outputAmt[r];
  }
  graph.output = std::move(output);
  graph.outputAmt = std::move(outputAmt);
  graph.nRecipe = newNRecipe;
}

// ---------------------------------------------------------------------------
// Single-use tag inlining
// ---------------------------------------------------------------------------
// See TagInlineMode in the header. A tag T with member edges `T <- m1 .. T <-
// mn` and exactly one real consumer `R: a <- T, ...` that consumes T once at
// amount 1 is replaced by the n real recipes `a <- m1, ...`: the tag node, its
// n member edges and R disappear, and n recipes take their place. The recipe
// count therefore drops by exactly one per inlined tag.
//
// The transform is a fixpoint, but it cannot cascade into a product of member
// sets: the n copies of R are consumers of every *other* tag R used, so those
// tags stop being single-use and are never expanded. Each inlining strictly
// decreases the recipe count, so the loop terminates.

struct MutableRecipe {
  NodeId out = 0;
  Amount outAmt = 0;
  aw::vector<NodeId> ws;
  aw::vector<NodeId> inputs;
  aw::vector<Amount> amounts;
  // Source recipe id, or UINT32_MAX for a recipe the query-time inliner made.
  uint32_t origin = UINT32_MAX;
};

// The core fixpoint. `allowed` is consulted once, while the member sets are
// collected, and receives the index of a recipe in `recipes`. The query-time
// path accepts everything: it only ever hands in the recipes that survived
// reachability and pruning, so a tag's surviving member edges are exactly the
// members whose stock the player can still spend.
template <typename Allowed>
bool inlineSingleUseTagsCore(aw::vector<MutableRecipe> &recipes, uint nReal,
                             uint nItem, Allowed allowed) {
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

  aw::vector<aw::vector<NodeId>> members(nItem);
  for (uint r = 0; r < recipes.size(); r++) {
    const MutableRecipe& rec = recipes[r];
    if (rec.out < nReal || !simpleTag[rec.out] || !allowed(r))
      continue;
    members[rec.out].push_back(rec.inputs[0]);
  }

  // The consumer side of a tag: the number of input slots that name it, the
  // (single) recipe that owns the slot and the amount it consumes.
  aw::vector<uint32_t> consumerCount(nItem, 0);
  aw::vector<uint32_t> consumerRecipe(nItem, 0);
  aw::vector<Amount> consumerAmount(nItem, 0);
  bool changed = false;

  while (true) {
    std::fill(consumerCount.begin(), consumerCount.end(), 0);
    for (uint i = 0; i < recipes.size(); i++) {
      const MutableRecipe& rec = recipes[i];
      if (rec.out >= nReal)
        continue;
      for (size_t k = 0; k < rec.inputs.size(); k++) {
        const NodeId j = rec.inputs[k];
        if (j < nReal)
          continue;
        consumerCount[j]++;
        consumerRecipe[j] = i;
        consumerAmount[j] = rec.amounts[k];
      }
    }

    uint32_t chosen = UINT32_MAX;
    for (uint t = nReal; t < nItem; t++) {
      if (simpleTag[t] && !members[t].empty() && consumerCount[t] == 1 &&
          consumerAmount[t] == 1) {
        chosen = t;
        break;
      }
    }
    if (chosen == UINT32_MAX)
      break;

    const uint t = chosen;
    const uint32_t consumer = consumerRecipe[t];
    const aw::vector<NodeId> ms = members[t];

    // The consumer without its tag input. Inputs are strictly ascending, so
    // this keeps them ordered.
    aw::vector<NodeId> newInputs;
    aw::vector<Amount> newAmounts;
    newInputs.reserve(recipes[consumer].inputs.size());
    newAmounts.reserve(recipes[consumer].inputs.size());
    for (size_t k = 0; k < recipes[consumer].inputs.size(); k++) {
      if (recipes[consumer].inputs[k] == t)
        continue;
      newInputs.push_back_unchecked(recipes[consumer].inputs[k]);
      newAmounts.push_back_unchecked(recipes[consumer].amounts[k]);
    }

    // Drop the consumer and every member edge of the tag, then add a copy of
    // the consumer per member. recipes[consumer] is skipped rather than moved,
    // so it is still readable while the copies are built.
    aw::vector<MutableRecipe> next;
    next.reserve(recipes.size() - 1);
    for (uint i = 0; i < recipes.size(); i++) {
      if (i == consumer || recipes[i].out == t)
        continue;
      next.push_back(std::move(recipes[i]));
    }
    for (NodeId m : ms) {
      MutableRecipe copy;
      copy.out = recipes[consumer].out;
      copy.outAmt = recipes[consumer].outAmt;
      copy.ws = recipes[consumer].ws;
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
    recipes.swap(next);
    changed = true;
  }
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
    const auto ws = graph.workstations.targetsOf(r);
    rec.ws.assign(ws.begin(), ws.end());
    const auto inputs = graph.r2i.targetsOf(r);
    const auto amounts = graph.r2i.weightsOf(r);
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

  // Rebuild item -> recipe.
  aw::vector<uint> itemOffsets(nItem, 0);
  for (const MutableRecipe& rec : recipes)
    itemOffsets[rec.out]++;
  prefixSum(itemOffsets);
  graph.i2r.offsets = std::move(itemOffsets);
  graph.i2r.targets.resize(newNRecipe);
  graph.i2r.weights.resize(newNRecipe);
  {
    aw::vector<uint> cursor(graph.i2r.offsets.begin(), graph.i2r.offsets.end() - 1);
    for (uint r = 0; r < newNRecipe; r++) {
      const uint slot = cursor[recipes[r].out]++;
      graph.i2r.targets[slot] = graph.recipeNode(r);
      graph.i2r.weights[slot] = recipes[r].outAmt;
    }
  }

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
    for (NodeId w : recipes[r].ws)
      graph.workstations.targets[slot++] = w;
  }

  graph.output.resize(newNRecipe);
  graph.outputAmt.resize(newNRecipe);
  for (uint r = 0; r < newNRecipe; r++) {
    graph.output[r] = recipes[r].out;
    graph.outputAmt[r] = recipes[r].outAmt;
  }
  graph.nRecipe = newNRecipe;

  canonicalizeRecipes();
}

// The amount of a recipe's own output that the recipe also consumes.
Amount selfConsumption(const MutableRecipe& rec) noexcept {
  Amount self = 0;
  for (size_t k = 0; k < rec.inputs.size(); k++)
    if (rec.inputs[k] == rec.out)
      self += rec.amounts[k];
  return self;
}

// The same quantity read off the CSR form, without materializing the recipes.
Amount selfConsumptionAt(const CraftingGraph& graph, uint r) noexcept {
  const auto inputs = graph.r2i.targetsOf(r);
  const auto amounts = graph.r2i.weightsOf(r);
  Amount self = 0;
  for (size_t k = 0; k < inputs.size(); k++)
    if (inputs[k] == graph.output[r])
      self += amounts[k];
  return self;
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
    for (Amount amount : graph.r2i.weightsOf(r))
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
//   * A recipe that consumes at least as much of its own output as it produces
//     has a column v_r <= 0: the output row is `p - c <= 0` and every other row
//     is a consumption. It is therefore dominated by doing nothing -- deleting
//     one of its executions from any feasible plan keeps the plan feasible and
//     strictly cheaper -- so no optimal plan uses it, for any target and any
//     inventory. This needs no stock or workstation guard. It covers the
//     synthetic self-loops and the per-output copies of multi-output recipes.
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
    return rec.out < graph.nReal && selfConsumption(rec) >= rec.outAmt;
  };
  const auto netLossAt = [&](uint r) noexcept {
    return graph.output[r] < graph.nReal &&
           selfConsumptionAt(graph, r) >= graph.outputAmt[r];
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
  aw::vector<uint8_t> recipeSeen;
  aw::vector<uint8_t> disabled;
  aw::vector<NodeId> queue;

  ReachQuery(Handle output, std::span<const Handle> workstations,
             std::span<const Amount> inventory) noexcept
      : output(output), inventory(inventory), allowed(graph.nItem, 0),
        itemSeen(graph.nItem, 0), recipeSeen(graph.nRecipe, 0),
        disabled(graph.nRecipe, 0) {
    for (Handle handle : workstations) {
      if (handle >= 1 && handle <= graph.nItem)
        allowed[handle - 1] = 1;
    }
    // Enqueued once per item at most, so nItem is a hard bound; the capacity
    // survives the clear() at the top of every walk.
    queue.reserve(graph.nItem);
  }

  // Stock of a source item, 0 when the inventory does not cover it.
  Amount held(NodeId item) const noexcept {
    return item < inventory.size() ? inventory[item] : 0;
  }

  // True when one of `stations` is available in this query.
  bool anyStationAvailable(std::span<const NodeId> stations) const noexcept {
    for (NodeId station : stations)
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
      const auto inputs = graph.r2i.targetsOf(recipe);
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
      for (NodeId guard : graph.recipeSubstitutedGuards[recipe]) {
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

  // Ordinary BFS from the target.
  void walk() noexcept {
    std::fill(itemSeen.begin(), itemSeen.end(), 0);
    std::fill(recipeSeen.begin(), recipeSeen.end(), 0);
    queue.clear();

    const NodeId start = CraftingGraph::itemNode(output);
    itemSeen[start] = 1;
    queue.push_back_unchecked(start);

    for (size_t q = 0; q < queue.size(); q++) {
      const NodeId item = queue[q];
      const bool real = graph.isRealItem(item);

      for (NodeId recipeNode : graph.i2r.targetsOf(item)) {
        const uint recipe = recipeNode - graph.nItem;
        if (recipeSeen[recipe] || disabled[recipe] || prunedOut(recipe))
          continue;

        // A pseudo-resource's synthetic recipes don't need workstation.
        // They exist only to unfold the pseudo-resource into one of its real
        // members.
        if (real && !anyStationAvailable(graph.workstations.targetsOf(recipe)))
          continue;

        recipeSeen[recipe] = 1;
        for (NodeId input : graph.r2i.targetsOf(recipe)) {
          if (!itemSeen[input]) {
            itemSeen[input] = 1;
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
      for (NodeId item : cert.zeroStock) {
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
    const NodeId target = CraftingGraph::itemNode(output);
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

  // Remove dead (un-produce-able) nodes.
  void pruneDeadNodes() noexcept {
    if (!options.deadNodePruning)
      return;
    const uint nItem = graph.nItem;
    const uint nRecipe = graph.nRecipe;
    const NodeId target = CraftingGraph::itemNode(output);

    // How many surviving recipes produce each item.
    aw::vector<uint> produced(nItem, 0);
    for (uint r = 0; r < nRecipe; r++)
      if (recipeSeen[r])
        produced[graph.output[r]]++;

    // Item -> surviving recipes that consume it, as a CSR.
    aw::vector<uint> consOffsets(nItem + 1, 0);
    for (uint r = 0; r < nRecipe; r++) {
      if (!recipeSeen[r])
        continue;
      for (NodeId input : graph.r2i.targetsOf(r))
        consOffsets[input + 1]++;
    }
    for (NodeId item = 0; item < nItem; item++)
      consOffsets[item + 1] += consOffsets[item];
    aw::vector<uint> consTargets(consOffsets.back());
    aw::vector<uint> cursor(consOffsets.begin(), consOffsets.end() - 1);
    for (uint r = 0; r < nRecipe; r++) {
      if (!recipeSeen[r])
        continue;
      for (NodeId input : graph.r2i.targetsOf(r))
        consTargets[cursor[input]++] = r;
    }
    

    auto usable = [&](NodeId item) {
      return item == target || produced[item] != 0 || held(item) != 0;
    };

    aw::vector<NodeId> dead;
    dead.reserve(nItem);
    aw::vector<uint8_t> queued(nItem, 0);
    for (NodeId item = 0; item < nItem; item++) {
      if (itemSeen[item] && !usable(item)) {
        queued[item] = 1;
        dead.push_back_unchecked(item);
      }
    }

    bool changed = false;
    for (size_t q = 0; q < dead.size(); q++) {
      const NodeId item = dead[q];
      for (uint slot = consOffsets[item]; slot < consOffsets[item + 1]; slot++) {
        const uint r = consTargets[slot];
        if (disabled[r])
          continue;
        disabled[r] = 1;
        recipeSeen[r] = 0;
        changed = true;
        const NodeId out = graph.output[r];
        if (produced[out] > 0)
          produced[out]--;
        if (produced[out] == 0 && itemSeen[out] && !queued[out] && !usable(out)) {
          queued[out] = 1;
          dead.push_back_unchecked(out);
        }
      }
    }
    // Rebuild the walk so items only needed by the dropped recipes disappear
    // too; the closure above guarantees no new producer-less item appears.
    if (changed)
      walk();
  }

  // Seed-reachability pruning. Reachability from the target and dead-node
  // cleanup both keep recipes that need an item nothing can produce from the
  // player's stock. The solver's balance is a net condition, so such a recipe
  // can look useful -- a loop's net output can pay for its own seed on
  // paper -- even though it can never fire. An item is attainable when the
  // player holds it, or a surviving recipe with every input attainable
  // produces it (a recipe with no inputs fires from nowhere). A recipe with
  // an unattainable input is dropped. This only ever removes recipes no
  // firing sequence can use, so it is sound and never loses a realizable
  // plan.
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
      for (NodeId input : graph.r2i.targetsOf(r))
        consOffsets[input + 1]++;
    }
    for (NodeId item = 0; item < nItem; item++)
      consOffsets[item + 1] += consOffsets[item];
    aw::vector<uint32_t> consRecipes(consOffsets.back());
    {
      aw::vector<uint> cursor(consOffsets.begin(), consOffsets.end() - 1);
      for (uint r = 0; r < nRecipe; r++) {
        if (!recipeSeen[r])
          continue;
        for (NodeId input : graph.r2i.targetsOf(r))
          consRecipes[cursor[input]++] = r;
      }
    }

    // missing[r] is the number of input slots not attained yet; UINT32_MAX
    // marks a recipe that is already outside the walk.
    aw::vector<uint32_t> missing(nRecipe, UINT32_MAX);
    for (uint r = 0; r < nRecipe; r++)
      if (recipeSeen[r])
        missing[r] = (uint32_t) graph.r2i.targetsOf(r).size();

    // An item no surviving recipe produces is a raw material the player is
    // expected to gather, the same assumption deadNodePruning makes, so it
    // seeds the closure. Recipes that need it are kept; the balance still
    // refuses to spend an unstocked raw material, so this only avoids pruning
    // the gatherable route rather than making it usable.
    aw::vector<uint8_t> producible(nItem, 0);
    for (uint r = 0; r < nRecipe; r++)
      if (recipeSeen[r])
        producible[graph.output[r]] = 1;

    aw::vector<uint8_t> attainable(nItem, 0);
    aw::vector<NodeId> itemStack;
    aw::vector<uint32_t> recipeStack;
    itemStack.reserve(nItem);
    recipeStack.reserve(nRecipe);
    for (NodeId item = 0; item < nItem; item++) {
      if (held(item) > 0 || !producible[item]) {
        attainable[item] = 1;
        itemStack.push_back_unchecked(item);
      }
    }
    // A recipe with no inputs fires from nowhere.
    for (uint r = 0; r < nRecipe; r++)
      if (recipeSeen[r] && missing[r] == 0)
        recipeStack.push_back_unchecked(r);

    while (!itemStack.empty() || !recipeStack.empty()) {
      while (!itemStack.empty()) {
        const NodeId item = itemStack.back();
        itemStack.pop_back();
        for (uint slot = consOffsets[item]; slot < consOffsets[item + 1]; slot++) {
          const uint32_t r = consRecipes[slot];
          if (missing[r] != UINT32_MAX && --missing[r] == 0)
            recipeStack.push_back_unchecked(r);
        }
      }
      while (!recipeStack.empty()) {
        const uint32_t r = recipeStack.back();
        recipeStack.pop_back();
        const NodeId out = graph.output[r];
        if (!attainable[out]) {
          attainable[out] = 1;
          itemStack.push_back_unchecked(out);
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

// Collect the surviving recipes as an explicit, rewritable list. Everything
// below is built from it, which is what lets the query-time inliner add
// recipes that have no single source counterpart.
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
    // Workstations are not kept in a Subgraph: reachability already filtered
    // by the caller's station set, and every synthesized variant inherits
    // them. They are still copied here so the inliner can tell a synthetic
    // tag edge from a real recipe.
    const auto ws = graph.workstations.targetsOf(r);
    rec.ws.assign(ws.begin(), ws.end());
    const auto inputs = graph.r2i.targetsOf(r);
    const auto amounts = graph.r2i.weightsOf(r);
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
    for (NodeId input : rec.inputs)
      used[input] = 1;
  }
  for (NodeId item = 0; item < graph.nItem; item++)
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
  aw::vector<NodeId> itemMap(nItem, UINT32_MAX);
  // At most one entry per source item.
  result.itemOrigin.reserve(nItem);
  // Note that we can compute the amount of real items in subgraph alongside
  // the way.
  uint subreal = 0;
  for (NodeId item = 0; item < nItem; item++) {
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

  // Fill item -> recipe. One edge per recipe, grouped by output item.
  sub.i2r.offsets.assign(subItems + 1, 0);
  for (const MutableRecipe &rec : built)
    sub.i2r.offsets[itemMap[rec.out] + 1]++;
  for (uint i = 0; i + 1 < sub.i2r.offsets.size(); i++)
    sub.i2r.offsets[i + 1] += sub.i2r.offsets[i];
  sub.i2r.targets.resize(subRecipes);
  sub.i2r.weights.resize(subRecipes);
  {
    aw::vector<uint> cursor(sub.i2r.offsets.begin(), sub.i2r.offsets.end() - 1);
    for (uint i = 0; i < subRecipes; i++) {
      const uint slot = cursor[itemMap[built[i].out]]++;
      sub.i2r.targets[slot] = sub.nItem + i;
      sub.i2r.weights[slot] = built[i].outAmt;
    }
  }

  sub.output.resize(subRecipes);
  sub.outputAmt.resize(subRecipes);
  for (uint i = 0; i < subRecipes; i++) {
    sub.output[i] = itemMap[built[i].out];
    sub.outputAmt[i] = built[i].outAmt;
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
  graph.i2r.targets.resize(nRecipe);
  graph.i2r.weights.resize(nRecipe);

  prefixSum(layout.ipr);
  graph.r2i.offsets = std::move(layout.ipr);
  graph.r2i.targets.resize(graph.r2i.offsets.back());
  graph.r2i.weights.resize(graph.r2i.offsets.back());

  prefixSum(layout.wpr);
  graph.workstations.offsets = std::move(layout.wpr);
  graph.workstations.targets.resize(graph.workstations.offsets.back());

  graph.output.resize(nRecipe);
  graph.outputAmt.resize(nRecipe);

  // Start filling the grpah.
  ByteReader in(bytes);
  checkMagic(in);
  [[maybe_unused]]
  const uint nReal = in.readVarInt();
  const uint nOutput = in.readVarInt();

  // This tracks the next free slot in each row.
  aw::vector<uint> itemCursor(graph.i2r.offsets.begin(), graph.i2r.offsets.end() - 1);

  uint output = 0;
  uint recipe = 0;
  for (uint entry = 0; entry < nOutput; entry++) {
    output += in.readVarInt();
    const uint nRecipe = in.readVarInt();

    for (uint i = 0; i < nRecipe; ++i) {
      Amount outputAmt = in.readVarLong();

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

      uint nInput = in.readVarInt();

      graph.output[recipe] = output - 1;
      graph.outputAmt[recipe] = outputAmt;

      uint itemSlot = itemCursor[output - 1]++;
      graph.i2r.targets[itemSlot] = graph.recipeNode(recipe);
      graph.i2r.weights[itemSlot] = outputAmt;

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

  ReachQuery query(output, workstations, inventory);
  query.walk();
  query.prunePackCertificates();
  query.pruneSatellites();
  query.pruneDeadNodes();
  query.pruneSeedUnreachable();

  aw::vector<MutableRecipe> built = collectSurvivingRecipes(query.recipeSeen);

  // Query-time single-use inlining. At this point `recipeSeen` already
  // reflects tag pruning and the inventory guard, so a tag's surviving member
  // edges are exactly the members the player can still spend -- including a
  // dominated member that is in stock. Folding them into the consumer removes
  // the tag node without dropping any option, and without the subgraph growth
  // the registration-time flattening has.
  if (options.tagInlining == TagInlineMode::QUERY_TIME ||
      options.tagInlining == TagInlineMode::BOTH) {
    inlineSingleUseTagsCore(built, graph.nReal, graph.nItem, [](uint) { return true; });
    dropUnusedItems(query.itemSeen, output, built);
  }

  return assembleSubgraph(query.itemSeen, built);
}


NodeId Subgraph::translate(NodeId sourceNode) const noexcept {
  const auto begin = itemOrigin.begin();
  const auto end = itemOrigin.end();
  const auto it = std::lower_bound(begin, end, sourceNode);
  if (it == end || *it != sourceNode)
    return UINT32_MAX;
  return it - begin;
}

} // namespace aw
