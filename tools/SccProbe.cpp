// aw_scc -- how much of a strongly connected component survives the pruning?
//
// The crafting graph is a bipartite digraph (items <-> recipes) and the AW docs
// observe that, without pruning, the reachable subgraph of a typical item is
// almost one SCC. This tool measures the same quantity *after* every pruning
// pass, by going through the real `reachableSubgraph` entry point rather than a
// Python re-implementation, so the answer is exactly what the planner sees.
//
// One process registers the graph once and then sweeps:
//
//   dataset x pruning stage x target
//
// A "stage" is a selection of the seven query-time gates (the registration-time
// marks are always computed, exactly as aw_bench does it), so the `none` stage
// is the canonicalized-but-unpruned graph and `all` is the shipped default.
//
// Output is TSV on stdout, one row per (stage, target), plus `#` comment rows
// for the registration summary. Empty inventory is the default: that is the
// smallest subgraph the query-time guards can produce, so a large SCC there is
// the strongest possible statement. Every workstation is available by default,
// which is the historical worst case; `--ws-percent` instead keeps all vanilla
// (`minecraft:`) stations and samples the non-vanilla pool (see
// WorkstationSample.h and bench/README.md).
//
// Note that -fno-exceptions is enabled for this file, as for the other tools.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "aw/plan/Options.h"
#include "aw/plan/CraftingGraph.h"

#include "WorkstationSample.h"

namespace {

using aw::Amount;
using aw::BaseCraftingGraph;
using aw::Handle;
using aw::ItemId;
using aw::RecipeId;

struct Target {
  Handle handle = 0;
  std::string name;
};

// ------------------------------------------------------------------ TSV input

bool readStock(const std::string &path, aw::vector<std::pair<Handle, Amount>> &out) {
  std::ifstream in(path);
  if (!in)
    return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    char *end = nullptr;
    const long long handle = std::strtoll(line.c_str(), &end, 10);
    const long long amount = std::strtoll(line.c_str() + tab + 1, &end, 10);
    if (handle <= 0 || amount < 0)
      continue;
    out.push_back({(Handle) handle, (Amount) amount});
  }
  return true;
}

bool readTargets(const std::string &path, aw::vector<Target> &out) {
  std::ifstream in(path);
  if (!in)
    return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    const std::string handleText = line.substr(0, tab);
    char *end = nullptr;
    const long long handle = std::strtoll(handleText.c_str(), &end, 10);
    if (end == handleText.c_str() || *end != '\0' || handle <= 0)
      continue;
    const size_t nameEnd = line.find('\t', tab + 1);
    Target target;
    target.handle = (Handle) handle;
    target.name = line.substr(tab + 1, nameEnd == std::string::npos
                                           ? std::string::npos
                                           : nameEnd - tab - 1);
    out.push_back(std::move(target));
  }
  return true;
}

// -------------------------------------------------------------------- stages

// A point on the cumulative ablation curve, matching the ordering agreed in
// bench/README.md and tools/Bench.cpp.
struct Stage {
  const char *name;
  bool seed;
  bool direct;
  bool recipe;
  bool subs;
  bool tag;
  bool pack;
  bool sat;
};

const Stage kStages[] = {
    {"none", false, false, false, false, false, false, false},
    {"seed", true, false, false, false, false, false, false},
    {"direct", true, true, false, false, false, false, false},
    {"recipe", true, true, true, false, false, false, false},
    {"substitution", true, true, true, true, false, false, false},
    {"tag", true, true, true, true, true, false, false},
    {"pack", true, true, true, true, true, true, false},
    {"satellite", true, true, true, true, true, true, true},
};

const Stage kAll = {"all", true, true, true, true, true, true, true};

const Stage *findStage(const std::string &name) {
  if (name == "all")
    return &kAll;
  for (const Stage &stage : kStages)
    if (name == stage.name)
      return &stage;
  return nullptr;
}

