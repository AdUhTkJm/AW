#include "aw/CraftingGraph.h"

#include <algorithm>
#include <array>

namespace aw {
CraftingGraph graph;
// Last parse error, or nullptr when the last registerCraftingGraph succeeded.
// The Java side reads this to turn a malformed blob into an exception.
const char *error;
// Tag-edge dominance pruning is on by default; see setTagPruningEnabled.
bool tagPruning = true;

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

// ---------------------------------------------------------------------------
// Tag-member dominance pruning
// ---------------------------------------------------------------------------
//
// A tag (pseudo-resource) T is a set of members, encoded as one synthetic
// `T <- m` recipe per member. Serving T through a member m is pointless when
// another member w is strictly cheaper, because then the tag edge `T <- m` can
// be replaced by `T <- w`. The relation used here is `requires(m, w)`: for
// every recipe r of m there is an amount-qualified input that is either w
// itself or a simple tag all of whose other producible members require w. It
// implies the per-unit cost of m exceeds that of w. See docs/pruning.typ.
//
// A tag is "simple" when every one of its recipes is a member edge: exactly one
// input, no workstation, and a real member. Everything the Java writer emits
// looks like that. A malformed-but-parseable tag is left unpruned, which is
// conservative.

using PairKey = uint64_t;

constexpr PairKey packPair(NodeId m, NodeId w) noexcept {
  return ((PairKey) m << 32) | (PairKey) w;
}

void intersectSorted(const std::vector<NodeId> &a, const std::vector<NodeId> &b,
                     std::vector<NodeId> &out) noexcept {
  out.clear();
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] < b[j]) {
      i++;
    } else if (b[j] < a[i]) {
      j++;
    } else {
      out.push_back(a[i]);
      i++;
      j++;
    }
  }
}

