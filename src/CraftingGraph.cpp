#include "aw/CraftingGraph.h"

#include "Prune.h"

#include <algorithm>
#include <array>

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
  std::vector<uint> rpi;
  // Maps `recipe` to its number of items (items per recipe).
  std::vector<uint> ipr;
  // Maps `recipe` to its number of workstations.
  std::vector<uint> wpr;

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

void prefixSum(std::vector<uint> &v) noexcept {
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
  std::vector<uint> order(nRecipe);
  for (uint r = 0; r < nRecipe; r++)
    order[r] = r;
  std::sort(order.begin(), order.end(), RecipeKeyLess{graph});

  const RecipeKeyLess less { graph };
  // Deduplication and grouping.
  std::vector<uint> rep(nRecipe, UINT32_MAX);
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
  std::vector<uint> newId(nRecipe, UINT32_MAX);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      newId[r] = newNRecipe++;
  }
  if (newNRecipe == nRecipe)
    return;

  // Rebuild workstations.
  std::vector<uint> wsOffsets(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++)
    wsOffsets[newId[rep[r]]] += graph.workstations.targetsOf(r).size();
  prefixSum(wsOffsets);

  std::vector<NodeId> wsTargets(wsOffsets.back());
  std::vector<uint> wsCursor(wsOffsets.begin(), wsOffsets.end() - 1);
  for (uint r = 0; r < nRecipe; r++) {
    const uint dst = newId[rep[r]];
    for (NodeId station : graph.workstations.targetsOf(r))
      wsTargets[wsCursor[dst]++] = station;
  }

  std::vector<uint> newWsOffsets(newNRecipe + 1, 0);
  std::vector<NodeId> newWsTargets;
  newWsTargets.reserve(wsTargets.size());
  for (uint j = 0; j < newNRecipe; j++) {
    std::sort(wsTargets.begin() + wsOffsets[j], wsTargets.begin() + wsOffsets[j + 1]);
    for (uint e = wsOffsets[j]; e < wsOffsets[j + 1]; e++) {
      if (e > wsOffsets[j] && wsTargets[e] == wsTargets[e - 1])
        continue;
      newWsTargets.push_back(wsTargets[e]);
    }
    newWsOffsets[j + 1] = newWsTargets.size();
  }
  graph.workstations.offsets = std::move(newWsOffsets);
  graph.workstations.targets = std::move(newWsTargets);

  // Rebuild item -> recipe edges.
  std::vector<uint> itemOffsets(graph.nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      itemOffsets[graph.output[r]]++;
  }
  prefixSum(itemOffsets);

  std::vector<NodeId> itemTargets(newNRecipe, 0);
  std::vector<Amount> itemWeights(newNRecipe, 0);
  std::vector<uint> itemCursor(itemOffsets.begin(), itemOffsets.end() - 1);
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
  std::vector<uint> inputOffsets(newNRecipe, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (rep[r] == r)
      inputOffsets[newId[r]] = (uint) graph.r2i.targetsOf(r).size();
  }
  prefixSum(inputOffsets);

  std::vector<NodeId> inputTargets(inputOffsets.back(), 0);
  std::vector<Amount> inputWeights(inputOffsets.back(), 0);
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

  std::vector<NodeId> output(newNRecipe, 0);
  std::vector<Amount> outputAmt(newNRecipe, 0);
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
  std::vector<uint> itemCursor(graph.i2r.offsets.begin(), graph.i2r.offsets.end() - 1);

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

  canonicalizeRecipes();
  computePruning(graph);
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
  Subgraph result;
  if (output == 0 || output > graph.nItem)
    return result;

  const uint nItem = graph.nItem;
  const uint nRecipe = graph.nRecipe;

  // Turn workstation availability into a bitset (in fact a byteset).
  std::vector<uint8_t> allowed(nItem, 0);
  for (Handle handle : workstations) {
    if (handle >= 1 && handle <= nItem)
      allowed[handle - 1] = 1;
  }

  const bool pruneTag = isTagPruningEnabled();
  const bool pruneRecipe = isRecipePruningEnabled();
  const bool prunePack = isPackPruningEnabled();

  std::vector<uint8_t> itemSeen(nItem, 0);
  std::vector<uint8_t> recipeSeen(nRecipe, 0);
  std::vector<uint8_t> disabled(nRecipe, 0);
  std::vector<NodeId> queue;

  // Ordinary BFS. `disabled` forces a recipe out of the walk without touching
  // the flags, so the pack-certificate post-pass can rebuild the subgraph.
  auto walk = [&]() {
    std::fill(itemSeen.begin(), itemSeen.end(), 0);
    std::fill(recipeSeen.begin(), recipeSeen.end(), 0);
    queue.clear();

    const NodeId start = CraftingGraph::itemNode(output);
    itemSeen[start] = 1;
    queue.push_back(start);

    for (size_t q = 0; q < queue.size(); q++) {
      const NodeId item = queue[q];
      const bool real = graph.isRealItem(item);

      for (NodeId recipeNode : graph.i2r.targetsOf(item)) {
        const uint recipe = recipeNode - nItem;
        if (recipeSeen[recipe] || disabled[recipe])
          continue;

        // A dominated tag edge is dropped unless the player actually holds the
        // member, in which case the free stock can still be spent on the tag.
        if (pruneTag && recipe < graph.tagEdgeDominated.size() &&
            graph.tagEdgeDominated[recipe]) {
          const auto inputs = graph.r2i.targetsOf(recipe);
          const Amount held = !inputs.empty() && inputs[0] < inventory.size()
                                  ? inventory[inputs[0]]
                                  : 0;
          if (held == 0)
            continue;
        }

        // A composite-dominated real recipe is likewise only dropped when the
        // witness input has no stock, so held stock can still be spent through
        // it.
        if (pruneRecipe && recipe < graph.recipeDominated.size() &&
            graph.recipeDominated[recipe]) {
          const NodeId guard = graph.recipeGuardInput[recipe];
          const Amount held = guard < inventory.size() ? inventory[guard] : 0;
          if (held == 0)
            continue;
        }

        // A pseudo-resource's synthetic recipes don't need workstation.
        // They exist only to unfold the pseudo-resource into one of its real members.
        if (real) {
          bool usable = false;
          for (NodeId station : graph.workstations.targetsOf(recipe)) {
            if (allowed[station]) {
              usable = true;
              break;
            }
          }
          if (!usable)
            continue;
        }

        recipeSeen[recipe] = 1;
        for (NodeId input : graph.r2i.targetsOf(recipe)) {
          if (!itemSeen[input]) {
            itemSeen[input] = 1;
            queue.push_back(input);
          }
        }
      }
    }
  };

  walk();

  // Pack certificates. A certified recipe is dropped when every recipe of its
  // pack is present in this subgraph and the pack's zero-stock items really
  // are out of stock. Removing every such recipe at once is sound: each one is
  // certified against the walk above, and an optimal plan of that subgraph
  // avoids all of them, so the optimum is unchanged.
  if (prunePack && graph.packDominated.size() == nRecipe) {
    bool any = false;
    std::vector<uint8_t> drop(nRecipe, 0);
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
        const Amount held = item < inventory.size() ? inventory[item] : 0;
        if (held > 0) {
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

  // Satellite elimination (docs/algorithm.typ, "孤岛消除"). Unlike the passes
  // above it needs the target and the inventory, so it cannot be precomputed at
  // registration time. Dropping a component can expose new articulation points,
  // hence the repeat; the pass carries its own wall-clock budget, so the loop
  // bound is only there to keep a pathological graph from spinning.
  if (isSatellitePruningEnabled()) {
    constexpr uint MAX_SATELLITE_ROUNDS = 4;
    const NodeId target = CraftingGraph::itemNode(output);
    std::vector<uint8_t> drop(nRecipe, 0);
    for (uint round = 0; round < MAX_SATELLITE_ROUNDS; round++) {
      std::fill(drop.begin(), drop.end(), 0);
      if (!computeSatellitePruning(graph, target, itemSeen, recipeSeen, inventory, drop))
        break;
      for (uint recipe = 0; recipe < nRecipe; recipe++)
        disabled[recipe] |= drop[recipe];
      walk();
    }
  }

  // Start filling the remapping between source graph and subgraph.
  // Use UINT32_MAX for empty entries.
  std::vector<NodeId> itemMap(nItem, UINT32_MAX);
  // Note that we can compute the amount of real items in subgraph alongside the way.
  uint subreal = 0;
  for (NodeId item = 0; item < nItem; item++) {
    if (itemSeen[item]) {
      itemMap[item] = result.itemOrigin.size();
      result.itemOrigin.push_back(item);
      if (item < graph.nReal)
        subreal++;
    }
  }
  std::vector<uint32_t> recipeMap(nRecipe, UINT32_MAX);
  for (uint recipe = 0; recipe < nRecipe; recipe++) {
    if (recipeSeen[recipe]) {
      recipeMap[recipe] = result.recipeOrigin.size();
      result.recipeOrigin.push_back(recipe);
    }
  }

  // Start filling the map itself.
  const uint subItems = result.itemOrigin.size();
  const uint subRecipes = result.recipeOrigin.size();

  BaseCraftingGraph &sub = result.graph;
  sub.nReal = subreal;
  sub.nItem = subItems;
  sub.nRecipe = subRecipes;

  // Fill item -> recipe.
  sub.i2r.offsets.assign(subItems + 1, 0);
  for (uint i = 0; i < subItems; ++i) {
    uint count = 0;
    for (NodeId recipeNode : graph.i2r.targetsOf(result.itemOrigin[i])) {
      if (recipeMap[recipeNode - nItem] != UINT32_MAX)
        ++count;
    }
    sub.i2r.offsets[i + 1] = sub.i2r.offsets[i] + count;
  }
  sub.i2r.targets.resize(sub.i2r.offsets.back());
  sub.i2r.weights.resize(sub.i2r.offsets.back());
  for (uint i = 0; i < subItems; i++) {
    NodeId cursor = sub.i2r.offsets[i];
    const auto targets = graph.i2r.targetsOf(result.itemOrigin[i]);
    const auto weights = graph.i2r.weightsOf(result.itemOrigin[i]);
    for (size_t k = 0; k < targets.size(); ++k) {
      const uint32_t recipe = recipeMap[targets[k] - nItem];
      if (recipe == UINT32_MAX)
        continue;
      sub.i2r.targets[cursor] = sub.nItem + recipe;
      sub.i2r.weights[cursor++] = weights[k];
    }
  }

  sub.output.resize(subRecipes);
  sub.outputAmt.resize(subRecipes);
  for (uint i = 0; i < subRecipes; i++) {
    const uint recipe = result.recipeOrigin[i];
    sub.output[i] = itemMap[graph.output[recipe]];
    sub.outputAmt[i] = graph.outputAmt[recipe];
  }

  // Fill recipe -> item.
  sub.r2i.offsets.assign(subRecipes + 1, 0);
  for (uint i = 0; i < subRecipes; ++i)
    sub.r2i.offsets[i + 1] = sub.r2i.offsets[i] +
        graph.r2i.targetsOf(result.recipeOrigin[i]).size();
  sub.r2i.targets.resize(sub.r2i.offsets.back());
  sub.r2i.weights.resize(sub.r2i.offsets.back());
  for (uint i = 0; i < subRecipes; ++i) {
    int cursor = sub.r2i.offsets[i];
    const uint recipe = result.recipeOrigin[i];
    const auto targets = graph.r2i.targetsOf(recipe);
    const auto weights = graph.r2i.weightsOf(recipe);
    for (size_t j = 0; j < targets.size(); j++) {
      sub.r2i.targets[cursor] = itemMap[targets[j]];
      sub.r2i.weights[cursor++] = weights[j];
    }
  }

  return result;
}

NodeId Subgraph::translate(NodeId sourceNode) const noexcept {
  const auto begin = itemOrigin.begin();
  const auto end = itemOrigin.end();
  const auto it = std::lower_bound(begin, end, sourceNode);
  if (it == end || *it != sourceNode)
    return UINT32_MAX;
  return it - begin;
}

std::vector<Amount> Subgraph::translateInv(std::span<const Amount> invSrc) const noexcept {
  std::vector<Amount> inventory(graph.nItem, 0);
  for (uint32_t i = 0; i < graph.nItem; i++) {
    const NodeId source = itemOrigin[i];
    if (source < invSrc.size())
      inventory[i] = invSrc[source];
  }
  return inventory;
}

} // namespace aw
