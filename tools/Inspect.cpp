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

#include <chrono>
#include <fstream>
#include <iostream>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Profiler.h"
#include "aw/plan/Status.h"

#include "WorkstationSample.h"

namespace {

#define fail(msg, ...) { std::cerr << (msg) << "\n"; return __VA_ARGS__; }

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

aw::vector<std::byte> readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    fail("cannot open file " + path, aw::vector<std::byte>());
  
  const std::streamsize size = in.tellg();
  if (size < 0)
    fail("cannot compute size of " + path, aw::vector<std::byte>());
  in.seekg(0, std::ios::beg);

  aw::vector<std::byte> bytes(size);
  if (size > 0 && !in.read((char*) bytes.data(), size))
    fail("cannot read from " + path, aw::vector<std::byte>());
  
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

bool parseHandleList(const std::string &text, aw::vector<aw::Handle> &out) {
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
// RecipeReader.writeNameTable emits one line per real resource:
//
//   <handle> \t <resource location> \t <en_US> \t <zh_CN>
//
// The resource location and the English name both resolve a command-line
// target; the English name is what gets printed. Pseudo-resources are absent
// from the table because they have no resource location, and they have always
// printed as #handle.
struct NameTable {
  // Indexed by handle - 1. Empty when the image has no entry for it.
  aw::vector<std::string> names;
  // Alias -> handle. When an alias is not unique the first handle wins.
  std::unordered_map<std::string, aw::Handle> byName;
  std::unordered_set<std::string> ambiguous;
};

// Records an alias, marking it ambiguous when a second handle claims it.
void addName(NameTable &out, const std::string &alias, aw::Handle handle) {
  if (alias.empty())
    return;
  auto [it, inserted] = out.byName.emplace(alias, handle);
  if (!inserted && it->second != handle)
    out.ambiguous.insert(alias);
}

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

    // Split on tabs. Only the first four columns are read, so a stray tab in a
    // translation cannot shift the parse.
    std::string column[4];
    size_t start = 0;
    for (int i = 0; i < 4; i++) {
      const size_t tab = line.find('\t', start);
      column[i] = line.substr(
          start, tab == std::string::npos ? std::string::npos : tab - start);
      if (tab == std::string::npos)
        break;
      start = tab + 1;
    }

    aw::Handle handle = 0;
    if (!parseHandle(column[0], handle) || handle == 0)
      continue;

    const std::string &resource = column[1];
    const std::string &name = column[2];

    if (out.names.size() < handle)
      out.names.resize(handle);
    if (!name.empty())
      out.names[handle - 1] = name;

    // Ingredients are pseudo-resources; only real resources are addressable.
    if (handle <= nReal) {
      addName(out, resource, handle);
      addName(out, name, handle);
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
                      aw::ItemId node) {
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
    if (!graph.producersOf(item).empty()) ++producedItems;
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
  size_t pseudoRecipes = 0, tagDominated = 0, recipeDominated = 0;
  size_t domWsEntries = 0, domWsEmpty = 0;
  size_t directDominated = 0, directWsEntries = 0, directWsEmpty = 0;
  size_t substituted = 0, substWsEntries = 0, substWsEmpty = 0, substGuards = 0;
  for (size_t r = 0; r < graph.nRecipe; ++r) {
    if (graph.output[r] >= graph.nReal) ++pseudoRecipes;
    if (r < graph.tagEdgeDominated.size() && graph.tagEdgeDominated[r]) ++tagDominated;
    if (r < graph.recipeDominated.size() && graph.recipeDominated[r]) {
      ++recipeDominated;
      if (r < graph.recipeDominatorWorkstations.size()) {
        if (graph.recipeDominatorWorkstations[r].empty())
          ++domWsEmpty;
        else
          domWsEntries += graph.recipeDominatorWorkstations[r].size();
      } else {
        ++domWsEmpty;
      }
    }
    if (r < graph.recipeDirectDominated.size() && graph.recipeDirectDominated[r]) {
      ++directDominated;
      if (r < graph.recipeDirectDominatorWorkstations.size()) {
        if (graph.recipeDirectDominatorWorkstations[r].empty())
          ++directWsEmpty;
        else
          directWsEntries += graph.recipeDirectDominatorWorkstations[r].size();
      } else {
        ++directWsEmpty;
      }
    }
    if (r < graph.recipeSubstituted.size() && graph.recipeSubstituted[r]) {
      ++substituted;
      if (r < graph.recipeSubstitutedDominatorWorkstations.size()) {
        if (graph.recipeSubstitutedDominatorWorkstations[r].empty())
          ++substWsEmpty;
        else
          substWsEntries += graph.recipeSubstitutedDominatorWorkstations[r].size();
      } else {
        ++substWsEmpty;
      }
      if (r < graph.recipeSubstitutedGuards.size())
        substGuards += graph.recipeSubstitutedGuards[r].size();
    }
  }
  std::cout << "tag edges      : " << pseudoRecipes << " (" << tagDominated
        << " dominated)\n";
  std::cout << "recipes dominated: " << recipeDominated << " (" << domWsEntries
        << " dominator workstations, " << domWsEmpty << " without any)\n";
  std::cout << "direct dominated : " << directDominated << " (" << directWsEntries
        << " dominator workstations, " << directWsEmpty << " without any)\n";
  std::cout << "substituted      : " << substituted << " (" << substWsEntries
        << " dominator workstations, " << substWsEmpty << " without any, "
        << substGuards << " guard items)\n";
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

  check(graph.i2r, graph.nItem, graph.nRecipe, "itemToRecipe");
  check(graph.r2i, graph.nRecipe, graph.nItem, "recipeToItem");
  check(graph.r2o, graph.nRecipe, graph.nItem, "recipeToOutput");

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
      if (graph.inputsOf(r).size() != 1) {
        report("a dominated tag edge is not a single-input recipe");
        break;
      }
    }
  }

