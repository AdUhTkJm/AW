// Cross-engine benchmark driver for AW itself.
//
// One process handles one (dataset, mode) pair: the recipe graph is registered
// ONCE, then every ablation stage and every query runs against that
// registration. That is what makes an overnight sweep affordable, because
// registration (dominance passes + pack certificates) is the expensive part and
// must not be repeated per query.
//
// Why one registration can serve every ablation stage: the dominance passes
// always compute their marks at registration time. `options.tagPruning` and
// friends are read by `reachableSubgraph` at query time, so flipping them after
// registration selects how much of the already-computed pruning is applied.
// `options.pack.enabled` is the exception -- `computePackPruning` early-returns
// when it is off -- so this tool always enables it before registering and only
// uses the flag as a query-time gate afterwards.
//
// The one option that genuinely cannot be flipped after registration is
// `options.nonoptimal`, which every registration-time pass reads. Hence the
// ground-truth run is a separate process with `--nonoptimal 0`.
//
// Output is JSON Lines: one `config` header object, one `registration` object,
// then one object per query. `--preprocess-only` stops after the registration row,
// which is what lets the preprocessing half of an existing file be refreshed
// without re-running the queries. See bench/README.md.

#include "aw/utils/Int128.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"

#include "WorkstationSample.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>

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
  aw::vector<aw::Amount> amounts;
};

bool readTsv(const std::string &path, aw::vector<aw::vector<std::string>> &rows) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    aw::vector<std::string> parts;
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
  aw::vector<aw::vector<std::string>> rows;
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

bool splitCommas(const std::string &text, aw::vector<std::string> &out) {
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
  bool seed;
  bool direct;
  bool recipe;
  bool subs;
  bool tag;
  bool pack;
  bool sat;
};

// The agreed cumulative order:
//   nothing -> seed -> direct -> recipe -> substitution -> tag -> pack -> satellite
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

