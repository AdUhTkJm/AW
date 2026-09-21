// awr_inspect -- decodes a .awr dump, prints a summary, and optionally
// validates the CSR invariants or dump every adjacency row. Built for
// the WSL side, where running Minecraft is inconvenient.
//
// Note that -fno-exception is also enabled for this file.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "aw/CraftingGraph.h"

namespace {

#define fail(msg, ...) { std::cerr << (msg) << "\n"; return __VA_ARGS__; }

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
bool parseHandle(const std::string &text, aw::Handle &out) {
  if (text.empty())
    return false;
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    value = value * 10 + (uint64_t) (c - '0');
    if (value > UINT32_MAX)
      return false;
  }
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

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool check = false;
  bool dump = false;
  bool doReach = false;
  aw::Handle reach = 0;
  std::vector<aw::Handle> workstations;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--check") {
      check = true;
    } else if (arg == "--dump") {
      dump = true;
    } else if (arg == "--reach") {
      if (i + 1 >= argc || !parseHandle(argv[++i], reach) || reach == 0) {
        std::cerr << "--reach needs a resource handle\n";
        return EXIT_FAILURE;
      }
      doReach = true;
    } else if (arg == "--ws") {
      if (i + 1 >= argc || !parseHandleList(argv[++i], workstations)) {
        std::cerr << "--ws needs a comma-separated list of resource handles\n";
        return EXIT_FAILURE;
      }
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "usage: awr_inspect [--check] [--dump] [--reach <handle>] [--ws <handle,...>] <recipes.awr>\n";
      return EXIT_SUCCESS;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::cerr << "unexpected argument: " << arg << '\n';
      return EXIT_FAILURE;
    }
  }

  if (path.empty()) {
    std::cerr << "usage: awr_inspect [--check] [--dump] [--reach <handle>] [--ws <handle,...>] <recipes.awr>\n";
    return EXIT_FAILURE;
  }

  const std::vector<std::byte> bytes = readFile(path);
  aw::registerCraftingGraph(bytes);
  if (const char *error = aw::getCraftingError()) {
    std::cout << "malformed graph: " << error << "\n";
    return EXIT_SUCCESS;
  }
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  std::cout << "parsed " << bytes.size() << " bytes from " << path << '\n';
  printSummary(graph);
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
  if (dump)
    dumpGraph(graph);
  if (check) {
    const size_t problems = checkGraph(graph);
    std::cout << (problems == 0 ? "checks: OK\n" : "checks: FAILED\n");
    return problems == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
