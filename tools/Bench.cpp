// Cross-engine benchmark driver for AW itself.
//
// One process handles one (dataset, mode) pair: the recipe graph is registered
// ONCE, then every ablation stage and every query runs against that
// registration. That is what makes an overnight sweep affordable, because
// registration (dominance passes + pack certificates) is the expensive part and
// must not be repeated per query.
//
// Why one registration can serve every ablation stage: the dominance passes
// always compute their marks at registration time. `setTagPruningEnabled` and
// friends are read by `reachableSubgraph` at query time, so flipping them after
// registration selects how much of the already-computed pruning is applied.
// `setPackPruningEnabled` is the exception -- `computePackPruning` early-returns
// when it is off -- so this tool always enables it before registering and only
// uses the flag as a query-time gate afterwards.
//
// The one option that genuinely cannot be flipped after registration is
// `options.nonoptimal`, which every registration-time pass reads. Hence the
// ground-truth run is a separate process with `--nonoptimal 0`.
//
// Output is JSON Lines: one `config` header object, one `registration` object,
// then one object per query. See bench/README.md.

#include "aw/CraftingGraph.h"
#include "aw/Options.h"
#include "aw/Plan.h"
#include "aw/Solver.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double sinceMs(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// --------------------------------------------------------------- JSON writing

std::string jsonEscape(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char) c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned) (unsigned char) c);
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Ordered JSON object builder; `text()` returns one line.
class Json {
public:
  Json &str(const char *key, const std::string &value) {
    return raw(key, "\"" + jsonEscape(value) + "\"");
  }
  Json &num(const char *key, long long value) { return raw(key, std::to_string(value)); }
  Json &real(const char *key, double value) {
    if (!std::isfinite(value)) return raw(key, "null");
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return raw(key, buffer);
  }
  Json &boolean(const char *key, bool value) { return raw(key, value ? "true" : "false"); }
  Json &raw(const char *key, const std::string &text) {
    if (first) first = false;
    else body += ",";
    body += '"';
    body += key;
    body += "\":";
    body += text;
    return *this;
  }
  const std::string &text() {
    if (!closed) {
      body += '}';
      closed = true;
    }
    return body;
  }

private:
  std::string body = "{";
  bool first = true;
  bool closed = false;
};

// -------------------------------------------------------------- TSV utilities

struct Target {
  aw::Handle handle = 0;
  std::string name;
  std::vector<aw::Amount> amounts;
};

bool readTsv(const std::string &path, std::vector<std::vector<std::string>> &rows) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
      const size_t tab = line.find('\t', start);
      parts.push_back(
          line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    rows.push_back(std::move(parts));
  }
  return true;
}

struct StockEntry {
  aw::Handle handle = 0;
  aw::Amount amount = 0;
};

bool readMeta(const std::string &path, std::map<std::string, std::string> &out) {
  std::vector<std::vector<std::string>> rows;
  if (!readTsv(path, rows)) return false;
  for (const auto &row : rows)
    if (row.size() >= 2) out[row[0]] = row[1];
  return true;
}

bool parseI64(const std::string &text, long long &out) {
  if (text.empty()) return false;
  char *end = nullptr;
  errno = 0;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') return false;
  out = value;
  return true;
}

bool parseDouble(const std::string &text, double &out) {
  if (text.empty()) return false;
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0') return false;
  out = value;
  return true;
}

bool splitCommas(const std::string &text, std::vector<std::string> &out) {
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const std::string piece =
        text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!piece.empty()) out.push_back(piece);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return !out.empty();
}

// ---------------------------------------------------------------------- stages

// One point on the cumulative ablation curve. The seven booleans are the
// query-time gates read by `reachableSubgraph`.
struct Stage {
  const char *name;
  bool dead;
  bool direct;
  bool recipe;
  bool subs;
  bool tag;
  bool pack;
  bool sat;
};

