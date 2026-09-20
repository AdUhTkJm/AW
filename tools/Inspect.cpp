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
  std::cout << "item -> recipe   : " << itemEdges << " edges\n";
  std::cout << "recipe -> item   : " << inputEdges << " edges\n";
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

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--check") {
      check = true;
    } else if (arg == "--dump") {
      dump = true;
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "usage: awr_inspect [--check] [--dump] <recipes.awr>\n";
      return EXIT_SUCCESS;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::cerr << "unexpected argument: " << arg << '\n';
      return EXIT_FAILURE;
    }
  }

  if (path.empty()) {
    std::cerr << "usage: awr_inspect [--check] [--dump] <recipes.awr>\n";
    return EXIT_FAILURE;
  }

  const std::vector<std::byte> bytes = readFile(path);
  aw::registerCraftingGraph(bytes);
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  std::cout << "parsed " << bytes.size() << " bytes from " << path << '\n';
  printSummary(graph);
  if (dump)
    dumpGraph(graph);
  if (check) {
    const size_t problems = checkGraph(graph);
    std::cout << (problems == 0 ? "checks: OK\n" : "checks: FAILED\n");
    return problems == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