void applyStage(const Stage &stage) {
  aw::options.seedPruning = stage.seed;
  aw::options.directPruning = stage.direct;
  aw::options.recipePruning = stage.recipe;
  aw::options.substitutionPruning = stage.subs;
  aw::options.tagPruning = stage.tag;
  aw::options.pack.enabled = stage.pack;
  aw::options.satellite.enabled = stage.sat;
}

// ----------------------------------------------------------------- SCC stats

struct SccStats {
  uint32_t components = 0;
  uint32_t largest = 0;
  uint32_t largestItems = 0;
  uint32_t largestRecipes = 0;
  uint32_t largestComp = 0;
};

// Fringe structure relative to the giant SCC: how much of the non-giant part is
// a foldable DAG (all singleton SCCs, no producer choice) versus small cycles
// or branching choice points that would still need solving.
struct Decompose {
  uint32_t bigNodes = 0, bigItems = 0, bigRecipes = 0;
  uint32_t fringeNodes = 0, fringeItems = 0, fringeRecipes = 0;
  uint32_t fringeComps = 0, fringeCycleComps = 0, fringeMaxScc = 0;
  // Dependency orientation (item -> producing recipe -> input item): the target
  // is a source. `upper` items can reach the giant SCC (target side, demand is
  // known up front), `lower` items are reachable from it (raw side, resolved
  // after the core), `parallel` items touch neither. Those three partition the
  // fringe items.
  uint32_t upperItems = 0, lowerItems = 0, parallelItems = 0;
  // Fringe items with more than one producing recipe (a real choice) and with
  // none (a raw leaf). A fringe that is all forced singletons folds cleanly.
  uint32_t choiceItems = 0, leafItems = 0;
  // Synthetic tag edges (zero-cost, just pick a member) versus real recipes.
  uint32_t fringeTagRecipes = 0, bigTagRecipes = 0;
};

