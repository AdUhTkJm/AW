#ifndef AW_TOOLS_WORKSTATION_SAMPLE_H
#define AW_TOOLS_WORKSTATION_SAMPLE_H

// Sampling a realistic set of available workstations.
//
// The benchmark tools so far always ran with *every* workstation available,
// which is the worst case for the pruning passes and the only setting the
// workstation-less Java baselines can be compared against. Real players have a
// subset on, though, so this helper turns a target percentage into a concrete
// `workstations` span for `aw::reachableSubgraph`.
//
// The draw follows the rule agreed in bench/README.md:
//
//   1. A workstation whose resource location starts with `minecraft:` is
//      always on. Vanilla stations are the ones a save file can be assumed to
//      have, and the corpora's early game depends on them.
//   2. Let M be the number of remaining, non-vanilla items that appear as a
//      workstation at least once. Keep a uniformly drawn subset of exactly
//      round(fraction * M) of them.
//
// The total is therefore V + round(fraction * M) where V is the vanilla count,
// which exceeds `fraction` of all workstation items by about (1 - fraction) * V.
// The draw is deterministic for a given (graph, fraction, seed) so `aw_bench`,
// `aw_scc` and `awr_inspect` all see the same subset.
//
// Note that restricting `workstations` to the set of items that actually occur
// as a workstation does not change the "all on" result: a real recipe with an
// empty workstation set is never reachable either way, so this helper's
// fraction >= 1 case matches the historical `1..nReal` span.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#include "aw/plan/CraftingGraph.h"

namespace awtools {

// Default seed for the workstation draw. The same constant is used by every
// tool, so a dataset + percentage pins down one subset across the harness.
inline constexpr uint64_t kDefaultWorkstationSeed = 20260101;

// `handle - 1` -> resource location, pulled from the second TSV column of a
// `.names.tsv`. `out` is resized to `nReal`; entries with no row stay empty
// (and are then treated as non-vanilla). Returns false when the file cannot be
// opened, leaving `out` empty.
inline bool loadResourceLocations(const std::string &path, uint32_t nReal,
                                  aw::vector<std::string> &out) {
  out.clear();
  std::ifstream in(path);
  if (!in)
    return false;
  out.assign(nReal, std::string());
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
    const std::string handleText = line.substr(0, first);
    char *end = nullptr;
    const unsigned long long handle = std::strtoull(handleText.c_str(), &end, 10);
    if (end == handleText.c_str() || *end != '\0' || handle == 0 || handle > nReal)
      continue;
    out[(size_t) handle - 1] =
        line.substr(first + 1, second == std::string::npos ? std::string::npos
                                                           : second - first - 1);
  }
  return true;
}

// The `.names.tsv` next to an `.awr`: strip the suffix, or append when the
// path does not use one.
inline std::string defaultNamesPath(const std::string &awr) {
  const std::string suffix = ".awr";
  if (awr.size() >= suffix.size() &&
      awr.compare(awr.size() - suffix.size(), suffix.size(), suffix) == 0)
    return awr.substr(0, awr.size() - suffix.size()) + ".names.tsv";
  return awr + ".names.tsv";
}

inline bool isVanillaResource(std::string_view resource) {
  return resource.rfind("minecraft:", 0) == 0;
}

// splitmix64: a small, fixed PRNG so the draw is identical on every platform
// and standard library. `std::shuffle`'s exact permutation is not specified.
inline uint64_t splitmix64(uint64_t &state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct WorkstationSample {
  // Handles to pass to `reachableSubgraph`, ascending.
  aw::vector<aw::Handle> stations;
  // Number of workstation items whose resource location is `minecraft:`.
  uint32_t vanilla = 0;
  // Number of non-vanilla workstation items (the pool the draw is taken from).
  uint32_t nonVanilla = 0;
  // How many of the pool were kept.
  uint32_t sampled = 0;
  // vanilla + sampled == stations.size().
  uint32_t total = 0;
};

// Builds the available-workstation set. `fraction` is in [0, 1]; a value >= 1
// (the default, and the historical behaviour) returns every real handle, so a
// caller that does not opt in to sampling is unchanged. `resourceLocation` is
// indexed by item node and may be shorter than nReal (missing entries count as
// non-vanilla); pass an empty span to treat everything as non-vanilla.
inline WorkstationSample sampleWorkstations(
    const aw::CraftingGraph &graph, std::span<const std::string> resourceLocation,
    double fraction, uint64_t seed) {
  WorkstationSample result;
  if (fraction >= 1.0) {
    result.stations.reserve(graph.nReal);
    for (aw::Handle handle = 1; handle <= graph.nReal; handle++)
      result.stations.push_back_unchecked(handle);
    result.total = graph.nReal;
    return result;
  }

  // The universe is the set of items that appear as a workstation at least
  // once, which is what "a percentage of workstations" refers to.
  aw::vector<uint8_t> isStation(graph.nReal, 0);
  for (uint32_t recipe = 0; recipe < graph.nRecipe; recipe++)
    for (aw::NodeId station : graph.workstations.targetsOf(recipe))
      if (station < graph.nReal)
        isStation[station] = 1;

  aw::vector<aw::Handle> pool;
  for (aw::NodeId item = 0; item < graph.nReal; item++) {
    if (!isStation[item])
      continue;
    const aw::Handle handle = (aw::Handle) (item + 1);
    const bool vanilla = item < resourceLocation.size() &&
                         isVanillaResource(resourceLocation[item]);
    if (vanilla) {
      result.vanilla++;
      result.stations.push_back(handle);
    } else {
      pool.push_back(handle);
    }
  }
  result.nonVanilla = (uint32_t) pool.size();

  size_t want = (size_t) std::llround(fraction * (double) pool.size());
  if (want > pool.size())
    want = pool.size();
  uint64_t state = seed != 0 ? seed : 1;
  for (size_t i = 0; i < want; i++) {
    const size_t span = pool.size() - i;
    const size_t j = i + (size_t) (splitmix64(state) % (uint64_t) span);
    const aw::Handle temporary = pool[i];
    pool[i] = pool[j];
    pool[j] = temporary;
  }
  for (size_t i = 0; i < want; i++)
    result.stations.push_back(pool[i]);
  result.sampled = (uint32_t) want;
  result.total = result.vanilla + (uint32_t) want;

  std::sort(result.stations.begin(), result.stations.end());
  return result;
}

}  // namespace awtools

#endif
