// awr_inspect -- decodes a .awr dump, prints a summary, and optionally
// validates the CSR invariants, dumps every adjacency row, or expands the
// whole subgraph reachable from one item with all workstations present.
// Built for the WSL side, where running Minecraft is inconvenient.
//
// The optional --subgraph mode also reads the sidecar name table
// (recipes-*.names.tsv) so real items print by name and pseudo-resources
// print as #handle.
//
// Note that -fno-exception is also enabled for this file.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aw/CraftingGraph.h"
#include "aw/Plan.h"
#include "aw/Profiler.h"

namespace {

#define fail(msg, ...) { std::cerr << (msg) << "\n"; return __VA_ARGS__; }

// AW_INSPECT_TIME=1 prints phase timings to stderr. Development aid; off by
// default so the normal run stays quiet.
bool inspectTiming() {
  static const bool enabled = std::getenv("AW_INSPECT_TIME") != nullptr;
  return enabled;
}

double since(const std::chrono::steady_clock::time_point &start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Owns the profiling session for the whole run. It stops the profiler however
// main returns and reports the sample count, which makes a run that was too
// short for a useful flamegraph obvious. `path` stays empty until profiling
// has actually started, so failures and non-profiling runs stay quiet.
struct ProfileGuard {
  aw::profiler::ScopedProfile profiler;
  std::string path;

  ~ProfileGuard() {
    const int64_t gathered = profiler.end();
    if (!path.empty())
      std::cerr << "profile: " << gathered << " sample(s) -> " << path << '\n';
  }
};

std::vector<std::byte> readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    fail("cannot open file " + path, std::vector<std::byte>());
  
  const std::streamsize size = in.tellg();
  if (size < 0)
    fail("cannot compute size of " + path, std::vector<std::byte>());
  in.seekg(0, std::ios::beg);

  std::vector<std::byte> bytes(size);
  if (size > 0 && !in.read((char*) bytes.data(), size))
    fail("cannot read from " + path, std::vector<std::byte>());
  
  return bytes;
}

// The tool is built without exceptions, so parse by hand rather than std::stoi.
bool parseU64(const std::string &text, uint64_t &out) {
  if (text.empty())
    return false;
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    const uint64_t digit = (uint64_t) (c - '0');
    if (value > (UINT64_MAX - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

// The tool is built without exceptions, so parse by hand rather than std::stod.
bool parseDouble(const std::string &text, double &out) {
  if (text.empty())
    return false;
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !(value >= 0.0))
    return false;
  out = value;
  return true;
}

bool parseHandle(const std::string &text, aw::Handle &out) {
  uint64_t value = 0;
  if (!parseU64(text, value) || value > UINT32_MAX)
    return false;
  out = (aw::Handle) value;
  return true;
}

bool parseHandleList(const std::string &text, std::vector<aw::Handle> &out) {
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const std::string piece = text.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start);
    aw::Handle handle = 0;
    if (!parseHandle(piece, handle))
      return false;
    out.push_back(handle);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Name table
// ---------------------------------------------------------------------------
// RecipeReader.writeNameTable emits one line per node:
//
//   <handle> \t <kind> \t <name>
//
// where kind is "resource" (name is the item id) or "ingredient" (name is the
// comma-separated member list of a pseudo-resource). Only resource names are
// used for display; pseudo-resources print as #handle because their member
// lists are too long to read in a listing.
struct NameTable {
  // Indexed by handle - 1. Empty when the image has no entry for it.
  std::vector<std::string> names;
  // Resource name -> handle. When a name is not unique the first handle wins.
  std::unordered_map<std::string, aw::Handle> byName;
  std::unordered_set<std::string> ambiguous;
};

bool loadNames(const std::string &path, aw::Handle nReal, NameTable &out) {
  std::ifstream in(path);
  if (!in)
    return false;

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;

    const size_t first = line.find('\t');
    if (first == std::string::npos)
      continue;
    const size_t second = line.find('\t', first + 1);
    if (second == std::string::npos)
      continue;

    aw::Handle handle = 0;
    if (!parseHandle(line.substr(0, first), handle) || handle == 0)
      continue;

    const std::string name = line.substr(second + 1);
    if (name.empty())
      continue;

    if (out.names.size() < handle)
      out.names.resize(handle);
    out.names[handle - 1] = name;

    // Ingredients are pseudo-resources; only real resources are addressable.
    if (handle <= nReal) {
      auto [it, inserted] = out.byName.emplace(name, handle);
      if (!inserted && it->second != handle)
        out.ambiguous.insert(name);
    }
  }
  return true;
}

std::string defaultNamesPath(const std::string &awr) {
  const std::string suffix = ".awr";
  if (awr.size() >= suffix.size() &&
      awr.compare(awr.size() - suffix.size(), suffix.size(), suffix) == 0)
    return awr.substr(0, awr.size() - suffix.size()) + ".names.tsv";
  return awr + ".names.tsv";
}

std::string itemLabel(const aw::CraftingGraph &graph, const NameTable &names,
                      aw::NodeId node) {
  const aw::Handle handle = graph.itemHandle(node);
  if (node >= graph.nReal)
    return "#" + std::to_string(handle);
  if (handle <= names.names.size() && !names.names[handle - 1].empty()) {
    const std::string &name = names.names[handle - 1];
    // Handles are keyed by (kind, id), so e.g. a fluid and a chemical can share
    // an id. Suffix the handle so those do not look like the same node.
    if (names.ambiguous.count(name) != 0)
      return name + "#" + std::to_string(handle);
    return name;
  }
  return "#" + std::to_string(handle);
}

aw::Handle resolveTarget(const std::string &text, const NameTable &names) {
  aw::Handle handle = 0;
  if (parseHandle(text, handle))
    return handle;
  const auto it = names.byName.find(text);
  return it == names.byName.end() ? 0 : it->second;
}

void printSummary(const aw::CraftingGraph& graph) {
  const size_t itemEdges = graph.i2r.numEdges();
  const size_t inputEdges = graph.r2i.numEdges();

  size_t producedItems = 0;
  for (size_t item = 0; item < graph.nItem; ++item) {
    if (!graph.i2r.targetsOf(item).empty()) ++producedItems;
  }

  std::cout << "real resources   : " << graph.nReal << '\n';
  std::cout << "item nodes     : " << graph.nItem << "  (" << producedItems
        << " produced, " << (graph.nItem - producedItems) << " leaf-only)\n";
  std::cout << "  pseudo-items   : " << (graph.nItem - graph.nReal) << '\n';
  std::cout << "recipe nodes   : " << graph.nRecipe << '\n';
  std::cout << "item -> recipe  : " << itemEdges << " edges\n";
  std::cout << "recipe -> item  : " << inputEdges << " edges\n";
  size_t withWorkstations = 0;
  for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
    if (!graph.workstations.targetsOf(recipe).empty()) ++withWorkstations;
  }
  std::cout << "workstations   : " << graph.workstations.numEdges()
        << " edges (" << withWorkstations << " recipes have one)\n";
  if (graph.nItem != 0) {
    std::cout << "recipes per item : " << ((double) itemEdges / graph.nItem)
          << " average\n";
  }
  if (graph.nRecipe != 0) {
    std::cout << "inputs per recipe: " << ((double) inputEdges / graph.nRecipe)
          << " average\n";
  }
  if (graph.packDominated.size() == graph.nRecipe) {
    size_t packCerts = 0, packEdges = 0, packZero = 0;
    for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
      if (!graph.packDominated[recipe])
        continue;
      ++packCerts;
      packEdges += graph.packCertificates[recipe].support.size();
      packZero += graph.packCertificates[recipe].zeroStock.size();
    }
    std::cout << "pack certs     : " << packCerts << " recipes (" << packEdges
          << " support entries, " << packZero << " zero-stock items)\n";
  }
}