// Iterative Tarjan over the bipartite subgraph. The combined vertex id is the
// item id for an item and nItem + recipe for a recipe. The i2r rows carry plain
// recipe ids, so the shift is applied while filling the CSR below.
SccStats analyzeScc(const BaseCraftingGraph &g, aw::vector<int32_t> &comp) {
  SccStats stats;
  const uint32_t nItem = g.nItem;
  const uint32_t nRecipe = g.nRecipe;
  const uint32_t n = nItem + nRecipe;
  comp.clear();
  if (n == 0)
    return stats;
  comp.assign(n, -1);

  // CSR over all bipartite arcs.
  aw::vector<uint32_t> offsets(n + 1, 0);
  for (uint32_t item = 0; item < nItem; item++)
    offsets[item + 1] = (uint32_t) g.producersOf(item).size();
  for (uint32_t r = 0; r < nRecipe; r++)
    offsets[nItem + r + 1] = (uint32_t) g.inputsOf(r).size();
  for (uint32_t v = 0; v < n; v++)
    offsets[v + 1] += offsets[v];
  aw::vector<uint32_t> arcs(offsets[n]);
  for (uint32_t item = 0; item < nItem; item++) {
    uint32_t cursor = offsets[item];
    for (RecipeId to : g.producersOf(item))
      arcs[cursor++] = nItem + to;
  }
  for (uint32_t r = 0; r < nRecipe; r++) {
    uint32_t cursor = offsets[nItem + r];
    for (ItemId to : g.inputsOf(r))
      arcs[cursor++] = to;
  }

  aw::vector<int32_t> disc(n, -1), low(n, 0);
  aw::vector<uint8_t> onStack(n, 0);
  aw::vector<uint32_t> tarjanStack, callNode, callEdge;
  tarjanStack.reserve(n);
  callNode.reserve(n);
  callEdge.reserve(n);
  int32_t timer = 0;
  uint32_t nComp = 0;

  for (uint32_t s = 0; s < n; s++) {
    if (disc[s] != -1)
      continue;
    disc[s] = low[s] = timer++;
    tarjanStack.push_back_unchecked(s);
    onStack[s] = 1;
    callNode.push_back_unchecked(s);
    callEdge.push_back_unchecked(offsets[s]);
    while (!callNode.empty()) {
      const uint32_t v = callNode.back();
      uint32_t &edge = callEdge.back();
      if (edge < offsets[v + 1]) {
        const uint32_t u = arcs[edge++];
        if (disc[u] == -1) {
          disc[u] = low[u] = timer++;
          tarjanStack.push_back_unchecked(u);
          onStack[u] = 1;
          callNode.push_back_unchecked(u);
          callEdge.push_back_unchecked(offsets[u]);
        } else if (onStack[u] && disc[u] < low[v]) {
          low[v] = disc[u];
        }
      } else {
        if (low[v] == disc[v]) {
          while (true) {
            const uint32_t u = tarjanStack.back();
            tarjanStack.pop_back();
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

  aw::vector<uint32_t> size(nComp, 0), items(nComp, 0), recipes(nComp, 0);
  for (uint32_t v = 0; v < n; v++) {
    const uint32_t c = (uint32_t) comp[v];
    size[c]++;
    if (v < nItem)
      items[c]++;
    else
      recipes[c]++;
  }
  uint32_t best = 0;
  for (uint32_t c = 1; c < nComp; c++)
    if (size[c] > size[best])
      best = c;
  stats.components = nComp;
  stats.largest = size[best];
  stats.largestItems = items[best];
  stats.largestRecipes = recipes[best];
  stats.largestComp = best;
  return stats;
}

// Forward bipartite arcs of a subgraph as CSR.
void buildArcs(const BaseCraftingGraph &g, aw::vector<uint32_t> &offsets,
               aw::vector<uint32_t> &arcs) {
  const uint32_t nItem = g.nItem;
  const uint32_t nRecipe = g.nRecipe;
  const uint32_t n = nItem + nRecipe;
  offsets.assign(n + 1, 0);
  for (uint32_t item = 0; item < nItem; item++)
    offsets[item + 1] = (uint32_t) g.producersOf(item).size();
  for (uint32_t r = 0; r < nRecipe; r++)
    offsets[nItem + r + 1] = (uint32_t) g.inputsOf(r).size();
  for (uint32_t v = 0; v < n; v++)
    offsets[v + 1] += offsets[v];
  arcs.resize(offsets[n]);
  for (uint32_t item = 0; item < nItem; item++) {
    uint32_t cursor = offsets[item];
    for (RecipeId to : g.producersOf(item))
      arcs[cursor++] = nItem + to;
  }
  for (uint32_t r = 0; r < nRecipe; r++) {
    uint32_t cursor = offsets[nItem + r];
    for (ItemId to : g.inputsOf(r))
      arcs[cursor++] = to;
  }
}

// Marks everything reachable from `root` in the given CSR.
void markReachable(const aw::vector<uint32_t> &offsets, const aw::vector<uint32_t> &arcs,
                   uint32_t root, aw::vector<uint8_t> &mark) {
  mark.assign(offsets.size() - 1, 0);
  aw::vector<uint32_t> stack;
  stack.reserve(offsets.size());
  stack.push_back(root);
  mark[root] = 1;
  while (!stack.empty()) {
    const uint32_t v = stack.back();
    stack.pop_back();
    for (uint32_t e = offsets[v]; e < offsets[v + 1]; e++) {
      const uint32_t u = arcs[e];
      if (!mark[u]) {
        mark[u] = 1;
        stack.push_back(u);
      }
    }
  }
}

Decompose decompose(const BaseCraftingGraph &g, const aw::vector<int32_t> &comp,
                    uint32_t big) {
  Decompose d;
  const uint32_t nItem = g.nItem;
  const uint32_t nRecipe = g.nRecipe;
  const uint32_t n = nItem + nRecipe;
  if (n == 0)
    return d;

  uint32_t nComp = 0;
  for (uint32_t v = 0; v < n; v++)
    if ((uint32_t) comp[v] + 1 > nComp)
      nComp = (uint32_t) comp[v] + 1;
  aw::vector<uint32_t> size(nComp, 0), items(nComp, 0), recipes(nComp, 0);
  for (uint32_t v = 0; v < n; v++) {
    const uint32_t c = (uint32_t) comp[v];
    size[c]++;
    if (v < nItem)
      items[c]++;
    else
      recipes[c]++;
  }

  d.bigNodes = size[big];
  d.bigItems = items[big];
  d.bigRecipes = recipes[big];
  for (uint32_t c = 0; c < nComp; c++) {
    if (c == big)
      continue;
    d.fringeNodes += size[c];
    d.fringeItems += items[c];
    d.fringeRecipes += recipes[c];
    d.fringeComps++;
    if (size[c] > 1)
      d.fringeCycleComps++;
    if (size[c] > d.fringeMaxScc)
      d.fringeMaxScc = size[c];
  }

  // Forward and reverse reachability from the giant SCC. BFS takes a node, so
  // pick one representative of the big component (item node id == comp index
  // only by luck; the two spaces are unrelated).
  uint32_t root = 0;
  for (uint32_t v = 0; v < n; v++)
    if ((uint32_t) comp[v] == big) {
      root = v;
      break;
    }
  aw::vector<uint32_t> fwdOff, fwdArcs;
  buildArcs(g, fwdOff, fwdArcs);
  aw::vector<uint32_t> revOff(n + 1, 0);
  for (uint32_t e = 0; e < fwdArcs.size(); e++)
    revOff[fwdArcs[e] + 1]++;
  for (uint32_t v = 0; v < n; v++)
    revOff[v + 1] += revOff[v];
  aw::vector<uint32_t> revArcs(fwdArcs.size());
  {
    aw::vector<uint32_t> cursor(revOff.begin(), revOff.end() - 1);
    for (uint32_t v = 0; v < n; v++)
      for (uint32_t e = fwdOff[v]; e < fwdOff[v + 1]; e++)
        revArcs[cursor[fwdArcs[e]]++] = v;
  }
  aw::vector<uint8_t> fwdMark, revMark;
  markReachable(fwdOff, fwdArcs, root, fwdMark);
  markReachable(revOff, revArcs, root, revMark);

  for (uint32_t item = 0; item < nItem; item++) {
    if ((uint32_t) comp[item] == big)
      continue;
    if (revMark[item])
      d.upperItems++;
    else if (fwdMark[item])
      d.lowerItems++;
    else
      d.parallelItems++;
    const size_t producers = g.producersOf(item).size();
    if (producers > 1)
      d.choiceItems++;
    if (producers == 0)
      d.leafItems++;
  }
  for (uint32_t r = 0; r < nRecipe; r++) {
    if (g.output[r] < g.nReal)
      continue;
    if ((uint32_t) comp[nItem + r] == big)
      d.bigTagRecipes++;
    else
      d.fringeTagRecipes++;
  }
  return d;
}

// ------------------------------------------------------------------ helpers

bool readFile(const std::string &path, aw::vector<std::byte> &out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return false;
  const std::streamsize size = in.tellg();
  if (size < 0)
    return false;
  in.seekg(0, std::ios::beg);
  out.resize((uint32_t) size);
  return size == 0 || (bool) in.read((char *) out.data(), size);
}

void usage() {
  std::fprintf(stderr,
               "usage: aw_scc --awr <recipes.awr> --plan <prefix>\n"
               "              [--stages none,all,...] [--nonoptimal 0|1]\n"
               "              [--stock none|leaves|random20|<path>]\n"
               "              [--pack-seconds 60] [--satellite-seconds 0.05]\n"
               "              [--out <path>] [--decompose] [--quiet]\n"
               "\n"
               "--plan is the path prefix of a bench plan manifest; "
               "<prefix>.targets.tsv supplies\n"
               "the handles. stages: none seed direct recipe substitution tag pack "
               "satellite all\n"
               "\n"
               "--ws-percent <0..100> samples that percentage of the non-vanilla\n"
               "workstation pool (all `minecraft:` stations stay on); --names points at the\n"
               "`.names.tsv` table, defaulting to the one next to the .awr. --ws-seed sets\n"
               "the draw seed (default 20260101).\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::string awrPath;
  std::string planPrefix;
  std::string stagesArg;
  std::string stockArg;
  std::string outPath;
  std::string namesPath;
  bool nonoptimal = true;
  bool quiet = false;
  bool decomposeMode = false;
  bool sampleWorkstations = false;
  double wsFraction = 1.0;
  uint64_t wsSeed = awtools::kDefaultWorkstationSeed;
  double packSeconds = 60.0;
  double satelliteSeconds = 0.05;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    auto next = [&](std::string &into) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg.c_str());
        std::exit(EXIT_FAILURE);
      }
      into = argv[++i];
    };
    if (arg == "--awr")
      next(awrPath);
    else if (arg == "--plan")
      next(planPrefix);
    else if (arg == "--stages")
      next(stagesArg);
    else if (arg == "--stock")
      next(stockArg);
    else if (arg == "--out")
      next(outPath);
    else if (arg == "--names")
      next(namesPath);
    else if (arg == "--ws-percent") {
      std::string value;
      next(value);
      const double percent = std::strtod(value.c_str(), nullptr);
      if (!(percent >= 0.0 && percent <= 100.0)) {
        std::fprintf(stderr, "--ws-percent must be between 0 and 100\n");
        return EXIT_FAILURE;
      }
      wsFraction = percent / 100.0;
      sampleWorkstations = percent < 100.0;
    } else if (arg == "--ws-seed") {
      std::string value;
      next(value);
      wsSeed = std::strtoull(value.c_str(), nullptr, 10);
    } else if (arg == "--nonoptimal") {
      std::string value;
      next(value);
      nonoptimal = value != "0";
    } else if (arg == "--pack-seconds") {
      std::string value;
      next(value);
      packSeconds = std::strtod(value.c_str(), nullptr);
    } else if (arg == "--satellite-seconds") {
      std::string value;
      next(value);
      satelliteSeconds = std::strtod(value.c_str(), nullptr);
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--decompose") {
      decomposeMode = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return EXIT_SUCCESS;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      usage();
      return EXIT_FAILURE;
    }
  }

  if (awrPath.empty() || planPrefix.empty()) {
    usage();
    return EXIT_FAILURE;
  }

  aw::vector<Target> targets;
  if (!readTargets(planPrefix + ".targets.tsv", targets) || targets.empty()) {
    std::fprintf(stderr, "cannot read targets from %s.targets.tsv\n", planPrefix.c_str());
    return EXIT_FAILURE;
  }

  // Stage list.
  aw::vector<const Stage *> stages;
  if (stagesArg.empty()) {
    stages.push_back(&kStages[0]);  // none
    stages.push_back(&kAll);
  } else {
    size_t start = 0;
    while (start <= stagesArg.size()) {
      const size_t comma = stagesArg.find(',', start);
      const std::string piece =
          stagesArg.substr(start, comma == std::string::npos ? std::string::npos
                                                             : comma - start);
      const Stage *stage = findStage(piece);
      if (stage == nullptr) {
        std::fprintf(stderr, "unknown stage: %s\n", piece.c_str());
        return EXIT_FAILURE;
      }
      stages.push_back(stage);
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
  }

  aw::vector<std::byte> bytes;
  if (!readFile(awrPath, bytes)) {
    std::fprintf(stderr, "cannot read %s\n", awrPath.c_str());
    return EXIT_FAILURE;
  }

  // Registration-time switches. The pack pass has to be on for registration so
  // a later stage can apply the certificates; the stage only gates the query.
  aw::options.nonoptimal = nonoptimal;
  aw::options.tagInlining = aw::TagInlineMode::OFF;
  aw::options.pack.enabled = true;
  aw::PackPruneOptions pack = aw::options.pack;
  pack.maxSeconds = packSeconds;
  aw::options.pack = pack;
  aw::SatellitePruneOptions satellite = aw::options.satellite;
  satellite.maxSeconds = satelliteSeconds;
  aw::options.satellite = satellite;
  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.substitutionPruning = true;
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;

  aw::registerCraftingGraph(bytes);
  if (const char *error = aw::getCraftingError()) {
    std::fprintf(stderr, "malformed graph: %s\n", error);
    return EXIT_FAILURE;
  }
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // Every workstation available by default. With --ws-percent, keep every
  // vanilla (`minecraft:`) station and sample the rest from the names table.
  // The inventory is either a bench stock group (a `<prefix>.stock.<group>.tsv`
  // sidecar) or a path to a handle/amount TSV; by default it is empty.
  awtools::WorkstationSample sample;
  aw::vector<Handle> stations;
  if (sampleWorkstations) {
    const std::string tablePath =
        namesPath.empty() ? awtools::defaultNamesPath(awrPath) : namesPath;
    aw::vector<std::string> resources;
    if (!awtools::loadResourceLocations(tablePath, graph.nReal, resources)) {
      std::fprintf(stderr, "cannot read name table %s (needed for --ws-percent)\n",
                   tablePath.c_str());
      return EXIT_FAILURE;
    }
    sample = awtools::sampleWorkstations(graph, resources, wsFraction, wsSeed);
    stations = sample.stations;
  } else {
    stations.reserve(graph.nReal);
    for (Handle handle = 1; handle <= graph.nReal; handle++)
      stations.push_back_unchecked(handle);
  }

  std::FILE *out = stdout;
  if (!outPath.empty()) {
    out = std::fopen(outPath.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
      return EXIT_FAILURE;
    }
  }

  std::fprintf(out,
               "# dataset\titems\trecipes\treal\ttag_edges\ttag_dominated\t"
               "recipe_dominated\tdirect_dominated\tsubstituted\tpack_dominated\t"
               "nonoptimal\n");
  {
    size_t tagEdges = 0, tagDominated = 0, recipeDominated = 0, directDominated = 0;
    size_t substituted = 0, packDominated = 0;
    for (uint32_t r = 0; r < graph.nRecipe; r++) {
      if (graph.output[r] >= graph.nReal)
        ++tagEdges;
      if (r < graph.tagEdgeDominated.size() && graph.tagEdgeDominated[r])
        ++tagDominated;
      if (r < graph.recipeDominated.size() && graph.recipeDominated[r])
        ++recipeDominated;
      if (r < graph.recipeDirectDominated.size() && graph.recipeDirectDominated[r])
        ++directDominated;
      if (r < graph.recipeSubstituted.size() && graph.recipeSubstituted[r])
        ++substituted;
      if (r < graph.packDominated.size() && graph.packDominated[r])
        ++packDominated;
    }
    std::fprintf(out, "# registration\t%u\t%u\t%u\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%d\n",
                 graph.nItem, graph.nRecipe, graph.nReal, tagEdges, tagDominated,
                 recipeDominated, directDominated, substituted, packDominated,
                 (int) nonoptimal);
    if (sampleWorkstations) {
      std::fprintf(out, "# workstations\t%.1f\t%u\t%u\t%u\t%u\t%llu\n", wsFraction * 100.0,
                   sample.vanilla, sample.nonVanilla, sample.sampled, sample.total,
                   (unsigned long long) wsSeed);
      std::fprintf(stderr,
                   "workstations: %.1f%% -> %u vanilla + %u/%u non-vanilla = %u stations\n",
                   wsFraction * 100.0, sample.vanilla, sample.sampled, sample.nonVanilla,
                   sample.total);
    }
  }

  std::fprintf(out,
               "stage\ttarget\tname\tsub_items\tsub_recipes\tsub_arcs\t"
               "sccs\tlargest_nodes\tlargest_items\tlargest_recipes\t"
               "largest_frac_nodes\tlargest_frac_items");
  if (decomposeMode)
    std::fprintf(out,
                 "\tbig_items\tbig_recipes\tfringe_nodes\tfringe_items\t"
                 "fringe_recipes\tfringe_sccs\tfringe_cycle_sccs\tfringe_max_scc\t"
                 "upper_items\tlower_items\tparallel_items\tfringe_choice_items\t"
                 "fringe_leaf_items\tfringe_tag_recipes\tbig_tag_recipes");
  std::fprintf(out, "\n");

  aw::vector<Amount> inventory = aw::vector<Amount>::zeroes(graph.nItem);
  if (!stockArg.empty()) {
    aw::vector<std::pair<Handle, Amount>> stock;
    std::string stockPath = planPrefix + ".stock." + stockArg + ".tsv";
    if (stockArg == "none")
      stockPath = planPrefix + ".stock.none.tsv";
    else if (stockArg.find('/') != std::string::npos || stockArg.find('.') != std::string::npos)
      stockPath = stockArg;
    if (!readStock(stockPath, stock)) {
      std::fprintf(stderr, "cannot read stock from %s\n", stockPath.c_str());
      return EXIT_FAILURE;
    }
    for (const auto &entry : stock) {
      if (entry.first > graph.nItem)
        continue;
      inventory[entry.first - 1] += entry.second;
    }
    std::fprintf(stderr, "stock %s: %zu entries\n", stockPath.c_str(), stock.size());
  }

  for (const Stage *stage : stages) {
    applyStage(*stage);
    for (const Target &target : targets) {
      const aw::Subgraph sub = aw::reachableSubgraph(target.handle, stations, inventory);
      if (sub.graph.nItem == 0) {
        std::fprintf(out, "%s\t%u\t%s\t0\t0\t0\t0\t0\t0\t0\t0\t0", stage->name,
                     target.handle, target.name.c_str());
        if (decomposeMode)
          std::fprintf(out, "\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0");
        std::fprintf(out, "\n");
        continue;
      }
      aw::vector<int32_t> comp;
      const SccStats scc = analyzeScc(sub.graph, comp);
      const double fracNodes =
          (double) scc.largest / (double) (sub.graph.nItem + sub.graph.nRecipe);
      const double fracItems = (double) scc.largestItems / (double) sub.graph.nItem;
      std::fprintf(out,
                   "%s\t%u\t%s\t%u\t%u\t%zu\t%u\t%u\t%u\t%u\t%.4f\t%.4f",
                   stage->name, target.handle, target.name.c_str(), sub.graph.nItem,
                   sub.graph.nRecipe, sub.graph.i2r.numEdges() + sub.graph.r2i.numEdges(),
                   scc.components, scc.largest, scc.largestItems, scc.largestRecipes,
                   fracNodes, fracItems);
      if (decomposeMode) {
        const Decompose d = decompose(sub.graph, comp, scc.largestComp);
        std::fprintf(out, "\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u",
                     d.bigItems, d.bigRecipes, d.fringeNodes, d.fringeItems,
                     d.fringeRecipes, d.fringeComps, d.fringeCycleComps, d.fringeMaxScc,
                     d.upperItems, d.lowerItems, d.parallelItems, d.choiceItems,
                     d.leafItems, d.fringeTagRecipes, d.bigTagRecipes);
      }
      std::fprintf(out, "\n");
    }
    if (!quiet)
      std::fprintf(stderr, "stage %s done\n", stage->name);
  }

  if (out != stdout)
    std::fclose(out);
  return EXIT_SUCCESS;
}