// The agreed cumulative order:
//   nothing -> dead_node -> direct -> recipe -> substitution -> tag -> pack -> satellite
const Stage kStages[] = {
    {"none", false, false, false, false, false, false, false},
    {"dead_node", true, false, false, false, false, false, false},
    {"direct", true, true, false, false, false, false, false},
    {"recipe", true, true, true, false, false, false, false},
    {"substitution", true, true, true, true, false, false, false},
    {"tag", true, true, true, true, true, false, false},
    {"pack", true, true, true, true, true, true, false},
    {"satellite", true, true, true, true, true, true, true},
};

const Stage *findStage(const std::string &name) {
  for (const Stage &stage : kStages)
    if (name == stage.name) return &stage;
  return nullptr;
}

void applyStage(const Stage &stage) {
  aw::setDeadNodePruningEnabled(stage.dead);
  aw::setDirectDominancePruningEnabled(stage.direct);
  aw::setRecipePruningEnabled(stage.recipe);
  aw::setSubstitutionPruningEnabled(stage.subs);
  aw::setTagPruningEnabled(stage.tag);
  aw::setPackPruningEnabled(stage.pack);
  aw::setSatellitePruningEnabled(stage.sat);
}

// Everything the registration-time passes read. Must run before
// `registerCraftingGraph`.
void configureForRegistration(bool nonoptimal, double packSeconds, double satelliteSeconds) {
  aw::options.nonoptimal = nonoptimal;
  aw::setTagInliningMode(aw::TagInlineMode::OFF);
  // Pack certificates are always computed; the stage only chooses whether the
  // query applies them, because the query gate is this same flag.
  aw::setPackPruningEnabled(true);
  aw::PackPruneOptions pack = aw::getPackPruningOptions();
  pack.maxSeconds = packSeconds;
  aw::setPackPruningOptions(pack);
  aw::SatellitePruneOptions satellite = aw::getSatellitePruningOptions();
  satellite.maxSeconds = satelliteSeconds;
  aw::setSatellitePruningOptions(satellite);
  // The other four are always on for the registration itself.
  aw::setTagPruningEnabled(true);
  aw::setRecipePruningEnabled(true);
  aw::setDirectDominancePruningEnabled(true);
  aw::setSubstitutionPruningEnabled(true);
  aw::setDeadNodePruningEnabled(true);
  aw::setSatellitePruningEnabled(true);
}

// ------------------------------------------------------- derived missing set

struct MissingItem {
  aw::NodeId item = 0;
  aw::Amount amount = 0;
};

// `planCrafting` returns only the execution vector, so the harness recomputes
// the very balance rows the solver was handed:
//   produced(i) - consumed(i) >= rhs(i);
//   rhs(target) = amount, rhs(i) = -stock(i)
// and reports every row that came up short -- the same currency in which the
// Java baselines report `missing`.
std::vector<MissingItem> deriveMissing(const aw::Subgraph &sub, aw::NodeId target,
                                       aw::Amount amount,
                                       const std::vector<aw::Amount> &inventory,
                                       const std::vector<int64_t> &exec) {
  const aw::BaseCraftingGraph &g = sub.graph;
  std::vector<__int128> balance(g.nItem, 0);
  for (uint32_t r = 0; r < g.nRecipe; r++) {
    const __int128 times = exec[r];
    if (times == 0) continue;
    balance[g.output[r]] += (__int128) g.outputAmt[r] * times;
    const auto inputs = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      balance[inputs[k]] -= (__int128) weights[k] * times;
  }

  std::vector<MissingItem> missing;
  for (uint32_t i = 0; i < g.nItem; i++) {
    __int128 required;
    if (i == target) {
      required = amount;
    } else {
      const aw::NodeId source = sub.itemOrigin[i];
      const aw::Amount available = source < inventory.size() ? inventory[source] : 0;
      required = -(__int128) available;
    }
    const __int128 shortfall = required - balance[i];
    if (shortfall <= 0) continue;
    const __int128 capped =
        shortfall > (__int128) INT64_MAX ? (__int128) INT64_MAX : shortfall;
    missing.push_back({i, (aw::Amount) capped});
  }
  std::sort(missing.begin(), missing.end(),
            [](const MissingItem &lhs, const MissingItem &rhs) { return lhs.amount > rhs.amount; });
  return missing;
}