bool mulOverflowInt(int64_t a, int64_t b, int64_t& out) {
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  const bool over = a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
                          : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b);
  if (over)
    return true;
  out = a * b;
  return false;
}

// Verifies the invariants the kernel is allowed to rely on. Returns the number
// of problems found.
size_t checkGraph(const aw::CraftingGraph& graph) {
  size_t problems = 0;
  // This is an error in the file we check, not an error of awr_inspect itself.
  // So we use cout rather than cerr.
  auto report = [&](const std::string& message) {
    std::cout << "  [FAIL] " << message << '\n';
    ++problems;
  };

  auto check = [&](const aw::SparseGraph &csr, size_t rows, size_t maxTarget,
            const std::string &name) {
    if (csr.offsets.size() != rows + 1) {
      report(name + ": offsets size does not match the row count");
      return;
    }

    if (csr.offsets.front() != 0)
      report(name + ": offsets[0] != 0");
    if (csr.offsets.back() != csr.targets.size())
      report(name + ": offsets.back() != edge count");
    if (csr.targets.size() != csr.weights.size())
      report(name + ": targets and weights differ in size");
    
    for (size_t i = 0; i + 1 < csr.offsets.size(); ++i) {
      if (csr.offsets[i] > csr.offsets[i + 1]) {
        report(name + ": offsets are not monotone");
        break;
      }
    }
    for (size_t i = 0; i < csr.targets.size(); ++i) {
      if (csr.targets[i] >= maxTarget) {
        report(name + ": node id out of range");
        break;
      }
      if (csr.weights[i] == 0) {
        report(name + ": zero amount");
        break;
      }
    }
  };

  check(graph.i2r, graph.nItem, graph.nItem + graph.nRecipe, "itemToRecipe");
  check(graph.r2i, graph.nRecipe, graph.nItem, "recipeToItem");

  // The workstation sets have one row per recipe and only ever name real
  // resources, which is what makes the intersection test in reachableSubgraph
  // safe to do with a plain array over the item range.
  const aw::BaseSparseSets &ws = graph.workstations;
  if (ws.offsets.size() != graph.nRecipe + 1) {
    report("workstations: offsets size does not match the recipe count");
  } else {
    if (ws.offsets.front() != 0)
      report("workstations: offsets[0] != 0");
    if (ws.offsets.back() != ws.targets.size())
      report("workstations: offsets.back() != edge count");
    for (size_t i = 0; i + 1 < ws.offsets.size(); ++i) {
      if (ws.offsets[i] > ws.offsets[i + 1]) {
        report("workstations: offsets are not monotone");
        break;
      }
    }
    for (size_t i = 0; i < ws.targets.size(); ++i) {
      if (ws.targets[i] >= graph.nReal) {
        report("workstations: node is not a real resource");
        break;
      }
    }
    for (size_t row = 0; row + 1 < ws.offsets.size(); ++row) {
      bool ascending = true;
      for (size_t i = ws.offsets[row] + 1; i < ws.offsets[row + 1]; ++i) {
        if (ws.targets[i] <= ws.targets[i - 1]) {
          ascending = false;
          break;
        }
      }
      if (!ascending) {
        report("workstations: row is not ascending");
        break;
      }
    }
  }
  // Only synthetic tag edges may be flagged as dominated.
  if (graph.tagEdgeDominated.size() != graph.nRecipe) {
    report("tagEdgeDominated size does not match the recipe count");
  } else {
    for (size_t r = 0; r < graph.nRecipe; ++r) {
      if (!graph.tagEdgeDominated[r])
        continue;
      if (graph.output[r] < graph.nReal) {
        report("a real recipe is marked as a dominated tag edge");
        break;
      }
      if (graph.r2i.targetsOf(r).size() != 1) {
        report("a dominated tag edge is not a single-input recipe");
        break;
      }
    }
  }

  // Only real recipes may be flagged as composite-dominated, and each keeps a
  // real guard input that it actually consumes. Every real item must also keep
  // at least one unflagged recipe, so composite pruning never erases one.
  if (graph.recipeDominated.size() != graph.nRecipe ||
      graph.recipeGuardInput.size() != graph.nRecipe) {
    report("recipe pruning arrays do not match the recipe count");
  } else {
    for (size_t r = 0; r < graph.nRecipe; ++r) {
      if (!graph.recipeDominated[r])
        continue;
      if (graph.output[r] >= graph.nReal) {
        report("a pseudo-resource recipe is marked as composite-dominated");
        break;
      }
      const aw::NodeId guard = graph.recipeGuardInput[r];
      if (guard >= graph.nReal) {
        report("a composite-dominated recipe has no real guard input");
        break;
      }
      bool consumed = false;
      for (aw::NodeId input : graph.r2i.targetsOf(r))
        if (input == guard)
          consumed = true;
      if (!consumed) {
        report("a composite-dominated recipe's guard is not one of its inputs");
        break;
      }
    }
    std::vector<uint8_t> kept(graph.nItem, 0);
    for (size_t r = 0; r < graph.nRecipe; ++r)
      if (graph.output[r] < graph.nReal && !graph.recipeDominated[r])
        kept[graph.output[r]] = 1;
    for (size_t item = 0; item < graph.nReal; ++item) {
      if (!graph.i2r.targetsOf(item).empty() && !kept[item]) {
        report("composite pruning removed every recipe of a real item");
        break;
      }
    }
  }

  // Pack certificates must describe a well-formed pack whose net column is
  // non-positive on every row: that is exactly the property the query-time
  // drop relies on.
  if (graph.packDominated.size() != graph.nRecipe ||
      graph.packCertificates.size() != graph.nRecipe) {
    report("pack pruning arrays do not match the recipe count");
  } else {
    std::vector<int64_t> net(graph.nItem, 0);
    std::vector<aw::NodeId> touched;
    for (size_t r = 0; r < graph.nRecipe; ++r) {
      if (!graph.packDominated[r])
        continue;
      const aw::PackCertificate &cert = graph.packCertificates[r];
      if (cert.support.empty() || cert.support.size() != cert.count.size()) {
        report("a pack certificate has a malformed support");
        break;
      }
      bool hasSelf = false, ascending = true;
      for (size_t k = 0; k < cert.support.size(); ++k) {
        if (cert.support[k] >= graph.nRecipe || cert.count[k] < 1) {
          report("a pack certificate names a bad recipe or count");
          ascending = false;
          break;
        }
        if (k > 0 && cert.support[k] <= cert.support[k - 1])
          ascending = false;
        if (cert.support[k] == r)
          hasSelf = true;
      }
      if (!ascending) {
        report("a pack certificate support is not strictly ascending");
        break;
      }
      if (!hasSelf) {
        report("a pack certificate does not contain its own recipe");
        break;
      }

      touched.clear();
      bool overflow = false;
      for (size_t k = 0; k < cert.support.size() && !overflow; ++k) {
        const size_t s = cert.support[k];
        const int64_t count = cert.count[k];
        auto addNet = [&](aw::NodeId item, int64_t delta) {
          if (net[item] == 0)
            touched.push_back(item);
          if (delta > 0 ? net[item] > INT64_MAX - delta
                        : net[item] < INT64_MIN - delta) {
            overflow = true;
            return;
          }
          net[item] += delta;
        };
        int64_t product = 0;
        if (mulOverflowInt(graph.outputAmt[s], count, product)) {
          overflow = true;
          break;
        }
        addNet(graph.output[s], product);
        const auto inputs = graph.r2i.targetsOf(s);
        const auto weights = graph.r2i.weightsOf(s);
        for (size_t e = 0; e < inputs.size(); ++e) {
          if (mulOverflowInt(weights[e], count, product)) {
            overflow = true;
            break;
          }
          addNet(inputs[e], -product);
        }
      }
      if (overflow) {
        for (aw::NodeId item : touched)
          net[item] = 0;
        report("a pack certificate overflows int64");
        break;
      }
      for (aw::NodeId item : touched) {
        if (net[item] > 0) {
          report("a pack certificate is not a net loss");
          break;
        }
      }
      for (aw::NodeId item : touched)
        net[item] = 0;

      bool zeroAscending = true;
      for (size_t k = 0; k < cert.zeroStock.size(); ++k) {
        if (cert.zeroStock[k] >= graph.nReal) {
          report("a pack certificate names a non-real zero-stock item");
          zeroAscending = false;
          break;
        }
        if (k > 0 && cert.zeroStock[k] <= cert.zeroStock[k - 1])
          zeroAscending = false;
      }
      if (!zeroAscending) {
        report("a pack certificate zero-stock set is not strictly ascending");
        break;
      }
    }
  }

  for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
    if (graph.output[recipe] >= graph.nReal)
      continue;  // Synthetic; it needs no workstation.
    if (graph.workstations.targetsOf(recipe).empty())
      report("a recipe that outputs a real resource has no workstation");
  }

  if (graph.output.size() != graph.nRecipe ||
    graph.outputAmt.size() != graph.nRecipe) {
    report("output/outputAmt size does not match the recipe count");
    return problems;
  }
  if (graph.i2r.numEdges() != graph.nRecipe) {
    report("i2r must contain exactly one edge per recipe");
  }

  // Every recipe must appear exactly once, in the row of the item it outputs.
  std::vector<uint32_t> seen(graph.nRecipe, 0);
  for (size_t item = 0; item < graph.nItem; ++item) {
    auto targets = graph.i2r.targetsOf(item);
    auto weights = graph.i2r.weightsOf(item);
    for (size_t k = 0; k < targets.size(); ++k) {
      const uint32_t recipe = targets[k] - graph.nItem;
      if (recipe >= graph.nRecipe) {
        report("i2r points at an out-of-range recipe");
        continue;
      }
      if (++seen[recipe] > 1) report("recipe listed more than once");
      if (graph.output[recipe] != item) {
        report("recipe is in the wrong item row");
      }
      if (graph.outputAmt[recipe] != weights[k]) {
        report("recipe output amount disagrees with the edge weight");
      }
    }
  }
  for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
    if (seen[recipe] == 0)
      report("recipe is not reachable from any item");
  }

  // Check that there's no duplication.
  {
    std::vector<uint32_t> order(graph.nRecipe);
    for (uint32_t r = 0; r < graph.nRecipe; ++r)
      order[r] = r;

    auto keyLess = [&graph](uint32_t a, uint32_t b) {
      if (graph.output[a] != graph.output[b])
        return graph.output[a] < graph.output[b];
      if (graph.outputAmt[a] != graph.outputAmt[b])
        return graph.outputAmt[a] < graph.outputAmt[b];
      const auto ta = graph.r2i.targetsOf(a);
      const auto tb = graph.r2i.targetsOf(b);
      const auto wa = graph.r2i.weightsOf(a);
      const auto wb = graph.r2i.weightsOf(b);
      const size_t shared = ta.size() < tb.size() ? ta.size() : tb.size();
      for (size_t i = 0; i < shared; ++i) {
        if (ta[i] != tb[i])
          return ta[i] < tb[i];
        if (wa[i] != wb[i])
          return wa[i] < wb[i];
      }
      return ta.size() < tb.size();
    };

    std::sort(order.begin(), order.end(), keyLess);
    for (size_t i = 1; i < order.size(); ++i) {
      if (!keyLess(order[i - 1], order[i])) {
        report("two recipes share the same output, amount and inputs");
        break;
      }
    }
  }

  return problems;
}

