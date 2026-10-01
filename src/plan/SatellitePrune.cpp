// Satellite elimination: a pruning pass that runs on the reachable subgraph,
// where the target and the inventory are known.
//
// docs/algorithm.typ ("孤岛消除") has the full statement. In one line: pick a
// set of items I, let G be every recipe that produces or consumes an item of I,
// and suppose I holds neither the target nor any stock and every recipe of G
// outputs an item of I or a single escape A. If G cannot produce a net surplus
// of A, then no optimal plan executes a recipe of G.
//
// The old, weaker statement is the special case where I is the item set of a
// component of subgraph - A (so G cannot even *consume* anything outside I).
// The pass finds that case with an undirected articulation search, and the
// wider "input leaking" islands with an escape enumeration: for a fixed escape
// A, I is the largest set of items that cannot reach the target or a stocked
// item without passing through A. That set is closed under "consumes ->
// produces", so the single output condition holds by construction. The copper
// oxidation cycle is the motivating case; it shares mekanism:oxygen with the
// ore processing, so it is not an undirected component.
//
// "Cannot reach ... without passing through A" is domination by A in the
// dependency graph, so the escape enumeration runs off one dominator tree
// (Lengauer-Tarjan) instead of one graph walk per escape.
//
// The condition "G cannot produce a net surplus of A" is the LP
//
//   max (A z)_A   s.t.  (A z)_j >= 0 for every item j of I,  sum z <= 1,  z >= 0
//
// whose optimum is 0 exactly when the island is dead. Rather than solve that
// and then have to trust its dual, the pass solves the Farkas certificate of
// "the optimum is 0" directly:
//
//   find y >= 0 with  sum_j y_j A_{j,r} + A_{A,r} <= 0  for every recipe r of G
//
// Such a y exists exactly when that optimum is 0, and it is a proof: for any
// z >= 0 with (A z)_j >= 0 on I,
//
//   D (A z)_A = sum_j Y_j (A z)_j + D (A z)_A - sum_j Y_j (A z)_j
//             = sum_r z_r (sum_j Y_j A_{j,r} + D A_{A,r}) - sum_j Y_j (A z)_j <= 0,
//
// because the first term is <= 0 by the certificate, z >= 0, and the second is
// >= 0 because Y >= 0 and (A z)_j >= 0 on I. The LP only proposes y; the pass
// rounds it to integers and re-checks the inequalities exactly, so an island is
// dropped only on an exactly verified certificate and never on a float.
//
// This is deliberately the only other file next to CpSatSolver.cpp that
// includes OR-Tools, so it is exempt from -fno-exceptions -fno-rtti; the
// exported entry point is noexcept and swallows every exception itself.

#ifdef IN_VSCODE
#define OR_PROTO_DLL // To make VSCode work with #include <ortools/...>
#endif

#include "Prune.h"

#include <chrono>
#include <memory>
#include <span>

#include <ortools/linear_solver/linear_solver.h>

#include "aw/plan/CraftingGraph.h"
#include "aw/utils/Int128.h"
#include "aw/plan/Options.h"

namespace aw {
namespace {

using uint = uint32_t;
using operations_research::MPConstraint;
using operations_research::MPSolver;
using operations_research::MPVariable;

// Upper bound on a single certificate value. A certificate is not scale free
// (the A coefficient is fixed at 1), so the LP needs a box; anything a real
// recipe graph can produce for one unit sits far below this.
constexpr double CERTIFICATE_ITEM_CAP = 1e9;

// Denominators tried when rounding the LP point to the integer certificate
// (Y, D). The true certificate is rational with a small denominator on systems
// this small; the list mixes powers of two with lcm(1..k) so both shapes are
// covered. A candidate that does not verify is simply skipped.
constexpr int64_t SCALES[] = {
    1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 30, 32, 48, 60, 64, 96, 120,
    128, 192, 240, 256, 384, 420, 512, 768, 840, 1024, 1536, 2048, 2520, 3072,
    4096, 8192, 16384, 27720, 32768, 65536, 131072, 360360, 1048576,
};

// Keeps the scaled certificate inside int64 territory: |Y| * |amount| has to
// stay well below 2^63 when it is checked with 128-bit arithmetic.
constexpr double CERTIFICATE_VALUE_LIMIT = 1e15;

bool verbose = false;

[[nodiscard]]
Amount held(std::span<const Amount> inventory, ItemId item) noexcept {
  return item < inventory.size() ? inventory[item] : 0;
}

// Coefficient of `item` in the column of `recipe`: +outputAmt when the recipe
// produces it, -amt for every input, summed when the recipe eats its own
// output. The single place the balance matrix is read.
[[nodiscard]]
Amount columnCoefficient(const CraftingGraph& graph, uint recipe, ItemId item) noexcept {
  Amount coeff = graph.output[recipe] == item ? graph.outputAmt[recipe] : 0;
  const auto inputs = graph.inputsOf(recipe);
  const auto weights = graph.inputAmountsOf(recipe);
  for (size_t k = 0; k < inputs.size(); k++)
    if (inputs[k] == item)
      coeff -= weights[k];
  return coeff;
}

// ---------------------------------------------------------------------------
// Undirected adjacency of the subgraph
// ---------------------------------------------------------------------------
// Nodes are the item id for an item and nItem + recipe for a recipe. This
// combined numbering is local to this file. Only the nodes the reachability
// walk kept take part. Parallel edges are folded away, so the articulation
// search below may skip the parent *vertex*.

struct Undirected {
  aw::vector<uint> offsets;  // V + 1
  aw::vector<uint> targets;