const Stage *findStage(const std::string &name) {
  for (const Stage &stage : kStages)
    if (name == stage.name) return &stage;
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

// Everything the registration-time passes read. Must run before
// `registerCraftingGraph`.
void configureForRegistration(bool nonoptimal, aw::TagInlineMode inlineMode,
                              double packSeconds, double satelliteSeconds) {
  aw::options.nonoptimal = nonoptimal;
  aw::options.tagInlining = inlineMode;
  // Pack certificates are always computed; the stage only chooses whether the
  // query applies them, because the query gate is this same flag.
  aw::options.pack.enabled = true;
  aw::PackPruneOptions pack = aw::options.pack;
  pack.maxSeconds = packSeconds;
  aw::options.pack = pack;
  aw::SatellitePruneOptions satellite = aw::options.satellite;
  satellite.maxSeconds = satelliteSeconds;
  aw::options.satellite = satellite;
  // The other four are always on for the registration itself.
  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.substitutionPruning = true;
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;
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
aw::vector<MissingItem> deriveMissing(const aw::Subgraph &sub, aw::NodeId target,
                                       aw::Amount amount,
                                       const aw::vector<aw::Amount> &inventory,
                                       const aw::vector<int64_t> &exec) {
  const aw::BaseCraftingGraph &g = sub.graph;
  aw::vector<aw::int128> balance(g.nItem, 0);
  for (uint32_t r = 0; r < g.nRecipe; r++) {
    const aw::int128 times = exec[r];
    if (times == 0) continue;
    balance[g.output[r]] += (aw::int128) g.outputAmt[r] * times;
    const auto inputs = g.inputsOf(r);
    const auto weights = g.inputAmountsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      balance[inputs[k]] -= (aw::int128) weights[k] * times;
  }

  aw::vector<MissingItem> missing;
  for (uint32_t i = 0; i < g.nItem; i++) {
    aw::int128 required;
    if (i == target) {
      required = amount;
    } else {
      const aw::NodeId source = sub.itemOrigin[i];
      const aw::Amount available = source < inventory.size() ? inventory[source] : 0;
      required = -(aw::int128) available;
    }
    const aw::int128 shortfall = required - balance[i];
    if (shortfall <= 0) continue;
    const aw::int128 capped =
        shortfall > (aw::int128) INT64_MAX ? (aw::int128) INT64_MAX : shortfall;
    missing.push_back({i, (aw::Amount) capped});
  }
  std::sort(missing.begin(), missing.end(),
            [](const MissingItem &lhs, const MissingItem &rhs) { return lhs.amount > rhs.amount; });
  return missing;
}

// ------------------------------------------------------------------- name table

std::map<aw::Handle, std::string> loadNames(const std::string &path) {
  std::map<aw::Handle, std::string> names;
  aw::vector<aw::vector<std::string>> rows;
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
               "                [--inline-tags off|pre|post|both]\n"
               "                [--reprune 0|1] [--reprune-exact 0|1] [--reprune-pack 0|1]\n"
               "                [--reprune-pack-seconds <s>]\n"
               "                [--amounts 1,100000] [--groups none,leaves,random20]\n"
               "                [--warmup N] [--repeats R]\n"
               "                [--time-limit 40] [--gap 0.01] [--workers 16]\n"
               "                [--pack-seconds 60] [--satellite-seconds 2]\n"
               "                [--names <path>] [--quiet] [--preprocess-only]\n"
               "                [--ws-percent 0..100] [--ws-seed <n>]\n"
               "\n"
               "--preprocess-only registers the graph and writes the config header and the\n"
               "registration row, then exits without running a single query. Use it to refresh\n"
               "the preprocessing half of an existing JSONL without paying for plan_ms again.\n"
               "\n"
               "--ws-percent samples that percentage of the non-vanilla workstation pool; every\n"
               "`minecraft:` station stays on, so the total slightly exceeds the percentage.\n"
               "--names defaults to the `.names.tsv` next to the .awr and --ws-seed to\n"
               "20260101, so the subset is reproducible across tools.\n"
               "\n"
               "stages (cumulative): none seed direct recipe substitution tag pack satellite\n");
}

// Parse the --inline-tags value. Returns false on an unknown spelling.
bool parseInlineMode(const std::string &text, aw::TagInlineMode &out) {
  if (text == "off") out = aw::TagInlineMode::OFF;
  else if (text == "pre") out = aw::TagInlineMode::PRE_PRUNE;
  else if (text == "post") out = aw::TagInlineMode::QUERY_TIME;
  else if (text == "both") out = aw::TagInlineMode::BOTH;
  else return false;
  return true;
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
  aw::vector<std::pair<std::string, std::string>> stageArgs;  // profile -> label
  bool nonoptimal = true;
  bool flash = false;
  bool quiet = false;
  bool preprocessOnly = false;
  bool sampleWorkstations = false;
  aw::TagInlineMode inlineMode = aw::TagInlineMode::OFF;
  bool singleMemberInline = true;
  bool reprune = false;
  bool repruneExact = true;
  bool reprunePack = true;
  double reprunePackSeconds = 0.3;
  uint64_t wsSeed = awtools::kDefaultWorkstationSeed;
  int warmup = 1;
  int repeats = 1;
  long long workers = 16;
  double timeLimit = 40.0;
  double gap = 0.01;
  double packSeconds = 60.0;
  double satelliteSeconds = 2.0;
  double wsFraction = 1.0;

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
    else if (arg == "--inline-tags") {
      std::string value;
      next(value);
      if (!parseInlineMode(value, inlineMode)) {
        std::fprintf(stderr, "--inline-tags needs off, pre, post or both\n");
        return EXIT_FAILURE;
      }
    } else if (arg == "--single-member-inline") {
      std::string value; next(value);
      singleMemberInline = value != "0";
    } else if (arg == "--reprune") {
      std::string value; next(value);
      reprune = value != "0";
    } else if (arg == "--reprune-exact") {
      std::string value; next(value);
      repruneExact = value != "0";
    } else if (arg == "--reprune-pack") {
      std::string value; next(value);
      reprunePack = value != "0";
    } else if (arg == "--reprune-pack-seconds") {
      std::string value; next(value);
      if (!parseDouble(value, reprunePackSeconds)) return EXIT_FAILURE;
    } else if (arg == "--ws-percent") {
      std::string v; next(v);
      double percent = 0.0;
      if (!parseDouble(v, percent) || percent < 0.0 || percent > 100.0) {
        std::fprintf(stderr, "--ws-percent must be between 0 and 100\n");
        return EXIT_FAILURE;
      }
      wsFraction = percent / 100.0;
      sampleWorkstations = percent < 100.0;
    } else if (arg == "--ws-seed") {
      std::string v; next(v);
      long long parsed = 0;
      if (!parseI64(v, parsed) || parsed < 0) { std::fprintf(stderr, "bad --ws-seed\n"); return EXIT_FAILURE; }
      wsSeed = (uint64_t) parsed;
    }
    else if (arg == "--quiet") quiet = true;
    else if (arg == "--preprocess-only") preprocessOnly = true;
    else if (arg == "-h" || arg == "--help") { usage(); return EXIT_SUCCESS; }
    else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); usage(); return EXIT_FAILURE; }
  }

  if (dataset.empty() || awrPath.empty() || planPrefix.empty() || config.empty() || outPath.empty()) {
    usage();
    return EXIT_FAILURE;
  }

  // Everything below is harness input preparation: read the plan manifests and the
  // `.awr` bytes off disk. `registerCraftingGraph` does the parsing, so it is timed
  // separately as the engine's preprocessing. Keeping the two apart is what makes
  // `parse_ms` (harness + I/O) and `register_ms` (engine) comparable to the baselines,
  // whose harnesses parse the `.awr` themselves.
  const auto parseStart = Clock::now();

  // ---- plan files -------------------------------------------------------
  std::map<std::string, std::string> meta;
  if (!readMeta(planPrefix + ".meta.tsv", meta)) {
    std::fprintf(stderr, "cannot read %s.meta.tsv\n", planPrefix.c_str());
    return EXIT_FAILURE;
  }

  aw::vector<Target> targets;
  {
    aw::vector<aw::vector<std::string>> rows;
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
      aw::vector<std::string> pieces;
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

  aw::vector<std::string> groups;
  if (!groupsArg.empty()) {
    if (!splitCommas(groupsArg, groups)) { std::fprintf(stderr, "bad --groups\n"); return EXIT_FAILURE; }
  } else {
    const auto it = meta.find("groups");
    if (it == meta.end() || !splitCommas(it->second, groups)) groups = {"none", "leaves", "random20"};
  }

  // Stock per group, keyed by 1-based handle. The library wants a vector indexed
  // by source item node (handle - 1).
  std::map<std::string, aw::vector<StockEntry>> groupStock;
  for (const std::string &group : groups) {
    aw::vector<aw::vector<std::string>> rows;
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
  aw::vector<std::pair<const Stage *, std::string>> stages;
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
  aw::vector<std::byte> bytes(raw.size());
  std::memcpy(bytes.data(), raw.data(), raw.size());

  const std::map<aw::Handle, std::string> names =
      namesPath.empty() ? std::map<aw::Handle, std::string>() : loadNames(namesPath);
  const double parseMs = sinceMs(parseStart);

  aw::options.inlineSingleMemberTags = singleMemberInline;
  configureForRegistration(nonoptimal, inlineMode, packSeconds, satelliteSeconds);
  const auto registerStart = Clock::now();
  aw::registerCraftingGraph(bytes);
  const double registerMs = sinceMs(registerStart);
  if (aw::getCraftingError() != nullptr) {
    std::fprintf(stderr, "malformed graph: %s\n", aw::getCraftingError());
    return EXIT_FAILURE;
  }
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // Workstation availability. The default is every real handle, the historical
  // worst case (and equivalent to "every workstation on", since a recipe with
  // an empty workstation set is unreachable either way). --ws-percent keeps
  // all vanilla stations and samples the remaining pool from the names table.
  awtools::WorkstationSample wsSample;
  aw::vector<aw::Handle> stations;
  if (sampleWorkstations) {
    const std::string tablePath =
        namesPath.empty() ? awtools::defaultNamesPath(awrPath) : namesPath;
    aw::vector<std::string> resources;
    if (!awtools::loadResourceLocations(tablePath, graph.nReal, resources)) {
      std::fprintf(stderr, "cannot read name table %s (needed for --ws-percent)\n",
                   tablePath.c_str());
      return EXIT_FAILURE;
    }
    wsSample = awtools::sampleWorkstations(graph, resources, wsFraction, wsSeed);
    stations = wsSample.stations;
    if (!quiet)
      std::fprintf(stderr,
                   "workstations: %.1f%% -> %u vanilla + %u/%u non-vanilla = %u stations\n",
                   wsFraction * 100.0, wsSample.vanilla, wsSample.sampled,
                   wsSample.nonVanilla, wsSample.total);
  } else {
    stations.reserve(graph.nReal);
    for (aw::Handle handle = 1; handle <= graph.nReal; handle++)
      stations.push_back_unchecked(handle);
  }

  // Query-time re-pruning. Read by reachableSubgraph per query, so it is
  // installed once here and every stage/group query below sees it.
  aw::options.reprune.enabled = reprune;
  aw::options.reprune.exact = repruneExact;
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
        .real("satellite_seconds", satelliteSeconds)
        .real("parse_ms", parseMs)
        .str("preprocess_scope", "engine")
        .boolean("ws_sampled", sampleWorkstations)
        .real("ws_percent", wsFraction * 100.0)
        .num("ws_seed", (long long) wsSeed)
        .num("ws_vanilla", (long long) wsSample.vanilla)
        .num("ws_nonvanilla", (long long) wsSample.nonVanilla)
        .num("ws_sampled_count", (long long) wsSample.sampled)
        .num("ws_total", (long long) wsSample.total)
        .boolean("preprocess_only", preprocessOnly);
    const char *inlineText = inlineMode == aw::TagInlineMode::PRE_PRUNE ? "pre"
                             : inlineMode == aw::TagInlineMode::QUERY_TIME ? "post"
                             : inlineMode == aw::TagInlineMode::BOTH       ? "both"
                                                                           : "off";
    json.str("inline_tags", inlineText)
        .boolean("single_member_inline", singleMemberInline)
        .boolean("reprune", reprune)
        .boolean("reprune_exact", repruneExact)
        .boolean("reprune_pack", reprunePack)
        .real("reprune_pack_seconds", reprunePackSeconds);
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
      if (graph.producersOf(item).size() > 1) ++multiRecipe;

    Json json;
    json.str("type", "registration")
        .str("dataset", dataset)
        .str("config", config)
        .boolean("nonoptimal", nonoptimal)
        .boolean("flash", flash)
        .real("register_ms", registerMs)
        .real("parse_ms", parseMs)
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

  if (preprocessOnly) {
    out.flush();
    if (!quiet) {
      std::printf("preprocess-only: parse %.1f ms, registration %.1f ms, no queries run\n",
                  parseMs, registerMs);
    }
    return EXIT_SUCCESS;
  }

  // ---- query sweep ------------------------------------------------------
  aw::solver::Options solverOptions;
  solverOptions.maxTimeSeconds = timeLimit;
  solverOptions.relativeGap = gap;
  solverOptions.numWorkers = (int) workers;
  solverOptions.flash = flash;

  // Workstation availability was resolved above, before the config header.

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
      auto inventory = aw::vector<aw::Amount>::zeroes(graph.nItem);
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

            aw::vector<MissingItem> missing;
            if (ok) missing = deriveMissing(sub, node, amount, inventory, plan.exec);

            uint32_t multi = 0;
            for (uint32_t item = 0; item < sub.graph.nItem; item++)
              if (sub.graph.producersOf(item).size() > 1) multi++;

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
              case aw::PlanStatus::CYCLE_UNFULFILLED: json.str("status", "cycle_unfulfilled"); break;
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