void dumpGraph(const aw::CraftingGraph& graph) {
  std::cout << "item -> recipe\n";
  for (size_t item = 0; item < graph.nItem; ++item) {
    auto targets = graph.i2r.targetsOf(item);
    if (targets.empty()) continue;
    std::cout << "  item " << graph.itemHandle(item) << ":";
    auto weights = graph.i2r.weightsOf(item);
    for (size_t k = 0; k < targets.size(); ++k) {
      std::cout << " r" << (targets[k] - graph.nItem) << " x" << weights[k];
    }
    std::cout << '\n';
  }

  std::cout << "recipe -> item\n";
  for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
    std::cout << "  r" << recipe << " -> item " << graph.itemHandle(graph.output[recipe])
          << " x" << graph.outputAmt[recipe] << " <-";
    auto targets = graph.r2i.targetsOf(recipe);
    auto weights = graph.r2i.weightsOf(recipe);
    for (size_t k = 0; k < targets.size(); ++k) {
      std::cout << " item " << graph.itemHandle(targets[k]) << " x" << weights[k];
    }
    std::cout << '\n';
  }
}

// Prints every recipe of `sub` as "output xN <- input xN, ...", plus a few
// stats. Pseudo-resources are shown as #handle. Item nodes are renumbered in
// the subgraph, so they are mapped back through itemOrigin before labelling.
void dumpSubgraph(const aw::Subgraph &sub, const aw::CraftingGraph &graph,
                  const NameTable &names) {
  const aw::BaseCraftingGraph &g = sub.graph;

  auto item = [&](aw::NodeId node) {
    return itemLabel(graph, names, sub.itemOrigin[node]);
  };

  size_t leaves = 0;
  size_t multi = 0;
  size_t bulk = 0;
  std::vector<aw::NodeId> leafItems;
  for (aw::NodeId i = 0; i < g.nItem; i++) {
    const size_t producers = g.i2r.targetsOf(i).size();
    if (producers == 0) {
      leaves++;
      leafItems.push_back(i);
    }
    if (producers > 1)
      multi++;
  }
  for (uint32_t r = 0; r < g.nRecipe; r++) {
    if (g.outputAmt[r] > 1)
      bulk++;
  }

  size_t noWorkstation = 0;
  for (uint32_t r = 0; r < graph.nRecipe; ++r) {
    if (graph.output[r] < graph.nReal && graph.workstations.targetsOf(r).empty())
      noWorkstation++;
  }

  std::cout << "# subgraph: " << g.nItem << " items (" << g.nReal << " real, "
            << (g.nItem - g.nReal) << " pseudo), " << g.nRecipe << " recipes\n";
  std::cout << "# items with >1 recipe : " << multi << '\n';
  std::cout << "# recipes with amount>1 : " << bulk << '\n';
  std::cout << "# leaf items (no recipe) : " << leaves << '\n';
  if (noWorkstation != 0)
    std::cout << "# note: " << noWorkstation
              << " real recipes have no workstation and are never reachable\n";

  for (uint32_t r = 0; r < g.nRecipe; r++) {
    std::cout << item(g.output[r]) << " x" << g.outputAmt[r] << " <- ";
    const auto targets = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    if (targets.empty()) {
      std::cout << "(nothing)";
    } else {
      for (size_t k = 0; k < targets.size(); k++) {
        if (k != 0)
          std::cout << ", ";
        std::cout << item(targets[k]) << " x" << weights[k];
      }
    }
    std::cout << '\n';
  }

  std::cout << "#\n# leaf items:\n";
  for (aw::NodeId i : leafItems)
    std::cout << "#   " << item(i) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string namesPath;
  std::string treeArg;
  std::string planArg;
  std::string invArg;
  std::string profilePath;
  ProfileGuard profileGuard;
  bool check = false;
  bool dump = false;
  bool noPrune = false;
  bool noRecipePrune = false;
  bool noPackPrune = false;
  double packSeconds = -1.0;
  bool noSatellitePrune = false;
  double satelliteSeconds = -1.0;
  bool doReach = false;
  bool doTree = false;
  bool doPlan = false;
  uint64_t planAmount = 1;
  aw::solver::Options solverOptions;
  aw::Handle reach = 0;
  std::vector<aw::Handle> workstations;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--check") {
      check = true;
    } else if (arg == "--dump") {
      dump = true;
    } else if (arg == "--no-prune") {
      noPrune = true;
    } else if (arg == "--no-recipe-prune") {
      noRecipePrune = true;
    } else if (arg == "--no-pack-prune") {
      noPackPrune = true;
    } else if (arg == "--no-satellite-prune") {
      noSatellitePrune = true;
    } else if (arg == "--satellite-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], satelliteSeconds)) {
        std::cerr << "--satellite-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--pack-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], packSeconds)) {
        std::cerr << "--pack-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--profile") {
      if (i + 1 >= argc) {
        std::cerr << "--profile needs an output path\n";
        return EXIT_FAILURE;
      }
      profilePath = argv[++i];
    } else if (arg == "--names") {
      if (i + 1 >= argc) {
        std::cerr << "--names needs a path\n";
        return EXIT_FAILURE;
      }
      namesPath = argv[++i];
    } else if (arg == "--subgraph" || arg == "--tree") {
      if (i + 1 >= argc) {
        std::cerr << "--subgraph needs an item name or handle\n";
        return EXIT_FAILURE;
      }
      treeArg = argv[++i];
      doTree = true;
    } else if (arg == "--reach") {
      if (i + 1 >= argc || !parseHandle(argv[++i], reach) || reach == 0) {
        std::cerr << "--reach needs a resource handle\n";
        return EXIT_FAILURE;
      }
      doReach = true;
    } else if (arg == "--plan") {
      if (i + 1 >= argc) {
        std::cerr << "--plan needs an item name or handle\n";
        return EXIT_FAILURE;
      }
      planArg = argv[++i];
      doPlan = true;
    } else if (arg == "--amount") {
      if (i + 1 >= argc || !parseU64(argv[++i], planAmount)) {
        std::cerr << "--amount needs a non-negative integer\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--time-limit") {
      if (i + 1 >= argc || !parseDouble(argv[++i], solverOptions.maxTimeSeconds)) {
        std::cerr << "--time-limit needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--gap") {
      if (i + 1 >= argc || !parseDouble(argv[++i], solverOptions.relativeGap)) {
        std::cerr << "--gap needs a fraction (0.01 is one percent)\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--workers") {
      uint64_t workers = 0;
      if (i + 1 >= argc || !parseU64(argv[++i], workers) || workers > 1024) {
        std::cerr << "--workers needs a small non-negative integer\n";
        return EXIT_FAILURE;
      }
      solverOptions.numWorkers = (int) workers;
    } else if (arg == "--ub") {
      uint64_t bound = 0;
      if (i + 1 >= argc || !parseU64(argv[++i], bound)) {
        std::cerr << "--ub needs a non-negative integer\n";
        return EXIT_FAILURE;
      }
      solverOptions.objectiveCap = (int64_t) bound;
    } else if (arg == "--inv") {
      if (i + 1 >= argc) {
        std::cerr << "--inv needs a comma-separated list of handle=amount\n";
        return EXIT_FAILURE;
      }
      invArg = argv[++i];
    } else if (arg == "--ws") {
      if (i + 1 >= argc || !parseHandleList(argv[++i], workstations)) {
        std::cerr << "--ws needs a comma-separated list of resource handles\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "usage: awr_inspect [--check] [--dump] [--reach <handle>] [--ws <handle,...>]\n"
                   "                   [--no-prune] [--no-recipe-prune] [--no-pack-prune]\n"
                   "                   [--no-satellite-prune] [--satellite-seconds <s>]\n"
                   "                   [--pack-seconds <s>]\n"
                   "                   [--plan <name|handle>] [--amount <n>] [--inv <h=a,...>]\n"
                   "                   [--time-limit <s>] [--gap <f>] [--workers <n>] [--ub <n>]\n"
                   "                   [--names <table.tsv>] [--subgraph <name|handle>]\n"
                   "                   [--profile <out.prof>] <recipes.awr>\n";
      return EXIT_SUCCESS;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::cerr << "unexpected argument: " << arg << '\n';
      return EXIT_FAILURE;
    }
  }

  if (path.empty()) {
    std::cerr << "usage: awr_inspect [--check] [--dump] [--reach <handle>] [--ws <handle,...>]\n"
                 "                   [--no-prune] [--no-recipe-prune] [--no-pack-prune]\n"
                 "                   [--no-satellite-prune] [--satellite-seconds <s>]\n"
                 "                   [--pack-seconds <s>]\n"
                 "                   [--plan <name|handle>] [--amount <n>] [--inv <h=a,...>]\n"
                 "                   [--time-limit <s>] [--gap <f>] [--workers <n>] [--ub <n>]\n"
                 "                   [--names <table.tsv>] [--subgraph <name|handle>]\n"
                 "                   [--profile <out.prof>] <recipes.awr>\n";
    return EXIT_FAILURE;
  }

  // The profiler is a build-time optional, runtime opt-in. Fail loudly rather
  // than silently producing no file.
  if (!profilePath.empty() && !aw::profiler::available()) {
    std::cerr << "--profile: this build has no CPU profiler; install "
                 "libgoogle-perftools-dev and rebuild awr_inspect\n";
    return EXIT_FAILURE;
  }

  const std::vector<std::byte> bytes = readFile(path);

  // The certificate pass runs at registration time, so its options have to be
  // installed before the graph is registered.
  {
    aw::PackPruneOptions packOptions = aw::getPackPruningOptions();
    if (noPackPrune)
      packOptions.enabled = false;
    if (packSeconds >= 0.0)
      packOptions.maxSeconds = packSeconds;
    aw::setPackPruningOptions(packOptions);
  }

  // Satellite elimination runs per query, on the reachable subgraph.
  {
    aw::SatellitePruneOptions satelliteOptions = aw::getSatellitePruningOptions();
    if (noSatellitePrune)
      satelliteOptions.enabled = false;
    if (satelliteSeconds >= 0.0)
      satelliteOptions.maxSeconds = satelliteSeconds;
    aw::setSatellitePruningOptions(satelliteOptions);
  }

  // Profile the whole run: decode + canonicalize + prune precompute, the
  // reachability pass, the LP/CP-SAT solve, and the report. Started before the
  // graph is registered and stopped by ProfileGuard on the way out.
  if (!profilePath.empty()) {
    if (!profileGuard.profiler.begin(profilePath)) {
      std::cerr << "--profile: cannot write " << profilePath << '\n';
      return EXIT_FAILURE;
    }
    profileGuard.path = profilePath;
  }

  const auto registerStart = std::chrono::steady_clock::now();
  aw::registerCraftingGraph(bytes);
  if (inspectTiming())
    std::fprintf(stderr, "[time] register+prune: %.3f s\n", since(registerStart));
  if (const char *error = aw::getCraftingError()) {
    std::cout << "malformed graph: " << error << "\n";
    return EXIT_SUCCESS;
  }
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  if (noPrune)
    aw::setTagPruningEnabled(false);
  if (noRecipePrune)
    aw::setRecipePruningEnabled(false);

  std::cout << "parsed " << bytes.size() << " bytes from " << path << '\n';
  printSummary(graph);
  if (doTree) {
    NameTable names;
    const std::string tablePath = namesPath.empty() ? defaultNamesPath(path) : namesPath;
    if (loadNames(tablePath, graph.nReal, names)) {
      std::cout << "names: " << names.names.size() << " entries from " << tablePath << '\n';
    } else if (!namesPath.empty()) {
      std::cerr << "cannot read name table: " << tablePath << '\n';
      return EXIT_FAILURE;
    } else {
      std::cout << "names: none (looked for " << tablePath << "), using handles\n";
    }

    if (names.ambiguous.count(treeArg) != 0)
      std::cerr << "warning: '" << treeArg << "' matches multiple handles; using "
                << names.byName[treeArg] << '\n';

    const aw::Handle target = resolveTarget(treeArg, names);
    if (target == 0 || target > graph.nItem) {
      std::cerr << "unknown item: " << treeArg << '\n';
      return EXIT_FAILURE;
    }

    // "All workstations present": allow every real resource as a station.
    std::vector<aw::Handle> all;
    all.reserve(graph.nReal);
    for (aw::Handle h = 1; h <= graph.nReal; ++h)
      all.push_back(h);

    const aw::Subgraph sub = aw::reachableSubgraph(target, all);
    if (sub.graph.nItem == 0) {
      std::cerr << "no subgraph reachable from " << treeArg << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "subgraph from " << itemLabel(graph, names, target - 1)
              << " (all workstations):\n";
    dumpSubgraph(sub, graph, names);
  }
  if (doReach) {
    aw::Subgraph sub = aw::reachableSubgraph(reach, workstations);
    if (sub.graph.nItem == 0) {
      std::cout << "reachable from handle " << reach << ": invalid output handle\n";
    } else {
      std::cout << "reachable from handle " << reach << " with " << workstations.size()
            << " workstation(s):\n";
      std::cout << "  item nodes : " << sub.graph.nItem << '\n';
      std::cout << "  recipes    : " << sub.graph.nRecipe << '\n';
    }
  }
  if (doPlan) {
    NameTable names;
    const std::string tablePath = namesPath.empty() ? defaultNamesPath(path) : namesPath;
    if (loadNames(tablePath, graph.nReal, names)) {
      std::cout << "names: " << names.names.size() << " entries from " << tablePath << '\n';
    } else if (!namesPath.empty()) {
      std::cerr << "cannot read name table: " << tablePath << '\n';
      return EXIT_FAILURE;
    }

    if (names.ambiguous.count(planArg) != 0)
      std::cerr << "warning: '" << planArg << "' matches multiple handles; using "
                << names.byName[planArg] << '\n';

    const aw::Handle target = resolveTarget(planArg, names);
    if (target == 0 || target > graph.nItem) {
      std::cerr << "unknown item: " << planArg << '\n';
      return EXIT_FAILURE;
    }

    std::vector<aw::Handle> stations = workstations;
    const bool allStations = stations.empty();
    if (allStations) {
      stations.reserve(graph.nReal);
      for (aw::Handle h = 1; h <= graph.nReal; ++h)
        stations.push_back(h);
    }

    // Inventory is given per handle and stored per source item node. It is built
    // before the reachability pass so the dominated tag edges can consult it.
    std::vector<aw::Amount> inventory(graph.nItem, 0);
    if (!invArg.empty()) {
      size_t start = 0;
      while (true) {
        const size_t comma = invArg.find(',', start);
        const std::string piece = invArg.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t equal = piece.find('=');
        aw::Handle handle = 0;
        uint64_t amount = 0;
        if (equal == std::string::npos || !parseHandle(piece.substr(0, equal), handle) ||
            !parseU64(piece.substr(equal + 1), amount) || handle == 0 ||
            handle > graph.nItem) {
          std::cerr << "bad --inv entry: " << piece << '\n';
          return EXIT_FAILURE;
        }
        inventory[aw::CraftingGraph::itemNode(handle)] += (aw::Amount) amount;
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    }

    const auto reachStart = std::chrono::steady_clock::now();
    const aw::Subgraph sub = aw::reachableSubgraph(target, stations, inventory);
    if (inspectTiming())
      std::fprintf(stderr, "[time] reachableSubgraph: %.3f s\n", since(reachStart));
    if (sub.graph.nItem == 0) {
      std::cerr << "no subgraph reachable from " << planArg << '\n';
      return EXIT_FAILURE;
    }

    std::cout << "plan for " << itemLabel(graph, names, target - 1) << " x" << planAmount;
    std::cout << (allStations ? " (all workstations)"
                              : " (" + std::to_string(stations.size()) + " workstations)");
    std::cout << ":\n";
    std::cout << "  subgraph: " << sub.graph.nItem << " items, " << sub.graph.nRecipe
              << " recipes\n";

    const aw::NodeId targetNode = sub.translate(aw::CraftingGraph::itemNode(target));
    const auto planStart = std::chrono::steady_clock::now();
    const aw::PlanResult plan =
        aw::planCrafting(sub, targetNode, planAmount, inventory, solverOptions);
    if (inspectTiming())
      std::fprintf(stderr, "[time] planCrafting(solve): %.3f s\n", since(planStart));

    const char *statusName = "?";
    switch (plan.status) {
      case aw::PlanStatus::OK: statusName = "ok"; break;
      case aw::PlanStatus::INFEASIBLE: statusName = "infeasible"; break;
      case aw::PlanStatus::NUMERICAL_FAIL: statusName = "numerical failure"; break;
      case aw::PlanStatus::ITER_LIMIT: statusName = "iteration limit"; break;
      case aw::PlanStatus::INVALID_INPUT: statusName = "invalid input"; break;
    }
    std::cout << "  status: " << statusName;
    if (plan.status == aw::PlanStatus::OK)
      std::cout << (plan.provenOptimal ? " (proven optimal)"
                                       : " (gave up early)")
                << ", gap=" << plan.gap << ", bound=" << plan.bestBound;
    std::cout << ", conflicts=" << plan.numConflicts
              << ", branches=" << plan.numBranches
              << ", fixed=" << plan.fixedColumns << '\n';

    if (plan.status == aw::PlanStatus::OK) {
      int64_t totalExec = 0;
      for (int64_t x : plan.exec)
        totalExec += x;

      std::cout << "  total executions: " << totalExec << '\n';
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
        const int64_t count = plan.exec[r];
        if (count == 0)
          continue;
        std::cout << "  " << count << " x "
                  << itemLabel(graph, names, sub.itemOrigin[sub.graph.output[r]]) << " x"
                  << sub.graph.outputAmt[r] << " <- ";
        const auto inputs = sub.graph.r2i.targetsOf(r);
        const auto weights = sub.graph.r2i.weightsOf(r);
        if (inputs.empty()) {
          std::cout << "(nothing)";
        } else {
          for (size_t k = 0; k < inputs.size(); k++) {
            if (k != 0)
              std::cout << ", ";
            std::cout << itemLabel(graph, names, sub.itemOrigin[inputs[k]]) << " x"
                      << weights[k];
          }
        }
        std::cout << '\n';
      }
    }
  }
  if (dump)
    dumpGraph(graph);
  if (check) {
    const size_t problems = checkGraph(graph);
    std::cout << (problems == 0 ? "checks: OK\n" : "checks: FAILED\n");
    return problems == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