// Fills graph.tagEdgeDominated from the canonical graph. Never fails; on
// malformed input it simply prunes less.
void computeTagPruning() noexcept {
  graph.tagEdgeDominated.assign(graph.nRecipe, 0);

  const uint nItem = graph.nItem;
  const uint nReal = graph.nReal;
  const uint nRecipe = graph.nRecipe;
  if (nItem == 0 || nReal == 0 || nRecipe == 0)
    return;

  // ---- Simple tags and their members ------------------------------------
  std::vector<uint8_t> simpleTag(nItem, 0);
  std::vector<std::vector<NodeId>> members(nItem);
  for (NodeId t = nReal; t < nItem; t++) {
    const auto recipes = graph.i2r.targetsOf(t);
    if (recipes.empty())
      continue;

    std::vector<NodeId> ms;
    ms.reserve(recipes.size());
    bool simple = true;
    for (NodeId recipeNode : recipes) {
      const uint r = recipeNode - nItem;
      const auto inputs = graph.r2i.targetsOf(r);
      if (!graph.workstations.targetsOf(r).empty() || inputs.size() != 1 ||
          inputs[0] >= nReal) {
        simple = false;
        break;
      }
      ms.push_back(inputs[0]);
    }
    if (!simple)
      continue;

    std::sort(ms.begin(), ms.end());
    ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
    simpleTag[t] = 1;
    members[t] = std::move(ms);
  }

  std::vector<uint8_t> producible(nItem, 0);
  for (NodeId m = 0; m < nItem; m++)
    if (!graph.i2r.targetsOf(m).empty())
      producible[m] = 1;

  // member -> tags containing it, ascending.
  std::vector<uint> itemTagOffsets(nReal, 0);
  for (NodeId t = nReal; t < nItem; t++)
    if (simpleTag[t])
      for (NodeId m : members[t])
        itemTagOffsets[m]++;
  prefixSum(itemTagOffsets);
  std::vector<NodeId> itemTagTargets(itemTagOffsets.back());
  {
    std::vector<uint> cursor(itemTagOffsets.begin(), itemTagOffsets.end() - 1);
    for (NodeId t = nReal; t < nItem; t++)
      if (simpleTag[t])
        for (NodeId m : members[t])
          itemTagTargets[cursor[m]++] = t;
  }

  // tag -> real items that consume it in a real recipe. Duplicates are kept;
  // the worklist dedups them.
  std::vector<uint> tagConsumerOffsets(nItem, 0);
  for (uint r = 0; r < nRecipe; r++) {
    if (graph.output[r] >= nReal)
      continue;
    for (NodeId j : graph.r2i.targetsOf(r))
      if (j >= nReal && j < nItem && simpleTag[j])
        tagConsumerOffsets[j]++;
  }
  prefixSum(tagConsumerOffsets);
  std::vector<NodeId> tagConsumerTargets(tagConsumerOffsets.back());
  {
    std::vector<uint> cursor(tagConsumerOffsets.begin(), tagConsumerOffsets.end() - 1);
    for (uint r = 0; r < nRecipe; r++) {
      if (graph.output[r] >= nReal)
        continue;
      const NodeId m = graph.output[r];
      for (NodeId j : graph.r2i.targetsOf(r))
        if (j >= nReal && j < nItem && simpleTag[j])
          tagConsumerTargets[cursor[j]++] = m;
    }
  }

  // ---- The universe of candidate pairs ----------------------------------
  // (a) witnesses for gating: w in the intersection of the amount-qualified
  //     input sources over every recipe of m.
  // (b) support pairs (z, w) for members of a common tag, so that the "all
  //     other members require w" rule has something to count.
  std::vector<PairKey> pairs;
  {
    std::vector<NodeId> acc, next, scratch;
    for (NodeId m = 0; m < nReal; m++) {
      const auto recipes = graph.i2r.targetsOf(m);
      if (recipes.empty())
        continue;

      bool first = true;
      acc.clear();
      for (NodeId recipeNode : recipes) {
        const uint r = recipeNode - nItem;
        const Amount out = graph.outputAmt[r];
        scratch.clear();
        const auto inputs = graph.r2i.targetsOf(r);
        const auto weights = graph.r2i.weightsOf(r);
        for (size_t k = 0; k < inputs.size(); k++) {
          if (weights[k] < out)
            continue;
          const NodeId j = inputs[k];
          if (j < nReal) {
            scratch.push_back(j);
          } else if (j < nItem && simpleTag[j]) {
            scratch.insert(scratch.end(), members[j].begin(), members[j].end());
          }
        }
        std::sort(scratch.begin(), scratch.end());
        scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
        if (first) {
          acc.swap(scratch);
          first = false;
        } else {
          intersectSorted(acc, scratch, next);
          acc.swap(next);
        }
        if (acc.empty())
          break;
      }
      for (NodeId w : acc)
        if (w != m)
          pairs.push_back(packPair(m, w));
    }
  }
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const std::vector<NodeId> &ms = members[t];
    for (NodeId z : ms) {
      if (!producible[z])
        continue;
      for (NodeId w : ms)
        if (z != w)
          pairs.push_back(packPair(z, w));
    }
  }

  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

  const uint32_t universe = (uint32_t) pairs.size();
  if (universe == 0)
    return;

  // Group the universe by witness. Sorting by (m, w) means a single scan fills
  // each witness row in ascending m, so idOf can binary search the row.
  std::vector<uint> byWOffsets(nItem, 0);
  for (PairKey key : pairs)
    byWOffsets[(uint32_t) (key & 0xFFFFFFFFu)]++;
  prefixSum(byWOffsets);
  std::vector<NodeId> byWTargets(universe);
  std::vector<NodeId> pairW(universe);
  {
    std::vector<uint> cursor(byWOffsets.begin(), byWOffsets.end() - 1);
    for (PairKey key : pairs) {
      const NodeId w = (NodeId) (key & 0xFFFFFFFFu);
      const NodeId m = (NodeId) (key >> 32);
      const uint slot = cursor[w]++;
      byWTargets[slot] = m;
      pairW[slot] = w;
    }
  }
  pairs.clear();
  pairs.shrink_to_fit();

  auto idOf = [&](NodeId m, NodeId w) -> uint32_t {
    const uint lo = byWOffsets[w], hi = byWOffsets[w + 1];
    const auto begin = byWTargets.begin() + lo;
    const auto end = byWTargets.begin() + hi;
    const auto it = std::lower_bound(begin, end, m);
    if (it == end || *it != m)
      return UINT32_MAX;
    return (uint32_t) (it - byWTargets.begin());
  };

  // ---- Counters for the tag rule ----------------------------------------
  // cnt[t][i] counts members z != M[i] that currently require M[i]. The tag may
  // gate through M[i] exactly when the counter reaches |M| - 1; a member with
  // no recipe contributes nothing, so a leaf member always blocks the gate.
  std::vector<std::vector<uint32_t>> cnt(nItem);
  std::vector<uint32_t> threshold(nItem, 0);
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const std::vector<NodeId> &ms = members[t];
    cnt[t].assign(ms.size(), 0);
    threshold[t] = (uint32_t) ms.size() - 1;
    for (size_t idx = 0; idx < ms.size(); idx++) {
      uint32_t c = 0;
      for (size_t k = 0; k < ms.size(); k++)
        if (k != idx && producible[ms[k]])
          c++;
      cnt[t][idx] = c;
    }
  }

  auto valid = [&](NodeId m, NodeId w) -> bool {
    for (NodeId recipeNode : graph.i2r.targetsOf(m)) {
      const uint r = recipeNode - nItem;
      const Amount out = graph.outputAmt[r];
      const auto inputs = graph.r2i.targetsOf(r);
      const auto weights = graph.r2i.weightsOf(r);
      bool ok = false;
      for (size_t k = 0; k < inputs.size(); k++) {
        if (weights[k] < out)
          continue;
        const NodeId j = inputs[k];
        if (j == w) {
          ok = true;
          break;
        }
        if (j >= nReal && j < nItem && simpleTag[j]) {
          const std::vector<NodeId> &ms = members[j];
          const auto it = std::lower_bound(ms.begin(), ms.end(), w);
          if (it != ms.end() && *it == w) {
            const size_t idx = (size_t) (it - ms.begin());
            if (cnt[j][idx] == threshold[j]) {
              ok = true;
              break;
            }
          }
        }
      }
      if (!ok)
        return false;
    }
    return true;
  };

  // ---- Greatest fixpoint ------------------------------------------------
  // Every pair starts alive and is killed once when it loses its justification.
  // A kill can only invalidate pairs that gate through a shared tag, so those
  // consumers are re-queued; the result is the greatest fixpoint.
  std::vector<uint8_t> alive(universe, 1);
  std::vector<uint32_t> queue(universe);
  std::vector<uint8_t> queued(universe, 0);
  size_t head = 0, tail = 0, pending = 0;
  auto push = [&](uint32_t id) noexcept {
    if (queued[id])
      return;
    queued[id] = 1;
    queue[tail] = id;
    tail++;
    if (tail == universe)
      tail = 0;
    pending++;
  };
  for (uint32_t id = 0; id < universe; id++)
    push(id);

  while (pending != 0) {
    const uint32_t id = queue[head];
    head++;
    if (head == universe)
      head = 0;
    pending--;
    queued[id] = 0;
    if (!alive[id])
      continue;

    const NodeId m = byWTargets[id];
    const NodeId w = pairW[id];
    if (valid(m, w))
      continue;

    // Kill (m, w). Every tag containing both loses one supporting member.
    // m and w are always real here: the universe is built from real items.
    alive[id] = 0;
    const auto ta = std::span(itemTagTargets.data() + itemTagOffsets[m],
                              itemTagOffsets[m + 1] - itemTagOffsets[m]);
    const auto tb = std::span(itemTagTargets.data() + itemTagOffsets[w],
                              itemTagOffsets[w + 1] - itemTagOffsets[w]);
    size_t i = 0, j = 0;
    while (i < ta.size() && j < tb.size()) {
      if (ta[i] < tb[j]) {
        i++;
      } else if (tb[j] < ta[i]) {
        j++;
      } else {
        const NodeId tag = ta[i];
        const std::vector<NodeId> &ms = members[tag];
        const auto it = std::lower_bound(ms.begin(), ms.end(), w);
        const size_t idx = (size_t) (it - ms.begin());
        const uint32_t c = --cnt[tag][idx];
        // cnt only ever decreases, so the drop from a full gate (threshold) to
        // a broken one happens at most once per (tag, witness).
        if (c + 1 == threshold[tag]) {
          for (uint e = tagConsumerOffsets[tag]; e < tagConsumerOffsets[tag + 1]; e++) {
            const uint32_t q = idOf(tagConsumerTargets[e], w);
            if (q != UINT32_MAX)
              push(q);
          }
        }
        i++;
        j++;
      }
    }
  }

  // ---- Keep one representative per sink SCC -----------------------------
  // On the alive edges inside a tag, every member reaches some sink SCC, and a
  // sink representative is substitutable for everything that reaches it. This
  // also keeps at least one member of every tag, including mutual-requirement
  // cycles, so no tag becomes unsatisfiable.
  std::vector<int32_t> comp, disc, low;
  std::vector<uint8_t> onStack;
  std::vector<uint32_t> tstack, callNode, callEdge;
  std::vector<uint32_t> adjOffsets, adjTargets, cursor;
  for (NodeId t = nReal; t < nItem; t++) {
    if (!simpleTag[t])
      continue;
    const std::vector<NodeId> &ms = members[t];
    const size_t k = ms.size();
    if (k < 2)
      continue;

    adjOffsets.assign(k + 1, 0);
    for (size_t a = 0; a < k; a++) {
      uint32_t degree = 0;
      for (size_t b = 0; b < k; b++) {
        if (a == b)
          continue;
        const uint32_t id = idOf(ms[a], ms[b]);
        if (id != UINT32_MAX && alive[id])
          degree++;
      }
      adjOffsets[a + 1] = degree;
    }
    for (size_t a = 0; a + 1 <= k; a++)
      adjOffsets[a + 1] += adjOffsets[a];
    adjTargets.assign(adjOffsets[k], 0);
    cursor.assign(adjOffsets.begin(), adjOffsets.end() - 1);
    for (size_t a = 0; a < k; a++)
      for (size_t b = 0; b < k; b++) {
        if (a == b)
          continue;
        const uint32_t id = idOf(ms[a], ms[b]);
        if (id != UINT32_MAX && alive[id])
          adjTargets[cursor[a]++] = (uint32_t) b;
      }

    // Iterative Tarjan, so a deep tag cannot overflow the stack.
    comp.assign(k, -1);
    disc.assign(k, -1);
    low.assign(k, 0);
    onStack.assign(k, 0);
    tstack.clear();
    callNode.clear();
    callEdge.clear();
    int32_t timer = 0;
    uint32_t nComp = 0;
    for (uint32_t s = 0; s < k; s++) {
      if (disc[s] != -1)
        continue;
      disc[s] = low[s] = timer++;
      tstack.push_back(s);
      onStack[s] = 1;
      callNode.push_back(s);
      callEdge.push_back(adjOffsets[s]);
      while (!callNode.empty()) {
        const uint32_t v = callNode.back();
        uint32_t &edge = callEdge.back();
        if (edge < adjOffsets[v + 1]) {
          const uint32_t u = adjTargets[edge++];
          if (disc[u] == -1) {
            disc[u] = low[u] = timer++;
            tstack.push_back(u);
            onStack[u] = 1;
            callNode.push_back(u);
            callEdge.push_back(adjOffsets[u]);
          } else if (onStack[u] && disc[u] < low[v]) {
            low[v] = disc[u];
          }
        } else {
          if (low[v] == disc[v]) {
            while (true) {
              const uint32_t u = tstack.back();
              tstack.pop_back();
              onStack[u] = 0;
              comp[u] = (int32_t) nComp;
              if (u == v)
                break;
            }
            nComp++;
          }
          callNode.pop_back();
          callEdge.pop_back();
          if (!callNode.empty()) {
            const uint32_t parent = callNode.back();
            if (low[v] < low[parent])
              low[parent] = low[v];
          }
        }
      }
    }

    std::vector<uint8_t> isSink(nComp, 1);
    for (uint32_t a = 0; a < k; a++)
      for (uint32_t e = adjOffsets[a]; e < adjOffsets[a + 1]; e++) {
        const uint32_t b = adjTargets[e];
        if (comp[a] != comp[b])
          isSink[comp[a]] = 0;
      }
    std::vector<uint32_t> rep(nComp, UINT32_MAX);
    for (uint32_t a = 0; a < k; a++) {
      const uint32_t c = (uint32_t) comp[a];
      if (isSink[c] && rep[c] == UINT32_MAX)
        rep[c] = a;
    }

    for (uint32_t a = 0; a < k; a++) {
      if (rep[comp[a]] == a)
        continue;
      const NodeId m = ms[a];
      for (NodeId recipeNode : graph.i2r.targetsOf(t)) {
        const uint r = recipeNode - nItem;
        const auto inputs = graph.r2i.targetsOf(r);
        if (inputs.size() == 1 && inputs[0] == m)
          graph.tagEdgeDominated[r] = 1;
      }
    }
  }
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
  computeTagPruning();
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