  // Only real recipes may be flagged as composite-dominated, and each keeps a
  // real guard input that it actually consumes. Every real item must also keep
  // at least one unflagged recipe, so composite pruning never erases one.
  if (graph.recipeDominated.size() != graph.nRecipe ||
      graph.recipeGuardInput.size() != graph.nRecipe ||
      graph.recipeDominatorWorkstations.size() != graph.nRecipe) {
    report("recipe pruning arrays do not match the recipe count");
  } else {
    for (size_t r = 0; r < graph.nRecipe; ++r) {
      if (!graph.recipeDominated[r])
        continue;
      if (graph.output[r] >= graph.nReal) {
        report("a pseudo-resource recipe is marked as composite-dominated");
        break;
      }
      const aw::ItemId guard = graph.recipeGuardInput[r];
      if (guard >= graph.nReal) {
        report("a composite-dominated recipe has no real guard input");
        break;
      }
      bool consumed = false;
      for (aw::ItemId input : graph.inputsOf(r))
        if (input == guard)
          consumed = true;
      if (!consumed) {
        report("a composite-dominated recipe's guard is not one of its inputs");
        break;
      }
      // The dominator workstations are a sorted set of item nodes. The list may
      // be empty: a real recipe with no workstation can be a representative in
      // graphs where that happens, and such a dominated recipe is simply never
      // dropped.
      const aw::vector<aw::ItemId> &ws = graph.recipeDominatorWorkstations[r];
      bool wsOk = true;
      for (size_t k = 0; k < ws.size(); ++k) {
        if (ws[k] >= graph.nItem || (k > 0 && ws[k] <= ws[k - 1]))
          wsOk = false;
      }
      if (!wsOk) {
        report("a composite-dominated recipe has a bad dominator workstation");
        break;
      }
    }
    auto kept = aw::vector<uint8_t>::zeroes(graph.nItem);
    // Real recipes are a prefix, so the scan stops at the first tag edge.
    for (size_t r = 0; r < graph.nRecipe && graph.output[r] < graph.nReal; ++r) {
      if (graph.recipeDominated[r])
        continue;
      for (aw::ItemId o : graph.outputsOf(r))
        kept[o] = 1;
    }
    for (size_t item = 0; item < graph.nReal; ++item) {
      if (!graph.producersOf(item).empty() && !kept[item]) {
        report("composite pruning removed every recipe of a real item");
        break;
      }
    }
  }

  // Only real recipes may be flagged as directly dominated, and each records a
  // sorted set of valid dominator workstations. The direct pass on its own must
  // also keep at least one recipe of every real item.
  if (graph.recipeDirectDominated.size() != graph.nRecipe ||
      graph.recipeDirectDominatorWorkstations.size() != graph.nRecipe) {
    report("direct dominance arrays do not match the recipe count");
  } else {
    for (size_t r = 0; r < graph.nRecipe; ++r) {
      if (!graph.recipeDirectDominated[r])
        continue;
      if (graph.output[r] >= graph.nReal) {
        report("a pseudo-resource recipe is marked as directly dominated");
        break;
      }
      const aw::vector<aw::ItemId> &ws = graph.recipeDirectDominatorWorkstations[r];
      bool wsOk = true;
      for (size_t k = 0; k < ws.size(); ++k) {
        if (ws[k] >= graph.nItem || (k > 0 && ws[k] <= ws[k - 1]))
          wsOk = false;
      }
      if (!wsOk) {
        report("a directly dominated recipe has a bad dominator workstation");
        break;
      }
    }
    // Both flags are read for every real item, so they must start zeroed:
    // PodVector does not value-initialize, unlike the std::vector this replaced.
    auto kept = aw::vector<uint8_t>::zeroes(graph.nItem);
    auto keptEither = aw::vector<uint8_t>::zeroes(graph.nItem);
    // Real recipes are a prefix, so the scan stops at the first tag edge.
    for (size_t r = 0; r < graph.nRecipe && graph.output[r] < graph.nReal; ++r) {
      if (!graph.recipeDirectDominated[r])
        for (aw::ItemId o : graph.outputsOf(r))
          kept[o] = 1;
      if (!graph.recipeDirectDominated[r] && !graph.recipeDominated[r] &&
          !(r < graph.recipeSubstituted.size() && graph.recipeSubstituted[r]))
        for (aw::ItemId o : graph.outputsOf(r))
          keptEither[o] = 1;
    }
    for (size_t item = 0; item < graph.nReal; ++item) {
      if (graph.producersOf(item).empty())
        continue;
      if (!kept[item]) {
        report("direct dominance removed every recipe of a real item");
        break;
      }
      if (!keptEither[item]) {
        report("the recipe passes together removed every recipe");
        break;
      }
    }
  }