  [[nodiscard]]
  std::span<const uint> neighbours(uint v) const noexcept {
    return {targets.data() + offsets[v], targets.data() + offsets[v + 1]};
  }
};

void buildUndirected(const CraftingGraph& graph, std::span<const uint8_t> recipeSeen,
                     Undirected& out) noexcept {
  const uint nItem = graph.nItem;
  const uint nNodes = nItem + graph.nRecipe;

  aw::vector<uint> counts(nNodes, 0);
  for (uint r = 0; r < graph.nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    const uint rn = nItem + r;
    counts[graph.output[r]]++;
    counts[rn]++;
    for (ItemId input : graph.inputsOf(r)) {
      counts[input]++;
      counts[rn]++;
    }
  }

  out.offsets.assign(nNodes + 1, 0);
  for (uint v = 0; v < nNodes; v++)
    out.offsets[v + 1] = out.offsets[v] + counts[v];
  out.targets.assign(out.offsets[nNodes], 0);

  aw::vector<uint> cursor(out.offsets.begin(), out.offsets.end() - 1);
  auto link = [&](uint a, uint b) noexcept {
    out.targets[cursor[a]++] = b;
  };
  for (uint r = 0; r < graph.nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    const uint rn = nItem + r;
    link(graph.output[r], rn);
    link(rn, graph.output[r]);
    for (ItemId input : graph.inputsOf(r)) {
      link(input, rn);
      link(rn, input);
    }
  }

  // Fold duplicates, then re-compact, because the rows are now shorter than the
  // offsets claim.
  uint write = 0;
  for (uint v = 0; v < nNodes; v++) {
    uint* begin = out.targets.data() + out.offsets[v];
    uint* end = out.targets.data() + out.offsets[v + 1];
    std::sort(begin, end);
    const uint kept = (uint) (std::unique(begin, end) - begin);
    for (uint k = 0; k < kept; k++)
      out.targets[write + k] = begin[k];
    out.offsets[v] = write;
    write += kept;
  }
  out.offsets[nNodes] = write;
  out.targets.resize(write);
}

// ---------------------------------------------------------------------------
// One DFS tree, rooted at the target
// ---------------------------------------------------------------------------
// Rooting the search at the target is what makes enumeration cheap: every
// component of subgraph - A that a child subtree describes is automatically
// target-free, so no component has to be searched for the target and the "rest"
// of the graph never has to be walked.
struct DfsTree {
  aw::vector<int32_t> disc;  // -1 when unseen
  aw::vector<int32_t> low;
  aw::vector<uint> parent;
};

void buildDfsTree(const Undirected& und, uint root, uint nNodes, DfsTree& out) noexcept {
  out.disc.assign(nNodes, -1);
  out.low.assign(nNodes, 0);
  out.parent.assign(nNodes, UINT32_MAX);

  aw::vector<uint> nodeStack;
  aw::vector<uint> edgeStack;
  int32_t timer = 0;
  out.disc[root] = out.low[root] = timer++;
  nodeStack.push_back(root);
  edgeStack.push_back(und.offsets[root]);
  while (!nodeStack.empty()) {
    const uint v = nodeStack.back();
    uint& edge = edgeStack.back();
    if (edge < und.offsets[v + 1]) {
      const uint u = und.targets[edge++];
      if (u == out.parent[v])
        continue;
      if (out.disc[u] == -1) {
        out.parent[u] = v;
        out.disc[u] = out.low[u] = timer++;
        nodeStack.push_back(u);
        edgeStack.push_back(und.offsets[u]);
      } else if (out.disc[u] < out.low[v]) {
        out.low[v] = out.disc[u];
      }
    } else {
      nodeStack.pop_back();
      edgeStack.pop_back();
      if (!nodeStack.empty()) {
        const uint p = nodeStack.back();
        if (out.low[v] < out.low[p])
          out.low[p] = out.low[v];
      }
    }
  }
}

// Collects the DFS subtree rooted at `v` into `nodes`, reusing it as the work
// stack. Only tree children are followed, so the walk costs the subtree size.
void collectSubtree(const Undirected& und, const DfsTree& tree, uint v,
                    aw::vector<uint> &nodes) noexcept {
  nodes.clear();
  nodes.push_back(v);
  for (size_t q = 0; q < nodes.size(); q++) {
    const uint x = nodes[q];
    for (uint y : und.neighbours(x))
      if (y != tree.parent[x] && tree.parent[y] == x)
        nodes.push_back(y);
  }
}

// ---------------------------------------------------------------------------
// The item-level "consume -> produce" graph
// ---------------------------------------------------------------------------
// The generalized lemma (docs/algorithm.typ, "孤岛消除") does not need the
// island to be separated on its inputs. It only needs every recipe that
// *touches* an island item to be part of the island (so the item's balance row
// is internal), and no recipe of the island to net-produce an item outside the
// island plus one escape A (so subtracting the island cannot starve anything
// else).
//
// Both conditions are about consumption, not production: an item j forces in
// every recipe that eats j, and that recipe forces in the item it makes. The
// graph below has exactly that edge, j -> output(r) for every recipe r that
// consumes j. Following it forward from an item therefore collects what that
// item drags into the island. The escape enumeration does not need the whole
// graph; it only needs this SCC decomposition, to try the escapes that sit
// inside a cycle first.

struct ItemGraph {
  aw::vector<uint> offsets;  // nItem + 1
  aw::vector<uint> targets;
};

void buildItemGraph(const CraftingGraph& graph, std::span<const uint8_t> recipeSeen,
                    ItemGraph& out) noexcept {
  const uint nItem = graph.nItem;
  aw::vector<uint> counts(nItem, 0);
  for (uint r = 0; r < graph.nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    const ItemId produced = graph.output[r];
    for (ItemId input : graph.inputsOf(r))
      if (input != produced)
        counts[input]++;
  }

  out.offsets.assign(nItem + 1, 0);
  for (uint v = 0; v < nItem; v++)
    out.offsets[v + 1] = out.offsets[v] + counts[v];
  out.targets.assign(out.offsets[nItem], 0);

  aw::vector<uint> cursor(out.offsets.begin(), out.offsets.end() - 1);
  for (uint r = 0; r < graph.nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    const ItemId produced = graph.output[r];
    for (ItemId input : graph.inputsOf(r))
      if (input != produced)
        out.targets[cursor[input]++] = produced;
  }
}

// Iterative Tarjan over the item graph. `comp[v]` is the SCC id of v, or
// UINT32_MAX when the item is not in the subgraph. The number of components is
// returned.
uint32_t computeItemSccs(const ItemGraph& graph, std::span<const uint8_t> itemSeen,
                         uint nItem, aw::vector<uint32_t> &comp) noexcept {
  comp.assign(nItem, UINT32_MAX);
  aw::vector<uint32_t> index(nItem, UINT32_MAX);
  aw::vector<uint32_t> low(nItem, 0);
  aw::vector<uint8_t> onStack(nItem, 0);
  aw::vector<uint> stack;
  aw::vector<uint> nodeStack;
  aw::vector<uint> edgeStack;
  uint32_t timer = 0;
  uint32_t nComp = 0;

  for (uint root = 0; root < nItem; root++) {
    if (!itemSeen[root] || index[root] != UINT32_MAX)
      continue;
    index[root] = low[root] = timer++;
    onStack[root] = 1;
    stack.push_back(root);
    nodeStack.push_back(root);
    edgeStack.push_back(graph.offsets[root]);
    while (!nodeStack.empty()) {
      const uint v = nodeStack.back();
      uint& edge = edgeStack.back();
      if (edge < graph.offsets[v + 1]) {
        const uint w = graph.targets[edge++];
        if (index[w] == UINT32_MAX) {
          index[w] = low[w] = timer++;
          onStack[w] = 1;
          stack.push_back(w);
          nodeStack.push_back(w);
          edgeStack.push_back(graph.offsets[w]);
        } else if (onStack[w] && index[w] < low[v]) {
          low[v] = index[w];
        }
      } else {
        nodeStack.pop_back();
        edgeStack.pop_back();
        if (low[v] == index[v]) {
          while (true) {
            const uint w = stack.back();
            stack.pop_back();
            onStack[w] = 0;
            comp[w] = nComp;
            if (w == v)
              break;
          }
          nComp++;
        }
        if (!nodeStack.empty()) {
          const uint p = nodeStack.back();
          if (low[v] < low[p])
            low[p] = low[v];
        }
      }
    }
  }
  return nComp;
}

// ---------------------------------------------------------------------------
// Dominators of the dependency graph
// ---------------------------------------------------------------------------
// The island of an escape A is the largest set of items that cannot reach the
// target or a stocked item without passing through A, plus the items no such
// walk reaches at all. The first part is exactly the set A dominates in the
// dependency graph, so every island is a subtree of one dominator tree: an
// island is "A dominates I", and the tree gives every escape's I at once.
//
// Reachability is an OR over the producers of an item, so the dependency graph
// is an ordinary digraph and Lengauer-Tarjan applies. The AND-OR obstacle that
// blocks the *cost* relation ("costs" in SubstitutionPrune.cpp) does not apply
// to reachability. Building the tree once replaces one graph walk per candidate,
// and the enumeration used to spend its whole budget on those walks.
//
// A virtual root (node `nItem`) has an edge to every required item, so a
// required item is reachable in one step and no other item can dominate it.
struct DepGraph {
  std::span<const uint> offsets;  // nItem + 1
  std::span<const uint> targets;  // offsets[nItem]
  std::span<const uint> required;
  uint nItem;
};

// The dominator tree of a DepGraph, in the shape the escape enumeration needs.
struct Dominators {
  aw::vector<uint8_t> inTree;        // nItem: reached from the virtual root
  aw::vector<uint32_t> subtreeSeen;  // nItem: subgraph items below it, itself included
  aw::vector<uint> childOffsets;     // nItem + 2; the virtual root is index nItem
  aw::vector<uint32_t> children;
};

// Lengauer-Tarjan's union-find, indexed by DFS number: `eval` returns the node
// of the path to the root with the smallest semidominator. The recursion of the
// textbook version is flattened so that a deep path cannot overflow the stack.
struct LinkEval {
  aw::vector<uint32_t> &semi;
  aw::vector<uint32_t> &label;
  aw::vector<uint32_t> &ancestor;
  aw::vector<uint32_t> &path;