void setTagPruningEnabled(bool enabled) noexcept {
  tagPruning = enabled;
}

bool isTagPruningEnabled() noexcept {
  return tagPruning;
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

  std::vector<uint8_t> itemSeen(nItem, 0);
  std::vector<uint8_t> recipeSeen(nRecipe, 0);

  // Ordinary BFS.
  std::vector<NodeId> queue;
  const NodeId start = CraftingGraph::itemNode(output);
  itemSeen[start] = 1;
  queue.push_back(start);

  for (size_t q = 0; q < queue.size(); q++) {
    const NodeId item = queue[q];
    const bool real = graph.isRealItem(item);

    for (NodeId recipeNode : graph.i2r.targetsOf(item)) {
      const uint recipe = recipeNode - nItem;
      if (recipeSeen[recipe])
        continue;

      // A dominated tag edge is dropped unless the player actually holds the
      // member, in which case the free stock can still be spent on the tag.
      if (tagPruning && recipe < graph.tagEdgeDominated.size() &&
          graph.tagEdgeDominated[recipe]) {
        const auto memberInputs = graph.r2i.targetsOf(recipe);
        const Amount held = !memberInputs.empty() && memberInputs[0] < inventory.size()
                                ? inventory[memberInputs[0]]
                                : 0;
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