// ------------------------------------------------------------------- name table

std::map<aw::Handle, std::string> loadNames(const std::string &path) {
  std::map<aw::Handle, std::string> names;
  std::vector<std::vector<std::string>> rows;
  if (!readTsv(path, rows)) return names;
  for (const auto &row : rows) {
    if (row.size() < 3) continue;
    long long handle = 0;
    if (parseI64(row[0], handle) && handle > 0) names[(aw::Handle) handle] = row[2];
  }
  return names;
}

std::string label(const std::map<aw::Handle, std::string> &names, aw::NodeId item,
                  uint32_t nReal) {
  const aw::Handle handle = item + 1;
  if (item >= nReal) return "#" + std::to_string(handle);
  const auto it = names.find(handle);
  return it == names.end() ? "#" + std::to_string(handle) : it->second;
}

void usage() {
  std::fprintf(stderr,
               "usage: aw_bench --dataset <name> --awr <path> --plan <prefix> --config <name> --out <jsonl>\n"
               "                [--nonoptimal 0|1] [--flash 0|1] [--stage <profile>[:<label>]]...\n"
               "                [--amounts 1,100000] [--groups none,leaves,random20]\n"
               "                [--warmup N] [--repeats R]\n"
               "                [--time-limit 40] [--gap 0.01] [--workers 16]\n"
               "                [--pack-seconds 60] [--satellite-seconds 2]\n"
               "                [--names <path>] [--quiet]\n"
               "\n"
               "stages (cumulative): none dead_node direct recipe substitution tag pack satellite\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::string dataset;
  std::string awrPath;
  std::string planPrefix;
  std::string config;
  std::string outPath;
  std::string namesPath;
  std::string amountsArg;
  std::string groupsArg;
  std::vector<std::pair<std::string, std::string>> stageArgs;  // profile -> label
  bool nonoptimal = true;
  bool flash = false;
  bool quiet = false;
  int warmup = 1;
  int repeats = 1;
  long long workers = 16;
  double timeLimit = 40.0;
  double gap = 0.01;
  double packSeconds = 60.0;
  double satelliteSeconds = 2.0;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    auto next = [&](std::string &into) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg.c_str());
        std::exit(EXIT_FAILURE);
      }
      into = argv[++i];
    };
    if (arg == "--dataset") next(dataset);
    else if (arg == "--awr") next(awrPath);
    else if (arg == "--plan") next(planPrefix);
    else if (arg == "--config") next(config);
    else if (arg == "--out") next(outPath);
    else if (arg == "--names") next(namesPath);
    else if (arg == "--amounts") next(amountsArg);
    else if (arg == "--groups") next(groupsArg);
    else if (arg == "--nonoptimal") {
      std::string value;
      next(value);
      nonoptimal = value != "0";
    } else if (arg == "--flash") {
      std::string value;
      next(value);
      flash = value != "0";
    } else if (arg == "--stage") {
      std::string value;
      next(value);
      const size_t colon = value.find(':');
      stageArgs.push_back({value.substr(0, colon),
                           colon == std::string::npos ? value : value.substr(colon + 1)});
    } else if (arg == "--warmup") {
      std::string value; next(value);
      long long parsed = 0;
      if (!parseI64(value, parsed) || parsed < 0) { std::fprintf(stderr, "bad --warmup\n"); return EXIT_FAILURE; }
      warmup = (int) parsed;
    } else if (arg == "--repeats") {
      std::string value; next(value);
      long long parsed = 0;
      if (!parseI64(value, parsed) || parsed < 1) { std::fprintf(stderr, "bad --repeats\n"); return EXIT_FAILURE; }
      repeats = (int) parsed;
    } else if (arg == "--time-limit") { std::string v; next(v); if (!parseDouble(v, timeLimit)) return EXIT_FAILURE; }
    else if (arg == "--gap") { std::string v; next(v); if (!parseDouble(v, gap)) return EXIT_FAILURE; }
    else if (arg == "--workers") { std::string v; next(v); long long p = 0; if (!parseI64(v, p) || p < 0) return EXIT_FAILURE; workers = p; }
    else if (arg == "--pack-seconds") { std::string v; next(v); if (!parseDouble(v, packSeconds)) return EXIT_FAILURE; }
    else if (arg == "--satellite-seconds") { std::string v; next(v); if (!parseDouble(v, satelliteSeconds)) return EXIT_FAILURE; }
    else if (arg == "--quiet") quiet = true;
    else if (arg == "-h" || arg == "--help") { usage(); return EXIT_SUCCESS; }
    else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); usage(); return EXIT_FAILURE; }
  }

  if (dataset.empty() || awrPath.empty() || planPrefix.empty() || config.empty() || outPath.empty()) {
    usage();
    return EXIT_FAILURE;
  }

  // ---- plan files -------------------------------------------------------
  std::map<std::string, std::string> meta;
  if (!readMeta(planPrefix + ".meta.tsv", meta)) {
    std::fprintf(stderr, "cannot read %s.meta.tsv\n", planPrefix.c_str());
    return EXIT_FAILURE;
  }

  std::vector<Target> targets;
  {
    std::vector<std::vector<std::string>> rows;
    if (!readTsv(planPrefix + ".targets.tsv", rows)) {
      std::fprintf(stderr, "cannot read %s.targets.tsv\n", planPrefix.c_str());
      return EXIT_FAILURE;
    }
    for (const auto &row : rows) {
      if (row.size() < 3) continue;
      Target target;
      long long handle = 0;
      if (!parseI64(row[0], handle) || handle <= 0) continue;
      target.handle = (aw::Handle) handle;
      target.name = row[1];
      // `--amounts` overrides the amounts recorded in the plan file.
      const std::string amountText = amountsArg.empty() ? row[2] : amountsArg;
      std::vector<std::string> pieces;
      if (!splitCommas(amountText, pieces)) continue;
      for (const auto &piece : pieces) {
        long long value = 0;
        if (!parseI64(piece, value) || value < 0) {
          std::fprintf(stderr, "bad amount for handle %lld\n", handle);
          return EXIT_FAILURE;
        }
        target.amounts.push_back((aw::Amount) value);
      }
      if (!target.amounts.empty()) targets.push_back(std::move(target));
    }
  }
  if (targets.empty()) {
    std::fprintf(stderr, "no targets in %s.targets.tsv\n", planPrefix.c_str());
    return EXIT_FAILURE;
  }

  std::vector<std::string> groups;
  if (!groupsArg.empty()) {
    if (!splitCommas(groupsArg, groups)) { std::fprintf(stderr, "bad --groups\n"); return EXIT_FAILURE; }
  } else {
    const auto it = meta.find("groups");
    if (it == meta.end() || !splitCommas(it->second, groups)) groups = {"none", "leaves", "random20"};
  }

  // Stock per group, keyed by 1-based handle. The library wants a vector indexed
  // by source item node (handle - 1).
  std::map<std::string, std::vector<StockEntry>> groupStock;
  for (const std::string &group : groups) {
    std::vector<std::vector<std::string>> rows;
    if (!readTsv(planPrefix + ".stock." + group + ".tsv", rows)) {
      std::fprintf(stderr, "cannot read %s.stock.%s.tsv\n", planPrefix.c_str(), group.c_str());
      return EXIT_FAILURE;
    }
    auto &entries = groupStock[group];
    for (const auto &row : rows) {
      if (row.size() < 2) continue;
      long long handle = 0, amount = 0;
      if (!parseI64(row[0], handle) || !parseI64(row[1], amount) || handle <= 0 || amount < 0) continue;
      entries.push_back({(aw::Handle) handle, (aw::Amount) amount});
    }
  }

  // Stage list.
  std::vector<std::pair<const Stage *, std::string>> stages;
  if (stageArgs.empty()) {
    for (const Stage &stage : kStages) stages.push_back({&stage, stage.name});
  } else {
    for (const auto &entry : stageArgs) {
      const Stage *stage = findStage(entry.first);
      if (stage == nullptr) {
        std::fprintf(stderr, "unknown stage: %s\n", entry.first.c_str());
        usage();
        return EXIT_FAILURE;
      }
      stages.push_back({stage, entry.second});
    }
  }

  // ---- graph ------------------------------------------------------------
  std::ifstream in(awrPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", awrPath.c_str());
    return EXIT_FAILURE;
  }
  const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(raw.size());
  std::memcpy(bytes.data(), raw.data(), raw.size());

  configureForRegistration(nonoptimal, packSeconds, satelliteSeconds);
  const auto registerStart = Clock::now();
  aw::registerCraftingGraph(bytes);
  const double registerMs = sinceMs(registerStart);
  if (aw::getCraftingError() != nullptr) {
    std::fprintf(stderr, "malformed graph: %s\n", aw::getCraftingError());
    return EXIT_FAILURE;
  }
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  const std::map<aw::Handle, std::string> names =
      namesPath.empty() ? std::map<aw::Handle, std::string>() : loadNames(namesPath);

  std::ofstream out(outPath);
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
    return EXIT_FAILURE;
  }

  // Config header, so a JSONL file is self-describing.
  {
    Json json;
    json.str("type", "config")
        .str("dataset", dataset)
        .str("config", config)
        .boolean("nonoptimal", nonoptimal)
        .boolean("flash", flash)
        .num("warmup", warmup)
        .num("repeats", repeats)
        .real("time_limit_s", timeLimit)
        .real("gap", gap)
        .num("workers", workers)
        .real("pack_seconds", packSeconds)
        .real("satellite_seconds", satelliteSeconds);
    std::string stageNames;
    for (size_t k = 0; k < stages.size(); k++) {
      if (k != 0) stageNames += ",";
      stageNames += stages[k].second;
    }
    json.str("stages", stageNames);
    std::string groupNames;
    for (size_t k = 0; k < groups.size(); k++) {
      if (k != 0) groupNames += ",";
      groupNames += groups[k];
    }
    json.str("groups", groupNames);
    out << json.text() << "\n";
  }

  // Registration row: the preprocessing half of the measurement, plus the
  // registration-time pruning marks.
  {
    size_t tagEdges = 0, tagDominated = 0, recipeDominated = 0, directDominated = 0;
    size_t substituted = 0, packDominated = 0, multiRecipe = 0;
    for (uint32_t r = 0; r < graph.nRecipe; r++) {
      if (graph.output[r] >= graph.nReal) ++tagEdges;
      if (r < graph.tagEdgeDominated.size() && graph.tagEdgeDominated[r]) ++tagDominated;
      if (r < graph.recipeDominated.size() && graph.recipeDominated[r]) ++recipeDominated;
      if (r < graph.recipeDirectDominated.size() && graph.recipeDirectDominated[r]) ++directDominated;
      if (r < graph.recipeSubstituted.size() && graph.recipeSubstituted[r]) ++substituted;
      if (r < graph.packDominated.size() && graph.packDominated[r]) ++packDominated;
    }
    for (uint32_t item = 0; item < graph.nItem; item++)
      if (graph.i2r.targetsOf(item).size() > 1) ++multiRecipe;

    Json json;
    json.str("type", "registration")
        .str("dataset", dataset)
        .str("config", config)
        .boolean("nonoptimal", nonoptimal)
        .boolean("flash", flash)
        .real("register_ms", registerMs)
        .num("items", graph.nItem)
        .num("real_items", graph.nReal)
        .num("recipes", graph.nRecipe)
        .num("i2r_edges", (long long) graph.i2r.numEdges())
        .num("r2i_edges", (long long) graph.r2i.numEdges())
        .num("ws_edges", (long long) graph.workstations.numEdges())
        .num("tag_edges", (long long) tagEdges)
        .num("tag_dominated", (long long) tagDominated)
        .num("recipe_dominated", (long long) recipeDominated)
        .num("direct_dominated", (long long) directDominated)
        .num("substituted", (long long) substituted)
        .num("pack_dominated", (long long) packDominated)
        .num("items_multi_recipe", (long long) multiRecipe);
    out << json.text() << "\n";
    if (!quiet) {
      std::printf("registration: %.1f ms  items=%u recipes=%u tag=%zu dominated(tag/recipe/direct/subs/pack)=%zu/%zu/%zu/%zu/%zu\n",
                  registerMs, graph.nItem, graph.nRecipe, tagEdges, tagDominated,
                  recipeDominated, directDominated, substituted, packDominated);
    }
  }

  // ---- query sweep ------------------------------------------------------
  aw::solver::Options solverOptions;
  solverOptions.maxTimeSeconds = timeLimit;
  solverOptions.relativeGap = gap;
  solverOptions.numWorkers = (int) workers;
  solverOptions.flash = flash;

  // All workstations available: every real item handle is allowed, which is the
  // worst case for the pruning and the only setting the workstation-less Java
  // baselines can be compared against.
  std::vector<aw::Handle> stations;
  stations.reserve(graph.nReal);
  for (aw::Handle handle = 1; handle <= graph.nReal; handle++) stations.push_back(handle);

  const size_t missingCap = 12;
  long long rows = 0;
  long long timeouts = 0;
  long long noAnswerCount = 0;
  long long unprovenCount = 0;
  long long infeasible = 0;

  for (const auto &stageEntry : stages) {
    applyStage(*stageEntry.first);
    const std::string &stageLabel = stageEntry.second;

    for (const std::string &group : groups) {
      // Build the full inventory vector once per group.
      std::vector<aw::Amount> inventory(graph.nItem, 0);
      for (const auto &entry : groupStock[group]) {
        if (entry.handle > graph.nItem) continue;
        inventory[entry.handle - 1] += entry.amount;
      }

      for (const Target &target : targets) {
        for (aw::Amount amount : target.amounts) {
          for (int repeat = 0; repeat < repeats + warmup; repeat++) {
            const bool measured = repeat >= warmup;

            const auto reachStart = Clock::now();
            const aw::Subgraph sub = aw::reachableSubgraph(target.handle, stations, inventory);
            const double reachMs = sinceMs(reachStart);

            double solveMs = 0.0;
            aw::PlanResult plan;
            aw::NodeId node = UINT32_MAX;
            if (sub.graph.nItem == 0) {
              plan.status = aw::PlanStatus::INVALID_INPUT;
            } else {
              node = sub.translate(aw::CraftingGraph::itemNode(target.handle));
              if (node == UINT32_MAX) {
                plan.status = aw::PlanStatus::INVALID_INPUT;
              } else {
                const auto solveStart = Clock::now();
                plan = aw::planCrafting(sub, node, amount, inventory, solverOptions);
                solveMs = sinceMs(solveStart);
              }
            }
            if (!measured) continue;

            rows++;
            const bool ok = plan.status == aw::PlanStatus::OK;
            // Three outcomes that a single "gave up" label would merge:
            //
            //   ok + provenOptimal      -> a certified optimal plan,
            //   ok + !provenOptimal     -> a FEASIBLE plan AW stopped improving (gap
            //                              is reported; this is PlanResult's
            //                              "gave up early" and it is usable in game),
            //   iter_limit              -> NO plan at all.
            //
            // The last one splits again: the 40 s budget actually elapsed, or CP-SAT
            // bailed immediately because the loosest variable domains are numerically
            // hopeless (on the unpruned subgraph every zero-cost tag column falls back
            // to absoluteCap's ~1e16 domain). `noAnswer`/`timeout` keep that visible so
            // a fast "no answer" is never read as "spent the whole budget".
            const bool iterLimit = plan.status == aw::PlanStatus::ITER_LIMIT;
            const bool timedOut = iterLimit && solveMs >= 0.9 * timeLimit * 1000.0;
            const bool noAnswer = iterLimit;
            if (timedOut) timeouts++;
            if (noAnswer) noAnswerCount++;
            if (ok && !plan.provenOptimal) unprovenCount++;
            if (plan.status == aw::PlanStatus::INFEASIBLE) infeasible++;

            long long cost = 0, tagExec = 0, distinct = 0;
            if (ok) {
              for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
                const int64_t times = plan.exec[r];
                if (times == 0) continue;
                distinct++;
                if (sub.graph.output[r] < sub.graph.nReal) cost += times;
                else tagExec += times;
              }
            }

            std::vector<MissingItem> missing;
            if (ok) missing = deriveMissing(sub, node, amount, inventory, plan.exec);

            uint32_t multi = 0;
            for (uint32_t item = 0; item < sub.graph.nItem; item++)
              if (sub.graph.i2r.targetsOf(item).size() > 1) multi++;

            Json json;
            json.str("type", "query")
                .str("dataset", dataset)
                .str("config", config)
                .str("stage", stageLabel)
                .num("target", (long long) target.handle)
                .str("target_name", target.name)
                .num("amount", (long long) amount)
                .str("stock", group)
                .num("repeat", repeat - warmup)
                // round 0 is the first execution of this query in this process. For AE2VM
                // that distinction changes the ANSWER, not just the time: its warm path
                // replays a memoized plan. Always report cold and warm separately there.
                .boolean("cold", repeat == 0)
                .real("reach_ms", reachMs)
                .real("solve_ms", solveMs)
                .real("query_ms", reachMs + solveMs)
                .num("items", sub.graph.nItem)
                .num("recipes", sub.graph.nRecipe)
                .num("r2i_edges", (long long) sub.graph.r2i.numEdges())
                .num("items_multi_recipe", (long long) multi)
                .num("missing_count", (long long) missing.size());
            switch (plan.status) {
              case aw::PlanStatus::OK: json.str("status", "ok"); break;
              case aw::PlanStatus::INFEASIBLE: json.str("status", "infeasible"); break;
              case aw::PlanStatus::NUMERICAL_FAIL: json.str("status", "numerical_fail"); break;
              case aw::PlanStatus::ITER_LIMIT: json.str("status", "iter_limit"); break;
              case aw::PlanStatus::INVALID_INPUT: json.str("status", "invalid_input"); break;
            }
            // `optimality` is the engine's own claim, made explicit and uniform
            // across engines: proven | unproven | unclaimed | no_plan. For AW,
            // "unproven" is a real plan with `gap` bounded by the relative-gap rule
            // (1% by default), so it is an answer, not a failure. `proven_optimal` is
            // tri-state on purpose: null means "this engine makes no such claim for
            // this row", which is different from false.
            const char *optimality = !ok ? "no_plan"
                                         : (plan.provenOptimal ? "proven" : "unproven");
            json.boolean("feasible", ok)
                .boolean("balance_ok", ok && missing.empty())
                .boolean("false_positive", ok && !missing.empty())
                .raw("proven_optimal", ok ? (plan.provenOptimal ? "true" : "false") : "null")
                .str("optimality", optimality)
                .boolean("has_plan", ok)
                .boolean("timeout", timedOut)
                .boolean("no_answer", noAnswer)
                .real("gap", plan.gap)
                .real("bound", plan.bestBound)
                .num("cost", cost)
                .num("tag_exec", tagExec)
                .num("distinct_recipes", distinct)
                .num("conflicts", plan.numConflicts)
                .num("branches", plan.numBranches)
                .num("fixed_columns", (long long) plan.fixedColumns);

            std::string missingJson = "[";
            const size_t shown = std::min(missing.size(), missingCap);
            for (size_t k = 0; k < shown; k++) {
              if (k != 0) missingJson += ",";
              missingJson += "[\"" + jsonEscape(label(names, missing[k].item, sub.graph.nReal)) +
                             "\"," + std::to_string(missing[k].amount) + "]";
            }
            missingJson += "]";
            json.raw("missing", missingJson);
            out << json.text() << "\n";
          }
        }
      }
    }
  }

  out.flush();
  if (!quiet) {
    std::printf("queries=%lld proven=%lld unproven=%lld no_answer=%lld (of which timed_out=%lld) "
                "infeasible=%lld stage(s)=%zu\n",
                rows, rows - unprovenCount - noAnswerCount - infeasible, unprovenCount,
                noAnswerCount, timeouts, infeasible, stages.size());
    std::printf("wrote %s\n", outPath.c_str());
  }
  return EXIT_SUCCESS;
}