  [[nodiscard]]
  uint eval(uint v) noexcept {
    if (ancestor[v] == UINT32_MAX)
      return label[v];
    compress(v);
    return label[v];
  }

  void compress(uint v) noexcept {
    path.clear();
    uint u = v;
    while (ancestor[ancestor[u]] != UINT32_MAX) {
      path.push_back(u);
      u = ancestor[u];
    }
    // The recursive definition updates the topmost frame first, then unwinds.
    for (size_t k = path.size(); k-- > 0;) {
      const uint x = path[k];
      if (semi[label[ancestor[x]]] < semi[label[x]])
        label[x] = label[ancestor[x]];
      ancestor[x] = ancestor[ancestor[x]];
    }
  }
};

// Runs the DFS numbering of the dependency graph from the virtual root.
// `dfn` is indexed by node, `vertex` by DFS number, `parent` by DFS number.
void numberDependencyGraph(const DepGraph& dep, aw::vector<uint32_t> &dfn,
                           aw::vector<uint32_t> &vertex, aw::vector<uint32_t> &parent,
                           uint &count) noexcept {
  const uint nItem = dep.nItem;
  const uint root = nItem;
  aw::vector<uint> cursor(nItem + 1, 0);
  for (uint v = 0; v < nItem; v++)
    cursor[v] = dep.offsets[v];
  aw::vector<uint> stack;
  stack.reserve(nItem + 1);

  count = 0;
  dfn[root] = count;
  vertex[count++] = root;
  stack.push_back(root);
  while (!stack.empty()) {
    const uint x = stack.back();
    const uint end = x == root ? (uint) dep.required.size() : dep.offsets[x + 1];
    if (cursor[x] == end) {
      stack.pop_back();
      continue;
    }
    const uint y = x == root ? dep.required[cursor[x]++] : dep.targets[cursor[x]++];
    if (dfn[y] != UINT32_MAX)
      continue;
    parent[count] = dfn[x];
    dfn[y] = count;
    vertex[count++] = y;
    stack.push_back(y);
  }
}

// Builds the predecessor CSR of the dependency graph, the direction
// Lengauer-Tarjan walks.
void buildPredecessors(const DepGraph& dep, aw::vector<uint> &offsets,
                       aw::vector<uint32_t> &targets) noexcept {
  const uint nItem = dep.nItem;
  const uint nNodes = nItem + 1;
  const uint root = nItem;

  offsets.assign(nNodes + 1, 0);
  for (uint x = 0; x < nItem; x++)
    for (uint e = dep.offsets[x]; e < dep.offsets[x + 1]; e++)
      offsets[dep.targets[e] + 1]++;
  for (uint r : dep.required)
    offsets[r + 1]++;
  for (uint v = 0; v < nNodes; v++)
    offsets[v + 1] += offsets[v];

  targets.resize(offsets[nNodes]);
  aw::vector<uint> cursor(offsets.begin(), offsets.end() - 1);
  for (uint x = 0; x < nItem; x++)
    for (uint e = dep.offsets[x]; e < dep.offsets[x + 1]; e++)
      targets[cursor[dep.targets[e]]++] = x;
  for (uint r : dep.required)
    targets[cursor[r]++] = root;
}

void computeDominators(const DepGraph& dep, std::span<const uint8_t> itemSeen,
                       Dominators& out) noexcept {
  const uint nItem = dep.nItem;
  const uint nNodes = nItem + 1;

  aw::vector<uint32_t> dfn(nNodes, UINT32_MAX);
  aw::vector<uint32_t> vertex(nNodes, UINT32_MAX);
  aw::vector<uint32_t> parent(nNodes, UINT32_MAX);
  uint n = 0;
  numberDependencyGraph(dep, dfn, vertex, parent, n);

  aw::vector<uint> predOffsets;
  aw::vector<uint32_t> preds;
  buildPredecessors(dep, predOffsets, preds);

  aw::vector<uint32_t> semi(n), label(n), ancestor(n, UINT32_MAX);
  aw::vector<uint32_t> idom(n, UINT32_MAX);
  aw::vector<uint32_t> bucketHead(n, UINT32_MAX);
  aw::vector<uint32_t> bucketNext(n, UINT32_MAX);
  for (uint i = 0; i < n; i++)
    semi[i] = label[i] = i;

  aw::vector<uint32_t> path;
  LinkEval evalState{semi, label, ancestor, path};
  for (uint i = n; i-- > 1;) {
    const uint w = vertex[i];
    for (uint e = predOffsets[w]; e < predOffsets[w + 1]; e++) {
      const uint p = preds[e];
      if (dfn[p] == UINT32_MAX)
        continue;
      const uint u = evalState.eval(dfn[p]);
      if (semi[u] < semi[i])
        semi[i] = semi[u];
    }

    bucketNext[i] = bucketHead[semi[i]];
    bucketHead[semi[i]] = i;

    // link(parent[i], i), then settle the subtree of parent[i].
    ancestor[i] = parent[i];
    const uint q = parent[i];
    for (uint v = bucketHead[q]; v != UINT32_MAX; v = bucketNext[v]) {
      const uint u = evalState.eval(v);
      idom[v] = semi[u] < semi[v] ? u : q;
    }
    bucketHead[q] = UINT32_MAX;
  }
  for (uint i = 1; i < n; i++)
    if (idom[i] != semi[i])
      idom[i] = idom[idom[i]];
  idom[0] = 0;

  // Children of the dominator tree, by node id.
  out.childOffsets.assign(nNodes + 1, 0);
  for (uint i = 1; i < n; i++)
    out.childOffsets[vertex[idom[i]] + 1]++;
  for (uint v = 0; v < nNodes; v++)
    out.childOffsets[v + 1] += out.childOffsets[v];
  out.children.resize(out.childOffsets[nNodes]);
  {
    aw::vector<uint> cursor(out.childOffsets.begin(), out.childOffsets.end() - 1);
    for (uint i = 1; i < n; i++) {
      const uint v = vertex[i];
      out.children[cursor[vertex[idom[i]]]++] = v;
    }
  }

  // The item count of every subtree. A child of the tree always has a larger
  // DFS number than its parent, so one sweep in reverse accumulates them all.
  out.inTree.assign(nItem, 0);
  out.subtreeSeen.assign(nItem, 0);
  for (uint i = 1; i < n; i++) {
    const uint v = vertex[i];
    out.inTree[v] = 1;
    if (itemSeen[v])
      out.subtreeSeen[v] = 1;
  }
  for (uint i = n; i-- > 1;) {
    const uint v = vertex[i];
    const uint p = vertex[idom[i]];
    if (p < nItem)
      out.subtreeSeen[p] += out.subtreeSeen[v];
  }
}

// Appends every subgraph item below `v` in the dominator tree, `v` excluded.
// Only tree edges are followed, so the walk costs the size of the subtree.
void collectDominated(const Dominators& dom, uint v, std::span<const uint8_t> itemSeen,
                      aw::vector<uint> &stack, aw::vector<uint> &out) noexcept {
  stack.clear();
  stack.push_back(v);
  while (!stack.empty()) {
    const uint x = stack.back();
    stack.pop_back();
    for (uint e = dom.childOffsets[x]; e < dom.childOffsets[x + 1]; e++) {
      const uint y = dom.children[e];
      stack.push_back(y);
      if (itemSeen[y])
        out.push_back(y);
    }
  }
}

// ---------------------------------------------------------------------------
// The certificate LP
// ---------------------------------------------------------------------------

struct Component {
  aw::vector<ItemId> items;
  aw::vector<uint> recipes;
};

// Looks for y >= 0 with sum_j y_j A_{j,r} + A_{A,r} <= 0 for every recipe r of
// G, minimising sum y so the point is as small as possible. False means "no
// certificate was found", which only ever costs pruning power.
bool solveCertificate(const CraftingGraph& graph, ItemId cutNode, const Component& comp,
                      std::span<const int32_t> itemPos, aw::vector<double> &y) noexcept {
  try {
    std::unique_ptr<MPSolver> solver(MPSolver::CreateSolver("GLOP"));
    if (!solver)
      return false;
    solver->SuppressOutput();

    aw::vector<MPVariable*> var(comp.items.size(), nullptr);
    for (size_t i = 0; i < comp.items.size(); i++) {
      var[i] = solver->MakeNumVar(0.0, CERTIFICATE_ITEM_CAP, "");
      solver->MutableObjective()->SetCoefficient(var[i], 1.0);
    }
    solver->MutableObjective()->SetMinimization();

    const double inf = solver->infinity();
    for (uint r : comp.recipes) {
      // sum_j y_j A_{j,r} <= -A_{A,r}.
      MPConstraint* row =
          solver->MakeRowConstraint(-inf, -(double) columnCoefficient(graph, r, cutNode), "");
      const int32_t outPos = itemPos[graph.output[r]];
      if (outPos >= 0)
        row->SetCoefficient(var[outPos], (double) graph.outputAmt[r]);
      const auto inputs = graph.inputsOf(r);
      const auto weights = graph.inputAmountsOf(r);
      for (size_t e = 0; e < inputs.size(); e++)
        if (itemPos[inputs[e]] >= 0)
          row->SetCoefficient(var[itemPos[inputs[e]]], -(double) weights[e]);
    }

    if (solver->Solve() != MPSolver::OPTIMAL)
      return false;
    y.assign(comp.items.size(), 0.0);
    for (size_t i = 0; i < comp.items.size(); i++) {
      const double value = var[i]->solution_value();
      if (!std::isfinite(value) || value < 0.0)
        return false;
      y[i] = value;
    }
    return true;
  } catch (...) {
    return false;
  }
}

// Checks sum_j Y_j A_{j,r} + D A_{A,r} <= 0 for every recipe of the component.
bool certificateHolds(const CraftingGraph& graph, ItemId cutNode, const Component& comp,
                      std::span<const int32_t> itemPos,
                      std::span<const int64_t> scaled, int64_t scale) noexcept {
  for (uint r : comp.recipes) {
    aw::int128 sum = (aw::int128) scale * columnCoefficient(graph, r, cutNode);
    const int32_t outPos = itemPos[graph.output[r]];
    if (outPos >= 0)
      sum += (aw::int128) scaled[outPos] * graph.outputAmt[r];
    const auto inputs = graph.inputsOf(r);
    const auto weights = graph.inputAmountsOf(r);
    for (size_t e = 0; e < inputs.size(); e++)
      if (itemPos[inputs[e]] >= 0)
        sum -= (aw::int128) scaled[itemPos[inputs[e]]] * weights[e];
    if (sum > 0)
      return false;
  }
  return true;
}

bool componentIsDead(const CraftingGraph& graph, ItemId cutNode, const Component& comp,
                     std::span<const int32_t> itemPos, aw::vector<double> &y,
                     aw::vector<int64_t> &scaled) noexcept {
  // Cheap and exact: with no recipe of G producing A, (A z)_A <= 0 for every z,
  // so the degenerate certificate Y = 0 is always available.
  bool producesCut = false;
  for (uint r : comp.recipes)
    if (columnCoefficient(graph, r, cutNode) > 0) {
      producesCut = true;
      break;
    }
  if (!producesCut)
    return true;

  if (!solveCertificate(graph, cutNode, comp, itemPos, y))
    return false;

  // The LP point is a certificate up to rounding. Round it to integers and check
  // the inequalities exactly; a candidate that fails is discarded, so neither
  // numerical noise nor a wrong LP answer can drop a component.
  double largest = 0.0;
  for (double value : y)
    largest = value > largest ? value : largest;
  if (!(largest >= 0.0) || !std::isfinite(largest))
    return false;

  for (int64_t scale : SCALES) {
    if (largest * (double) scale > CERTIFICATE_VALUE_LIMIT)
      break;
    scaled.assign(y.size(), 0);
    for (size_t i = 0; i < y.size(); i++) {
      const int64_t value = (int64_t) std::llround(y[i] * (double) scale);
      scaled[i] = value > 0 ? value : 0;
    }
    if (certificateHolds(graph, cutNode, comp, itemPos, scaled, scale))
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The generalized pass
// ---------------------------------------------------------------------------
// One round of it: the subgraph lookups plus the buffers and the counters every
// candidate reuses. `stamp` is the candidate index, so the stamps clear
// themselves as the enumeration advances.
struct DirectedRun {
  const CraftingGraph* graph = nullptr;
  std::span<const uint8_t> recipeSeen;
  std::span<const uint> consOffsets;
  std::span<const uint> consTargets;
  uint nItem = 0;
  uint32_t stamp = 0;

  aw::vector<uint32_t> itemStamp;    // nItem, marked with the island's items
  aw::vector<uint32_t> recipeStamp;  // nRecipe, marked with the island's recipes
  aw::vector<int32_t> itemPos;       // nItem, column position inside the LP
  Component island;
  aw::vector<double> y;
  aw::vector<int64_t> scaled;
  aw::vector<uint8_t>* drop = nullptr;

  int64_t evaluated = 0;
  int64_t dropped = 0;
  int64_t skippedWide = 0;
  int64_t skippedGates = 0;
  int64_t skippedEscapes = 0;
};

// Builds G, the recipes that produce or consume an island item, checks that G
// nets nothing outside the island but the escape, and certifies the island
// dead. The island items must already be stamped. Returns true when G was
// dropped.
bool certifyIsland(DirectedRun& run, uint escape) noexcept {
  const CraftingGraph& graph = *run.graph;
  const uint32_t stamp = run.stamp;
  Component& island = run.island;

  // G = every recipe that produces or consumes an island item.
  island.recipes.clear();
  for (uint j : island.items) {
    for (RecipeId r : graph.producersOf(j)) {
      if (!run.recipeSeen[r] || run.recipeStamp[r] == stamp)
        continue;
      run.recipeStamp[r] = stamp;
      island.recipes.push_back(r);
    }
    for (uint e = run.consOffsets[j]; e < run.consOffsets[j + 1]; e++) {
      const uint r = run.consTargets[e];
      if (run.recipeStamp[r] == stamp)
        continue;
      run.recipeStamp[r] = stamp;
      island.recipes.push_back(r);
    }
  }
  if (island.recipes.empty()) {
    run.skippedGates++;
    return false;
  }
  if (island.items.size() + island.recipes.size() > options.satellite.maxIslandNodes) {
    run.skippedWide++;
    return false;
  }

  // The closure guarantees this, but never drop G on a surprise: the only net
  // output of G outside I may be the escape.
  for (uint r : island.recipes) {
    const ItemId produced = graph.output[r];
    if (run.itemStamp[produced] == stamp || produced == escape)
      continue;
    if (columnCoefficient(graph, r, produced) > 0) {
      run.skippedEscapes++;
      return false;
    }
  }

  for (size_t i = 0; i < island.items.size(); i++)
    run.itemPos[island.items[i]] = (int32_t) i;
  run.evaluated++;
  const bool dead = componentIsDead(graph, escape, island, run.itemPos, run.y, run.scaled);
  for (ItemId item : island.items)
    run.itemPos[item] = -1;
  if (!dead)
    return false;
  for (uint r : island.recipes)
    (*run.drop)[r] = 1;
  run.dropped++;
  return true;
}

// The island does not have to be separated on its inputs. Pick an escape item A
// and let I be the largest set of items that cannot reach the target (or a
// stocked item) without passing through A. Every recipe touching I goes into G;
// because I is closed under "consumes -> produces", nothing in G makes an item
// outside I except A. The certificate then drops G exactly as in the undirected
// pass, but over the island's items rather than over every item of undirected
// component.
//
// The copper oxidation chain is the motivating case: copper_block is not an
// articulation point of the undirected graph, because the chain shares
// mekanism:oxygen with the ore processing, but {exposed, weathered,
// oxidized}_copper cannot reach the target without copper_block and the
// certificate settles them.
//
// The escape is enumerated over the subgraph's items. The candidate order puts
// items inside a nontrivial cycle first, because that is where the interesting
// escapes live; running out of budget only means an escape is not tried. Each
// island is read off the dominator tree, so a candidate costs its own subtree
// and never a walk of the whole subgraph.
bool runDirected(const CraftingGraph& graph, ItemId target, std::span<const uint8_t> itemSeen,
                 std::span<const uint8_t> recipeSeen, std::span<const Amount> inventory,
                 aw::vector<uint8_t> &drop,
                 std::chrono::steady_clock::time_point started) noexcept {
  const uint nItem = graph.nItem;
  if (target >= nItem || !itemSeen[target])
    return false;

  // SCCs of the item level consume->produce graph, used only to order escapes.
  ItemGraph itemEdges;
  buildItemGraph(graph, recipeSeen, itemEdges);
  aw::vector<uint32_t> comp;
  const uint32_t nComp = computeItemSccs(itemEdges, itemSeen, nItem, comp);
  aw::vector<uint8_t> cycleItem(nItem, 0);
  if (nComp > 0) {
    aw::vector<uint> sizes(nComp, 0);
    for (uint j = 0; j < nItem; j++)
      if (itemSeen[j] && comp[j] != UINT32_MAX)
        sizes[comp[j]]++;
    for (uint j = 0; j < nItem; j++)
      if (itemSeen[j] && comp[j] != UINT32_MAX && sizes[comp[j]] > 1)
        cycleItem[j] = 1;
  }

  // Dependency graph: x -> every input of a recipe that produces x. Following
  // it from the required items visits everything useful for producing them, so
  // an item reachable without A can leave the island.
  aw::vector<uint> depOffsets(nItem + 1, 0);
  for (uint r = 0; r < graph.nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    depOffsets[graph.output[r] + 1] += (uint) graph.inputsOf(r).size();
  }
  for (uint j = 0; j < nItem; j++)
    depOffsets[j + 1] += depOffsets[j];
  aw::vector<uint> depTargets(depOffsets[nItem]);
  {
    aw::vector<uint> cursor(depOffsets.begin(), depOffsets.end() - 1);
    for (uint r = 0; r < graph.nRecipe; r++) {
      if (!recipeSeen[r])
        continue;
      for (ItemId input : graph.inputsOf(r))
        depTargets[cursor[graph.output[r]]++] = input;
    }
  }

  // Recipes consuming each item, restricted to the subgraph; producers come
  // straight from i2r.
  aw::vector<uint> consOffsets(nItem + 1, 0);
  for (uint r = 0; r < graph.nRecipe; r++) {
    if (!recipeSeen[r])
      continue;
    for (ItemId input : graph.inputsOf(r))
      consOffsets[input + 1]++;
  }
  for (uint j = 0; j < nItem; j++)
    consOffsets[j + 1] += consOffsets[j];
  aw::vector<uint> consTargets(consOffsets[nItem]);
  {
    aw::vector<uint> cursor(consOffsets.begin(), consOffsets.end() - 1);
    for (uint r = 0; r < graph.nRecipe; r++) {
      if (!recipeSeen[r])
        continue;
      for (ItemId input : graph.inputsOf(r))
        consTargets[cursor[input]++] = r;
    }
  }

  // The items the plan has to supply: the target and everything already held.
  // Both must stay outside every island, so the reverse walk starts there.
  aw::vector<uint> required;
  // The target plus each distinct stocked item.
  required.reserve(nItem + 1);
  required.push_back_unchecked(target);
  for (uint j = 0; j < nItem; j++)
    if (j != target && held(inventory, j) != 0)
      required.push_back_unchecked(j);

  aw::vector<uint> candidates;
  candidates.reserve(nItem);
  for (uint j = 0; j < nItem; j++)
    if (itemSeen[j] && cycleItem[j])
      candidates.push_back_unchecked(j);
  if (!cycleItem[target])
    candidates.push_back_unchecked(target);
  for (uint j = 0; j < nItem; j++)
    if (itemSeen[j] && !cycleItem[j] && j != target)
      candidates.push_back_unchecked(j);

  // Every escape's island is a subtree of this tree, so it is built once here
  // instead of once per candidate.
  const DepGraph depGraph{depOffsets, depTargets, required, nItem};
  Dominators dom;
  computeDominators(depGraph, itemSeen, dom);

  // The items of the subgraph no required item can reach at all. Usually empty,
  // because the subgraph is built from what the target needs. They lie in every
  // island except their own escape's, so an escape outside the dominator tree
  // still has this island: that is the dead branch the lemma also covers.
  aw::vector<uint> unreached;
  unreached.reserve(nItem);
  for (uint j = 0; j < nItem; j++)
    if (itemSeen[j] && !dom.inTree[j])
      unreached.push_back(j);

  DirectedRun run;
  run.graph = &graph;
  run.recipeSeen = recipeSeen;
  run.consOffsets = consOffsets;
  run.consTargets = consTargets;
  run.nItem = nItem;
  run.itemStamp.assign(nItem, UINT32_MAX);
  run.recipeStamp.assign(graph.nRecipe, UINT32_MAX);
  run.itemPos.assign(nItem, -1);
  run.drop = &drop;
  aw::vector<uint> subtreeStack;

  bool any = false;

  for (uint32_t candidate = 0; candidate < candidates.size(); candidate++) {
    const uint escape = candidates[candidate];
    if (options.satellite.maxSeconds > 0.0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >
            options.satellite.maxSeconds)
      break;
    run.stamp = candidate;

    // I = the items this escape dominates, plus the ones nothing required can
    // reach. An escape outside the tree dominates nothing, so its island is
    // then just the unreached set without itself.
    const uint unreachedSize = (uint) unreached.size();
    const uint islandSize = dom.inTree[escape]
                                ? unreachedSize + dom.subtreeSeen[escape] - 1
                                : unreachedSize - 1;
    if (islandSize == 0)
      continue;  // empty island
    if (islandSize > options.satellite.maxIslandNodes) {
      run.skippedWide++;
      continue;
    }

    run.island.items.clear();
    for (uint j : unreached)
      if (j != escape)
        run.island.items.push_back(j);
    if (dom.inTree[escape])
      collectDominated(dom, escape, itemSeen, subtreeStack, run.island.items);
    // The walk this replaced collected the island by scanning item indices, and
    // which LP point comes back may depend on the order. Keep that order.
    std::sort(run.island.items.begin(), run.island.items.end());
    for (ItemId item : run.island.items)
      run.itemStamp[item] = candidate;

    if (certifyIsland(run, escape))
      any = true;
  }

  if (verbose)
    std::fprintf(stderr,
                 "[satellite/directed] evaluated=%lld dropped=%lld (wide=%lld gates=%lld "
                 "escapes=%lld candidates=%lld) time=%.4f s\n",
                 (long long) run.evaluated, (long long) run.dropped,
                 (long long) run.skippedWide, (long long) run.skippedGates,
                 (long long) run.skippedEscapes, (long long) candidates.size(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                     .count());
  return any;
}

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

bool run(const CraftingGraph& graph, ItemId target, std::span<const uint8_t> itemSeen,
         std::span<const uint8_t> recipeSeen, std::span<const Amount> inventory,
         aw::vector<uint8_t> &drop) noexcept {
  const uint nItem = graph.nItem;
  const uint nNodes = nItem + graph.nRecipe;
  if (target >= nItem || !itemSeen[target])
    return false;

  Undirected und;
  DfsTree tree;
  buildUndirected(graph, recipeSeen, und);
  buildDfsTree(und, target, nNodes, tree);

  aw::vector<uint> subtree;
  aw::vector<int32_t> itemPos(nItem, -1);
  Component comp;
  aw::vector<double> y;
  aw::vector<int64_t> scaled;

  const auto started = std::chrono::steady_clock::now();
  bool any = false;
  int64_t evaluated = 0;
  int64_t dropped = 0;
  int64_t skippedStock = 0;
  int64_t skippedWide = 0;

  // Every item A that has a child c in the DFS tree with low[c] >= disc[A] is an
  // articulation point, and the subtree of c is one component of subgraph - A.
  // Rooting at the target keeps the root side out of the picture and guarantees
  // that the target is never inside a candidate.
  for (uint c = 0; c < nNodes; c++) {
    const uint cut = tree.parent[c];
    if (cut == UINT32_MAX || cut >= nItem || !itemSeen[cut])
      continue;
    if (tree.low[c] < tree.disc[cut])
      continue;
    if (options.satellite.maxSeconds > 0.0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >
            options.satellite.maxSeconds)
      break;

    collectSubtree(und, tree, c, subtree);
    if (subtree.size() > options.satellite.maxComponentNodes) {
      skippedWide++;
      continue;
    }

    // Component breakdown, plus the cheap gates from the lemma.
    comp.items.clear();
    comp.recipes.clear();
    // Every subtree node lands in exactly one of the two lists.
    comp.items.reserve(subtree.size());
    comp.recipes.reserve(subtree.size());
    bool stocked = false;
    for (uint node : subtree) {
      if (node < nItem) {
        comp.items.push_back_unchecked(node);
        if (held(inventory, node) != 0)
          stocked = true;
      } else {
        comp.recipes.push_back_unchecked(node - nItem);
      }
    }
    if (stocked || comp.recipes.empty()) {
      skippedStock++;
      continue;
    }
    for (size_t i = 0; i < comp.items.size(); i++)
      itemPos[comp.items[i]] = (int32_t) i;

    // A component that contains a raw material the player does not hold is
    // dead as well: no plan can run it, so the certificate below accepts it.
    // Dropping it only hides the input the plan is missing; it can never change
    // a feasible plan, and it is what exposes the variants that hang off a
    // basic form and cannot pay it back.
    evaluated++;
    if (componentIsDead(graph, cut, comp, itemPos, y, scaled)) {
      for (uint r : comp.recipes)
        drop[r] = 1;
      any = true;
      dropped++;
    }
    for (ItemId item : comp.items)
      itemPos[item] = -1;
  }
  if (verbose)
    std::fprintf(stderr,
                 "[satellite] evaluated=%lld dropped=%lld (stock=%lld wide=%lld) "
                 "time=%.4f s\n",
                 (long long) evaluated, (long long) dropped, (long long) skippedStock,
                 (long long) skippedWide,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                     .count());
  any |= runDirected(graph, target, itemSeen, recipeSeen, inventory, drop, started);
  return any;
}

}  // namespace

bool computeSatellitePruning(const CraftingGraph& graph, ItemId target,
                             std::span<const uint8_t> itemSeen,
                             std::span<const uint8_t> recipeSeen,
                             std::span<const Amount> inventory,
                             aw::vector<uint8_t> &drop) noexcept {
  if (!options.satellite.enabled)
    return false;
  static const bool debug = std::getenv("AW_SATELLITE_DEBUG") != nullptr;
  verbose = debug;
  try {
    return run(graph, target, itemSeen, recipeSeen, inventory, drop);
  } catch (...) {
    return false;
  }
}

}  // namespace aw