  // A substituted recipe keeps a stock guard (real items, sorted) and a set of
  // dominator workstations. The guard is the union of the collapsed inputs along
  // the replacement path, so an intermediate edge's guard need not be an input
  // of the recipe itself; only its range and ordering are checked.
  if (graph.recipeSubstituted.size() != graph.nRecipe ||
      graph.recipeSubstitutedGuards.size() != graph.nRecipe ||
      graph.recipeSubstitutedDominatorWorkstations.size() != graph.nRecipe) {
    report("substitution arrays do not match the recipe count");
  } else {
    for (size_t r = 0; r < graph.nRecipe; ++r) {
      if (!graph.recipeSubstituted[r])
        continue;
      if (graph.output[r] >= graph.nReal) {
        report("a pseudo-resource recipe is marked as substituted");
        break;
      }
      const aw::vector<aw::ItemId> &guards = graph.recipeSubstitutedGuards[r];
      bool guardsOk = true;
      for (size_t k = 0; k < guards.size(); ++k) {
        if (guards[k] >= graph.nReal || (k > 0 && guards[k] <= guards[k - 1])) {
          guardsOk = false;
          break;
        }
      }
      if (!guardsOk) {
        report("a substituted recipe has a bad stock guard");
        break;
      }
      const aw::vector<aw::ItemId> &ws =
          graph.recipeSubstitutedDominatorWorkstations[r];
      bool wsOk = true;
      for (size_t k = 0; k < ws.size(); ++k) {
        if (ws[k] >= graph.nItem || (k > 0 && ws[k] <= ws[k - 1]))
          wsOk = false;
      }
      if (!wsOk) {
        report("a substituted recipe has a bad dominator workstation");
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
    auto net = aw::vector<int64_t>::zeroes(graph.nItem);
    aw::vector<aw::ItemId> touched;
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
        auto addNet = [&](aw::ItemId item, int64_t delta) {
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
        {
          const auto outs = graph.outputsOf(s);
          const auto outAmts = graph.outputAmountsOf(s);
          for (size_t o = 0; o < outs.size(); ++o) {
            if (mulOverflowInt(outAmts[o], count, product)) {
              overflow = true;
              break;
            }
            addNet(outs[o], product);
          }
          if (overflow)
            break;
        }
        const auto inputs = graph.inputsOf(s);
        const auto weights = graph.inputAmountsOf(s);
        for (size_t e = 0; e < inputs.size(); ++e) {
          if (mulOverflowInt(weights[e], count, product)) {
            overflow = true;
            break;
          }
          addNet(inputs[e], -product);
        }
      }
      if (overflow) {
        for (aw::ItemId item : touched)
          net[item] = 0;
        report("a pack certificate overflows int64");
        break;
      }
      for (aw::ItemId item : touched) {
        if (net[item] > 0) {
          report("a pack certificate is not a net loss");
          break;
        }
      }
      for (aw::ItemId item : touched)
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

  // Synthetic tag edges need no workstation, and they are a suffix of the
  // recipe list, so the check stops at the first one.
  for (size_t recipe = 0; recipe < graph.nRecipe && graph.output[recipe] < graph.nReal;
       ++recipe) {
    if (graph.workstations.targetsOf(recipe).empty())
      report("a recipe that outputs a real resource has no workstation");
  }

  if (graph.output.size() != graph.nRecipe ||
    graph.outputAmt.size() != graph.nRecipe ||
    graph.cost.size() != graph.nRecipe) {
    report("output/outputAmt/cost size does not match the recipe count");
    return problems;
  }
  // One execution of a real recipe has to cost at least one underlying
  // execution, otherwise the objective would not bound its column; a tag edge
  // is free. See BaseCraftingGraph::cost.
  for (size_t r = 0; r < graph.nRecipe; ++r) {
    if (graph.output[r] < graph.nReal ? graph.cost[r] < 1 : graph.cost[r] != 0) {
      report("a recipe's cost contradicts its output kind");
      break;
    }
  }
  // The real recipes must be a prefix of the recipe list: that order is what
  // lets the passes stop at the first tag edge. See BaseCraftingGraph::output.
  for (size_t r = 1; r < graph.output.size(); ++r) {
    if (graph.output[r] < graph.output[r - 1]) {
      report("output is not non-decreasing over the recipe ids");
      break;
    }
  }
  size_t totalOutputs = 0;
  for (size_t r = 0; r < graph.nRecipe; ++r)
    totalOutputs += graph.outputsOf(r).size();
  if (graph.i2r.numEdges() != totalOutputs) {
    report("i2r must contain exactly one edge per recipe output");
  }

  // Every output of every recipe must appear in the row of that item, and the
  // edge weight must be the amount the recipe produces of it.
  auto seen = aw::vector<uint32_t>::zeroes(graph.nRecipe);
  for (size_t item = 0; item < graph.nItem; ++item) {
    auto targets = graph.producersOf(item);
    auto weights = graph.producedAmountsOf(item);
    for (size_t k = 0; k < targets.size(); ++k) {
      const uint32_t recipe = targets[k];
      if (recipe >= graph.nRecipe) {
        report("i2r points at an out-of-range recipe");
        continue;
      }
      seen[recipe]++;
      if (graph.producedAmountOf(recipe, item) != weights[k]) {
        report("recipe output amount disagrees with the edge weight");
      }
    }
  }
  for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
    if (seen[recipe] == 0)
      report("recipe is not reachable from any item");
    else if (seen[recipe] != (uint32_t) graph.outputsOf(recipe).size())
      report("a recipe has a different number of producer edges than outputs");
  }

  // Check that there's no duplication.
  {
    aw::vector<uint32_t> order(graph.nRecipe);
    for (uint32_t r = 0; r < graph.nRecipe; ++r)
      order[r] = r;

    auto keyLess = [&graph](uint32_t a, uint32_t b) {
      if (graph.output[a] != graph.output[b])
        return graph.output[a] < graph.output[b];
      if (graph.outputAmt[a] != graph.outputAmt[b])
        return graph.outputAmt[a] < graph.outputAmt[b];
      const auto oa = graph.outputsOf(a);
      const auto ob = graph.outputsOf(b);
      const auto owa = graph.outputAmountsOf(a);
      const auto owb = graph.outputAmountsOf(b);
      const size_t sharedOut = oa.size() < ob.size() ? oa.size() : ob.size();
      for (size_t i = 0; i < sharedOut; ++i) {
        if (oa[i] != ob[i])
          return oa[i] < ob[i];
        if (owa[i] != owb[i])
          return owa[i] < owb[i];
      }
      if (oa.size() != ob.size())
        return oa.size() < ob.size();
      const auto ta = graph.inputsOf(a);
      const auto tb = graph.inputsOf(b);
      const auto wa = graph.inputAmountsOf(a);
      const auto wb = graph.inputAmountsOf(b);
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
    auto targets = graph.producersOf(item);
    if (targets.empty()) continue;
    std::cout << "  item " << graph.itemHandle(item) << ":";
    auto weights = graph.producedAmountsOf(item);
    for (size_t k = 0; k < targets.size(); ++k) {
      std::cout << " r" << targets[k] << " x" << weights[k];
    }
    std::cout << '\n';
  }

  std::cout << "recipe -> item\n";
  for (size_t recipe = 0; recipe < graph.nRecipe; ++recipe) {
    std::cout << "  r" << recipe << " -> item " << graph.itemHandle(graph.output[recipe])
          << " x" << graph.outputAmt[recipe] << " (cost " << graph.cost[recipe] << ")";
    // Every other output, so a catalyst recipe that returns an input it also
    // consumes is visible as one.
    for (size_t k = 0; k < graph.outputsOf(recipe).size(); k++) {
      const aw::ItemId item = graph.outputsOf(recipe)[k];
      if (item == graph.output[recipe])
        continue;
      std::cout << " + item " << graph.itemHandle(item) << " x"
                << graph.outputAmountsOf(recipe)[k];
    }
    std::cout << " <-";
    auto targets = graph.inputsOf(recipe);
    auto weights = graph.inputAmountsOf(recipe);
    for (size_t k = 0; k < targets.size(); ++k) {
      std::cout << " item " << graph.itemHandle(targets[k]) << " x" << weights[k];
    }
    std::cout << '\n';
  }
}

// Prints every recipe of `sub` as "output xN <- input xN, ... @ ws1, ws2", plus
// a few stats. Pseudo-resources are shown as #handle. Item nodes are renumbered
// in the subgraph, so they are mapped back through itemOrigin before labelling.
// The workstation set is looked up in the source graph through recipeOrigin;
// tag recipes have none and simply omit the " @ ..." suffix.
void dumpSubgraph(const aw::Subgraph &sub, const aw::CraftingGraph &graph,
                  const NameTable &names, bool dumpWorkstation) {
  const aw::BaseCraftingGraph &g = sub.graph;

  auto item = [&](aw::ItemId node) {
    return itemLabel(graph, names, sub.itemOrigin[node]);
  };

  size_t leaves = 0;
  size_t multi = 0;
  size_t bulk = 0;
  aw::vector<aw::ItemId> leafItems;
  leafItems.reserve(g.nItem);
  for (aw::ItemId i = 0; i < g.nItem; i++) {
    const size_t producers = g.producersOf(i).size();
    if (producers == 0) {
      leaves++;
      leafItems.push_back_unchecked(i);
    }
    if (producers > 1)
      multi++;
  }
  for (uint32_t r = 0; r < g.nRecipe; r++) {
    if (g.outputAmt[r] > 1)
      bulk++;
  }

  size_t noWorkstation = 0;
  // Real recipes are a prefix, so the scan stops at the first tag edge.
  for (uint32_t r = 0; r < graph.nRecipe && graph.output[r] < graph.nReal; ++r) {
    if (graph.workstations.targetsOf(r).empty())
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
    // The cost is how many underlying recipe executions one execution of this
    // column is, so a batched chanced level shows its batch size here.
    std::cout << item(g.output[r]) << " x" << g.outputAmt[r] << " (cost " << g.cost[r]
              << ")";
    // The other outputs, which a plain "a <- b" reading would hide: a recipe
    // that consumes an item it also outputs is a catalyst, and the display has
    // to say so or the plan looks impossible.
    for (size_t k = 0; k < g.outputsOf(r).size(); k++) {
      const aw::ItemId out = g.outputsOf(r)[k];
      if (out == g.output[r])
        continue;
      std::cout << " + " << item(out) << " x" << g.outputAmountsOf(r)[k];
    }
    std::cout << " <- ";
    const auto targets = g.inputsOf(r);
    const auto weights = g.inputAmountsOf(r);
    if (targets.empty()) {
      std::cout << "(nothing)";
    } else {
      for (size_t k = 0; k < targets.size(); k++) {
        if (k != 0)
          std::cout << ", ";
        std::cout << item(targets[k]) << " x" << weights[k];
      }
    }
    if (r < sub.recipeOrigin.size() && dumpWorkstation) {
      const auto stations = graph.workstations.targetsOf(sub.recipeOrigin[r]);
      if (!stations.empty()) {
        std::cout << " @ ";
        for (size_t k = 0; k < stations.size(); k++) {
          if (k != 0)
            std::cout << ", ";
          std::cout << itemLabel(graph, names, stations[k]);
        }
      }
    }
    std::cout << '\n';
  }

  std::cout << "#\n# leaf items:\n";
  for (aw::ItemId i : leafItems)
    std::cout << "#   " << item(i) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string namesPath;
  std::string treeArg;
  std::string planArg;
  std::string invArg;
  std::string invFilePath;
  std::string profilePath;
  std::string inlineTags = "off";
  ProfileGuard profileGuard;
  double packSeconds = -1.0;
  double satelliteSeconds = -1.0;
  double variantSeconds = -1.0;
  double tagExclusiveSeconds = -1.0;
  double variantFoldSeconds = -1.0;
  uint64_t planAmount = 1;
  aw::solver::Options solverOptions;
  aw::Handle reach = 0;
  aw::vector<aw::Handle> workstations;

  bool check = false;
  bool dump = false;
  bool noPrune = false;
  bool noTagPrune = false;
  bool noRecipePrune = false;
  bool noDirectPrune = false;
  bool noSubstitutionPrune = false;
  bool noPackPrune = false;
  bool noSatellitePrune = false;
  bool noVariantPrune = false;
  bool noTagExclusivePrune = false;
  bool noVariantFoldPrune = false;
  bool noSeedPrune = false;
  bool noTagTidy = false;
  bool optimalPruning = false;
  bool doReach = false;
  bool doTree = false;
  bool doPlan = false;
  bool dumpWorkstation = false;
  bool wsSampleRequested = false;
  double wsFraction = 1.0;
  uint64_t wsSeed = awtools::kDefaultWorkstationSeed;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--check") {
      check = true;
    } else if (arg == "--dump") {
      dump = true;
    } else if (arg == "--no-prune") {
      noPrune = true;
    } else if (arg == "--no-tag-prune") {
      noTagPrune = true;
    } else if (arg == "--no-recipe-prune") {
      noRecipePrune = true;
    } else if (arg == "--no-direct-prune") {
      noDirectPrune = true;
    } else if (arg == "--no-substitution-prune") {
      noSubstitutionPrune = true;
    } else if (arg == "--no-pack-prune") {
      noPackPrune = true;
    } else if (arg == "--no-satellite-prune") {
      noSatellitePrune = true;
    } else if (arg == "--no-variant-prune") {
      noVariantPrune = true;
    } else if (arg == "--no-tag-exclusive-prune") {
      noTagExclusivePrune = true;
    } else if (arg == "--no-variant-fold-prune") {
      noVariantFoldPrune = true;
    } else if (arg == "--no-seed-prune") {
      noSeedPrune = true;
    } else if (arg == "--no-tag-tidy") {
      noTagTidy = true;
    } else if (arg == "--optimal") {
      optimalPruning = true;
    } else if (arg == "--dump-ws") {
      dumpWorkstation = true;
    } else if (arg == "--flash") {
      aw::options.flash = true;
    } else if (arg == "--no-flash-probe") {
      // Turn off flash mode's early greedy probe, for measuring what it buys.
      aw::options.flashProbe = false;
    } else if (arg == "--no-cycle-retries") {
      // Turn off the post-rejection re-solves, for measuring their cost.
      solverOptions.maxCycleRetries = 0;
    } else if (arg == "--no-startup-cuts") {
      // Turn off the eager 1- and 2-cycle entry cuts, for measuring what they
      // save against the rejected-solve retry path.
      solverOptions.maxStartupGroups = 0;
    } else if (arg == "--satellite-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], satelliteSeconds)) {
        std::cerr << "--satellite-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--variant-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], variantSeconds)) {
        std::cerr << "--variant-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--tag-exclusive-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], tagExclusiveSeconds)) {
        std::cerr << "--tag-exclusive-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--variant-fold-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], variantFoldSeconds)) {
        std::cerr << "--variant-fold-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--pack-seconds") {
      if (i + 1 >= argc || !parseDouble(argv[++i], packSeconds)) {
        std::cerr << "--pack-seconds needs a number of seconds\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--inline-tags") {
      if (i + 1 >= argc) {
        std::cerr << "--inline-tags needs off, pre, post or both\n";
        return EXIT_FAILURE;
      }
      inlineTags = argv[++i];
      if (inlineTags != "off" && inlineTags != "pre" && inlineTags != "post" &&
          inlineTags != "both") {
        std::cerr << "--inline-tags needs off, pre, post or both\n";
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
    } else if (arg == "--inv-file") {
      // The same inventory, read from a plan stock table (handle<TAB>amount).
      // The inline --inv cannot carry recipes-atm's random20 set: one argv
      // string is capped at MAX_ARG_STRLEN (128 KiB) and that one is 134 KiB.
      if (i + 1 >= argc) {
        std::cerr << "--inv-file needs a path\n";
        return EXIT_FAILURE;
      }
      invFilePath = argv[++i];
    } else if (arg == "--ws") {
      if (i + 1 >= argc || !parseHandleList(argv[++i], workstations)) {
        std::cerr << "--ws needs a comma-separated list of resource handles\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "--ws-percent") {
      if (i + 1 >= argc) {
        std::cerr << "--ws-percent needs a value in [0, 100]\n";
        return EXIT_FAILURE;
      }
      const double percent = std::strtod(argv[++i], nullptr);
      if (!(percent >= 0.0 && percent <= 100.0)) {
        std::cerr << "--ws-percent needs a value in [0, 100]\n";
        return EXIT_FAILURE;
      }
      wsFraction = percent / 100.0;
      wsSampleRequested = percent < 100.0;
    } else if (arg == "--ws-seed") {
      uint64_t seed = 0;
      if (i + 1 >= argc || !parseU64(argv[++i], seed)) {
        std::cerr << "--ws-seed needs a non-negative integer\n";
        return EXIT_FAILURE;
      }
      wsSeed = seed;
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "usage: awr_inspect [--check] [--dump] [--reach <handle>] [--ws <handle,...>]\n"
                   "                   [--ws-percent <0..100>] [--ws-seed <n>]\n"
                   "                   [--no-prune] [--no-recipe-prune] [--no-direct-prune] [--no-substitution-prune]\n"
                   "                   [--no-pack-prune]\n"
                   "                   [--no-satellite-prune] [--satellite-seconds <s>]\n"
                   "                   [--no-variant-prune] [--variant-seconds <s>]\n"
                   "                   [--no-tag-exclusive-prune] [--tag-exclusive-seconds <s>]\n"
                   "                   [--no-variant-fold-prune] [--variant-fold-seconds <s>]\n"
                   "                   [--no-seed-prune] [--no-tag-tidy] [--optimal] [--flash] [--no-flash-probe]\n"
                   "                   [--no-cycle-retries]\n"
                   "                   [--pack-seconds <s>] [--inline-tags off|pre|post|both]\n"
                   "                   [--plan <name|handle>] [--amount <n>] [--inv <h=a,...>]\n"
                   "                   [--inv-file <stock.tsv>]\n"
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
                 "                   [--ws-percent <0..100>] [--ws-seed <n>]\n"
                 "                   [--no-prune] [--no-recipe-prune] [--no-direct-prune] [--no-substitution-prune]\n"
                 "                   [--no-pack-prune]\n"
                 "                   [--no-satellite-prune] [--satellite-seconds <s>]\n"
                 "                   [--no-variant-prune] [--variant-seconds <s>]\n"
                 "                   [--no-tag-exclusive-prune] [--tag-exclusive-seconds <s>]\n"
                 "                   [--no-seed-prune] [--no-tag-tidy] [--optimal] [--flash] [--no-flash-probe]\n"
                 "                   [--no-cycle-retries]\n"
                 "                   [--pack-seconds <s>] [--inline-tags off|pre|post|both]\n"
                 "                   [--plan <name|handle>] [--amount <n>] [--inv <h=a,...>]\n"
                 "                   [--inv-file <stock.tsv>]\n"
                 "                   [--time-limit <s>] [--gap <f>] [--workers <n>] [--ub <n>]\n"
                 "                   [--names <table.tsv>] [--subgraph <name|handle>] [--dump-ws]\n"
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

  const aw::vector<std::byte> bytes = readFile(path);

  // Single-use tag inlining is read at registration time.
  if (inlineTags == "pre")
    aw::options.tagInlining = aw::TagInlineMode::PRE_PRUNE;
  else if (inlineTags == "post")
    aw::options.tagInlining = aw::TagInlineMode::QUERY_TIME;
  else if (inlineTags == "both")
    aw::options.tagInlining = aw::TagInlineMode::BOTH;
  else
    aw::options.tagInlining = aw::TagInlineMode::OFF;

  // Nonoptimal mode is on by default.
  aw::options.nonoptimal = !optimalPruning;
  // Prune profiling is on by default.
#ifdef AW_PROFILE_PRUNING
  aw::options.outputPruningProfile = true;
  aw::options.outputRepruningProfile = true;
#endif

  // The certificate pass runs at registration time, so its options have to be
  // installed before the graph is registered.
  {
    aw::PackPruneOptions packOptions = aw::options.pack;
    if (noPackPrune || noPrune)
      packOptions.enabled = false;
    if (packSeconds >= 0.0)
      packOptions.maxSeconds = packSeconds;
    aw::options.pack = packOptions;
  }

  // Satellite elimination runs per query, on the reachable subgraph.
  {
    aw::SatellitePruneOptions satelliteOptions = aw::options.satellite;
    if (noSatellitePrune || noPrune)
      satelliteOptions.enabled = false;
    if (satelliteSeconds >= 0.0)
      satelliteOptions.maxSeconds = satelliteSeconds;
    aw::options.satellite = satelliteOptions;
  }

  // Interchangeable-variant elimination runs per query as well.
  {
    aw::VariantClassPruneOptions variantOptions = aw::options.variantClass;
    if (noVariantPrune || noPrune)
      variantOptions.enabled = false;
    if (variantSeconds >= 0.0)
      variantOptions.maxSeconds = variantSeconds;
    aw::options.variantClass = variantOptions;
  }

  // Tag-exclusive producer elimination runs per query as well.
  {
    aw::TagExclusivePruneOptions exclusiveOptions = aw::options.tagExclusive;
    if (noTagExclusivePrune || noPrune)
      exclusiveOptions.enabled = false;
    if (tagExclusiveSeconds >= 0.0)
      exclusiveOptions.maxSeconds = tagExclusiveSeconds;
    aw::options.tagExclusive = exclusiveOptions;
  }

  // Variant folding runs per query as well.
  {
    aw::VariantFoldPruneOptions foldOptions = aw::options.variantFold;
    if (noVariantFoldPrune || noPrune)
      foldOptions.enabled = false;
    if (variantFoldSeconds >= 0.0)
      foldOptions.maxSeconds = variantFoldSeconds;
    aw::options.variantFold = foldOptions;
  }

  // Seed pruning runs per query, after the walk and every pruning pass.
  aw::options.seedPruning = !noSeedPrune && !noPrune;

  // Tag tidying runs per query, after the solve.
  aw::options.tagTidy = !noTagTidy;

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
  std::fprintf(stderr, "[time] register+prune: %.3f s\n", aw::since(registerStart));
  if (const char *error = aw::getCraftingError()) {
    std::cout << "malformed graph: " << error << "\n";
    return EXIT_SUCCESS;
  }
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  if (noTagPrune || noPrune)
    aw::options.tagPruning = false;
  if (noRecipePrune || noPrune)
    aw::options.recipePruning = false;
  if (noDirectPrune || noPrune)
    aw::options.directPruning = false;
  if (noSubstitutionPrune || noPrune)
    aw::options.substitutionPruning = false;

  std::cout << "parsed " << bytes.size() << " bytes from " << path << '\n';
  printSummary(graph);

  // Workstation availability for --tree / --reach / --plan. An explicit --ws
  // list wins; otherwise --ws-percent samples the non-vanilla pool (keeping
  // every `minecraft:` station on); the default is every real handle.
  awtools::WorkstationSample wsSample;
  bool wsSampled = false;
  aw::vector<aw::Handle> sampledStations;
  aw::vector<aw::Handle> allHandles;
  if (wsSampleRequested && workstations.empty()) {
    const std::string tablePath = namesPath.empty() ? defaultNamesPath(path) : namesPath;
    aw::vector<std::string> resources;
    if (!awtools::loadResourceLocations(tablePath, graph.nReal, resources)) {
      std::cerr << "cannot read name table " << tablePath << " (needed for --ws-percent)\n";
      return EXIT_FAILURE;
    }
    wsSample = awtools::sampleWorkstations(graph, resources, wsFraction, wsSeed);
    sampledStations = wsSample.stations;
    wsSampled = true;
    std::cout << "workstations: " << wsFraction * 100.0 << "% -> " << wsSample.vanilla
              << " vanilla + " << wsSample.sampled << "/" << wsSample.nonVanilla
              << " non-vanilla = " << wsSample.total << " stations\n";
  } else if (wsSampleRequested) {
    std::cerr << "note: --ws-percent ignored because --ws was given\n";
  }
  if (!wsSampled && workstations.empty()) {
    allHandles.reserve(graph.nReal);
    for (aw::Handle h = 1; h <= graph.nReal; h++)
      allHandles.push_back_unchecked(h);
  }
  // The set every mode falls back to: an explicit --ws list, then the
  // percentage draw, then every real handle.
  const aw::vector<aw::Handle> &engineStations =
      !workstations.empty() ? workstations
                            : (wsSampled ? sampledStations : allHandles);
  auto stationLabel = [&]() -> std::string {
    if (!workstations.empty())
      return std::to_string(workstations.size()) + " workstations";
    if (wsSampled)
      return std::to_string(sampledStations.size()) + " sampled workstations";
    return "all workstations";
  };

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

    // "All workstations present" by default; --ws-percent narrows the set.
    const aw::Subgraph sub = aw::reachableSubgraph(target, engineStations);
    if (sub.graph.nItem == 0) {
      std::cerr << "no subgraph reachable from " << treeArg << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "subgraph from " << itemLabel(graph, names, target - 1) << " ("
              << stationLabel() << "):\n";
    dumpSubgraph(sub, graph, names, dumpWorkstation);
  }
  if (doReach) {
    aw::Subgraph sub = aw::reachableSubgraph(reach, engineStations);
    if (sub.graph.nItem == 0) {
      std::cout << "reachable from handle " << reach << ": invalid output handle\n";
    } else {
      std::cout << "reachable from handle " << reach << " with " << engineStations.size()
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

    const bool allStations = workstations.empty() && !wsSampled;
    const aw::vector<aw::Handle> &stations = engineStations;

    // Inventory is given per handle and stored per source item node. It is built
    // before the reachability pass so the dominated tag edges can consult it.
    auto inventory = aw::vector<aw::Amount>::zeroes(graph.nItem);
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
    if (!invFilePath.empty()) {
      std::ifstream in(invFilePath);
      if (!in) {
        std::cerr << "cannot read --inv-file " << invFilePath << '\n';
        return EXIT_FAILURE;
      }
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty() || line[0] == '#')
          continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos)
          continue;
        aw::Handle handle = 0;
        uint64_t amount = 0;
        if (!parseHandle(line.substr(0, tab), handle) ||
            !parseU64(line.substr(tab + 1), amount) || handle == 0 ||
            handle > graph.nItem) {
          std::cerr << "bad --inv-file entry: " << line << '\n';
          return EXIT_FAILURE;
        }
        inventory[aw::CraftingGraph::itemNode(handle)] += (aw::Amount) amount;
      }
    }

    const auto reachStart = std::chrono::steady_clock::now();
    const aw::Subgraph sub =
        aw::reachableSubgraph(target, stations, inventory, planAmount);
    std::fprintf(stderr, "[time] reachableSubgraph: %.3f s\n", aw::since(reachStart));
    if (sub.graph.nItem == 0) {
      std::cerr << "no subgraph reachable from " << planArg << '\n';
      return EXIT_FAILURE;
    }

    std::cout << "plan for " << itemLabel(graph, names, target - 1) << " x" << planAmount;
    std::cout << (allStations ? " (all workstations)"
                              : " (" + std::to_string(stations.size()) +
                                    (wsSampled ? " sampled workstations)" : " workstations)"));
    std::cout << ":\n";
    std::cout << "  subgraph: " << sub.graph.nItem << " items, " << sub.graph.nRecipe
              << " recipes\n";

    const aw::ItemId targetNode = sub.translate(aw::CraftingGraph::itemNode(target));
    const auto planStart = std::chrono::steady_clock::now();
    const aw::PlanResult plan =
        aw::planCrafting(sub, targetNode, planAmount, inventory, solverOptions);
    std::fprintf(stderr, "[time] planCrafting(solve): %.3f s\n", aw::since(planStart));

    const char *statusName = "?";
    switch (plan.status) {
      case aw::PlanStatus::OK: statusName = "ok"; break;
      case aw::PlanStatus::INFEASIBLE: statusName = "infeasible"; break;
      case aw::PlanStatus::NUMERICAL_FAIL: statusName = "numerical failure"; break;
      case aw::PlanStatus::ITER_LIMIT: statusName = "iteration limit"; break;
      case aw::PlanStatus::INVALID_INPUT: statusName = "invalid input"; break;
      case aw::PlanStatus::CYCLE_UNFULFILLED: statusName = "cycle unfulfilled"; break;
    }
    std::cout << "  status: " << statusName;
    if (plan.status == aw::PlanStatus::OK)
      std::cout << (plan.provenOptimal ? " (proven optimal)" : "")
                << ", gap=" << plan.gap << ", bound=" << plan.bestBound;
    std::cout << ", conflicts=" << plan.numConflicts
              << ", branches=" << plan.numBranches
              << ", fixed=" << plan.fixedColumns << '\n';

    if (plan.status == aw::PlanStatus::OK || plan.status == aw::PlanStatus::CYCLE_UNFULFILLED) {
      int64_t totalExec = 0;
      int64_t realExec = 0;
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
        totalExec += plan.exec[r];
        if (sub.graph.output[r] < sub.graph.nReal)
          realExec += plan.exec[r];
      }

      std::cout << "  exec: " << totalExec
                << ", real: " << realExec;
      // The objective, which is what the planner minimises and what a chanced
      // batch level is charged for: one execution of a cost-8 column is eight
      // real executions. The raw counts above say nothing about it.
      if (!plan.exec.empty()) {
        aw::int128 cost = 0;
        for (uint32_t r = 0; r < sub.graph.nRecipe; r++)
          if (sub.graph.output[r] < sub.graph.nReal)
            cost += (aw::int128) sub.graph.cost[r] * (aw::int128) plan.exec[r];
        std::cout << ", cost: " << (long long) (cost > (aw::int128) INT64_MAX ? INT64_MAX : cost);
      }
      std::cout << '\n';
      // A rejected plan is the interesting case: say which recipe could not
      // start and what it was missing, or the status reads like a mystery.
      if (plan.status == aw::PlanStatus::CYCLE_UNFULFILLED) {
        aw::FireabilityWitness witness;
        aw::planIsFireable(sub, inventory,
                           std::span<const int64_t>(plan.exec.data(), plan.exec.size()),
                           &witness);
        for (size_t k = 0; k < witness.recipe.size(); k++) {
          const uint32_t r = witness.recipe[k];
          const aw::ItemId item = witness.item[k];
          const aw::Amount stock = sub.itemOrigin[item] < inventory.size()
                                       ? inventory[sub.itemOrigin[item]]
                                       : 0;
          std::cout << "  cannot start " << itemLabel(graph, names, sub.itemOrigin[sub.graph.output[r]])
                    << " <- " << itemLabel(graph, names, sub.itemOrigin[item]) << " x"
                    << witness.need[k] << ": only " << stock
                    << " in stock";
          std::cout << '\n';
        }
      }
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
        const int64_t count = plan.exec[r];
        if (count == 0)
          continue;
        std::cout << "  " << count << " x "
                  << itemLabel(graph, names, sub.itemOrigin[sub.graph.output[r]]) << " x"
                  << sub.graph.outputAmt[r];
        // Every other output, so a catalyst recipe -- one that returns an item
        // it also consumes -- is not mistaken for an impossible one.
        for (size_t k = 0; k < sub.graph.outputsOf(r).size(); k++) {
          const aw::ItemId out = sub.graph.outputsOf(r)[k];
          if (out == sub.graph.output[r])
            continue;
          std::cout << " + " << itemLabel(graph, names, sub.itemOrigin[out]) << " x"
                    << sub.graph.outputAmountsOf(r)[k];
        }
        std::cout << " <- ";
        const auto inputs = sub.graph.inputsOf(r);
        const auto weights = sub.graph.inputAmountsOf(r);
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
