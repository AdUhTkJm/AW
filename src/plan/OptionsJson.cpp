#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <initializer_list>

#include "aw/plan/Options.h"
#include "aw/plan/OptionsJson.h"
#include "aw/plan/Solver.h"

namespace aw {

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Scalar readers.
//
// Each one returns true when the key is absent, so a patch only overrides what
// it names. `key` doubles as the error label.
// ---------------------------------------------------------------------------

bool readBool(const json &j, const char *key, bool &out, std::string &error) {
  const auto it = j.find(key);
  if (it == j.end())
    return true;
  if (!it->is_boolean()) {
    error = std::string(key) + " must be a boolean";
    return false;
  }
  out = it->get<bool>();
  return true;
}

bool readU64(const json &j, const char *key, uint64_t &out, std::string &error) {
  const auto it = j.find(key);
  if (it == j.end())
    return true;
  if (it->is_number_unsigned()) {
    out = it->get<uint64_t>();
    return true;
  }
  if (it->is_number_integer()) {
    const int64_t value = it->get<int64_t>();
    if (value < 0) {
      error = std::string(key) + " must not be negative";
      return false;
    }
    out = (uint64_t) value;
    return true;
  }
  error = std::string(key) + " must be a non-negative integer";
  return false;
}

bool readU32(const json &j, const char *key, uint32_t &out, std::string &error) {
  uint64_t wide = 0;
  if (!readU64(j, key, wide, error))
    return false;
  if (wide > UINT32_MAX) {
    error = std::string(key) + " does not fit in 32 bits";
    return false;
  }
  out = (uint32_t) wide;
  return true;
}

bool readSize(const json &j, const char *key, size_t &out, std::string &error) {
  uint64_t wide = 0;
  if (!readU64(j, key, wide, error))
    return false;
  out = (size_t) wide;
  return true;
}

bool readI64(const json &j, const char *key, int64_t &out, std::string &error) {
  const auto it = j.find(key);
  if (it == j.end())
    return true;
  if (it->is_number_unsigned()) {
    const uint64_t value = it->get<uint64_t>();
    if (value > (uint64_t) INT64_MAX) {
      error = std::string(key) + " does not fit in 64 bits";
      return false;
    }
    out = (int64_t) value;
    return true;
  }
  if (it->is_number_integer()) {
    out = it->get<int64_t>();
    return true;
  }
  error = std::string(key) + " must be an integer";
  return false;
}

bool readInt(const json &j, const char *key, int &out, std::string &error) {
  int64_t wide = 0;
  if (!readI64(j, key, wide, error))
    return false;
  if (wide < INT32_MIN || wide > INT32_MAX) {
    error = std::string(key) + " does not fit in 32 bits";
    return false;
  }
  out = (int) wide;
  return true;
}

bool readDouble(const json &j, const char *key, double &out, std::string &error) {
  const auto it = j.find(key);
  if (it == j.end())
    return true;
  if (!it->is_number()) {
    error = std::string(key) + " must be a number";
    return false;
  }
  out = it->get<double>();
  return true;
}

// Tag inlining is the one enum, so it takes its values by name; the raw ordinal
// is accepted too, which keeps the Java side free to send either.
bool readTagInlining(const json &j, const char *key, TagInlineMode &out,
                     std::string &error) {
  const auto it = j.find(key);
  if (it == j.end())
    return true;
  if (it->is_string()) {
    const std::string value = it->get<std::string>();
    if (value == "off") out = TagInlineMode::OFF;
    else if (value == "prePrune") out = TagInlineMode::PRE_PRUNE;
    else if (value == "queryTime") out = TagInlineMode::QUERY_TIME;
    else if (value == "both") out = TagInlineMode::BOTH;
    else {
      error = std::string(key) + " must be one of off, prePrune, queryTime, both";
      return false;
    }
    return true;
  }
  uint32_t ordinal = 0;
  if (!readU32(j, key, ordinal, error))
    return false;
  if (ordinal > (uint32_t) TagInlineMode::BOTH) {
    error = std::string(key) + " must be between 0 and 3";
    return false;
  }
  out = (TagInlineMode) ordinal;
  return true;
}

// Rejects a key the C++ side would otherwise ignore. Without this a config typo
// would look like a no-op.
bool checkKnown(const json &j, std::initializer_list<const char *> known,
                std::string &error) {
  for (const auto &entry : j.items()) {
    bool found = false;
    for (const char *key : known)
      if (entry.key() == key) {
        found = true;
        break;
      }
    if (!found) {
      error = "unknown option: " + entry.key();
      return false;
    }
  }
  return true;
}

bool readPackOptions(const json &j, PackPruneOptions &out, std::string &error) {
  return readBool(j, "enabled", out.enabled, error) &&
         readU32(j, "maxPackRecipes", out.maxPackRecipes, error) &&
         readI64(j, "maxPackValue", out.maxPackValue, error) &&
         readU32(j, "maxBranchDepth", out.maxBranchDepth, error) &&
         readU32(j, "maxBranchNodes", out.maxBranchNodes, error) &&
         readU32(j, "maxZeroStockItems", out.maxZeroStockItems, error) &&
         readDouble(j, "maxSeconds", out.maxSeconds, error) &&
         checkKnown(j, {"enabled", "maxPackRecipes", "maxPackValue", "maxBranchDepth",
                        "maxBranchNodes", "maxZeroStockItems", "maxSeconds"},
                    error);
}

bool readSatelliteOptions(const json &j, SatellitePruneOptions &out,
                          std::string &error) {
  return readBool(j, "enabled", out.enabled, error) &&
         readU32(j, "maxComponentNodes", out.maxComponentNodes, error) &&
         readU32(j, "maxIslandNodes", out.maxIslandNodes, error) &&
         readDouble(j, "maxSeconds", out.maxSeconds, error) &&
         checkKnown(j, {"enabled", "maxComponentNodes", "maxIslandNodes", "maxSeconds"},
                    error);
}

bool readVariantClassOptions(const json &j, VariantClassPruneOptions &out,
                             std::string &error) {
  return readBool(j, "enabled", out.enabled, error) &&
         readU32(j, "maxClassNodes", out.maxClassNodes, error) &&
         readU32(j, "maxCandidates", out.maxCandidates, error) &&
         readDouble(j, "maxSeconds", out.maxSeconds, error) &&
         checkKnown(j, {"enabled", "maxClassNodes", "maxCandidates", "maxSeconds"},
                    error);
}

// A nested object is optional; when present it must really be an object.
bool readNested(const json &j, const char *key, const json *&out, std::string &error) {
  const auto it = j.find(key);
  if (it == j.end()) {
    out = nullptr;
    return true;
  }
  if (!it->is_object()) {
    error = std::string(key) + " must be an object";
    return false;
  }
  out = &*it;
  return true;
}

}  // namespace

bool applyPlannerOptionsJson(std::string_view text, std::string &error) noexcept {
  try {
    const json j = json::parse(text.begin(), text.end());
    if (!j.is_object()) {
      error = "planner options must be a JSON object";
      return false;
    }

    // Patched in place on a copy so a mid-way failure cannot leave the running
    // configuration half updated.
    Options next = options;

    if (!readBool(j, "nonoptimal", next.nonoptimal, error) ||
        !readBool(j, "tagPruning", next.tagPruning, error) ||
        !readBool(j, "recipePruning", next.recipePruning, error) ||
        !readBool(j, "directPruning", next.directPruning, error) ||
        !readBool(j, "substitutionPruning", next.substitutionPruning, error) ||
        !readBool(j, "seedPruning", next.seedPruning, error) ||
        !readBool(j, "tagTidy", next.tagTidy, error) ||
        !readBool(j, "flash", next.flash, error))
      return false;

    {
      // Always accepted, even when the build has no profiling pass to switch:
      // the mod sends one options document on every platform.
      [[maybe_unused]] bool ignored = false;
      if (!readBool(j, "outputPruningProfile",
#ifdef AW_PROFILE_PRUNING
                    next.outputPruningProfile,
#else
                    ignored,
#endif
                    error))
        return false;
    }

    if (!readTagInlining(j, "tagInlining", next.tagInlining, error))
      return false;

    const json *pack = nullptr;
    if (!readNested(j, "pack", pack, error))
      return false;
    if (pack != nullptr && !readPackOptions(*pack, next.pack, error))
      return false;

    const json *satellite = nullptr;
    if (!readNested(j, "satellite", satellite, error))
      return false;
    if (satellite != nullptr && !readSatelliteOptions(*satellite, next.satellite, error))
      return false;

    const json *variantClass = nullptr;
    if (!readNested(j, "variantClass", variantClass, error))
      return false;
    if (variantClass != nullptr &&
        !readVariantClassOptions(*variantClass, next.variantClass, error))
      return false;

    if (!readSize(j, "maxTagMembers", next.maxTagMembers, error) ||
        !readU64(j, "maxTagPairs", next.maxTagPairs, error) ||
        !readU64(j, "maxTagCoverWork", next.maxTagCoverWork, error) ||
        !readU64(j, "maxWitnessPairs", next.maxWitnessPairs, error) ||
        !readU64(j, "maxPrunePairs", next.maxPrunePairs, error) ||
        !readSize(j, "maxSiblingRecipes", next.maxSiblingRecipes, error) ||
        !readSize(j, "maxWitnessProducers", next.maxWitnessProducers, error) ||
        !readU64(j, "maxSubstitutionWork", next.maxSubstitutionWork, error) ||
        !readU64(j, "maxSubstitutionCostWork", next.maxSubstitutionCostWork, error) ||
        !readU32(j, "maxSubstitutionDepth", next.maxSubstitutionDepth, error) ||
        !readU32(j, "maxCostDepth", next.maxCostDepth, error) ||
        !readSize(j, "maxSubstitutionGuardItems", next.maxSubstitutionGuardItems, error) ||
        !readSize(j, "maxSubstitutionGuardTotal", next.maxSubstitutionGuardTotal, error) ||
        !readI64(j, "maxNeed", next.maxNeed, error) ||
        !readInt(j, "maxPropIterations", next.maxPropIterations, error))
      return false;

    // `deadNodePruning` is a retired knob: seed pruning subsumes it. It is kept
    // in the known set so an older client that still sends it is not rejected;
    // the value is ignored, and it is not written back by plannerOptionsJson.
    if (!checkKnown(j,
                    {"nonoptimal", "tagPruning", "recipePruning", "directPruning",
                     "substitutionPruning", "deadNodePruning", "seedPruning",
                     "tagTidy", "outputPruningProfile",
                     "tagInlining", "pack", "satellite", "variantClass", "maxTagMembers",
                     "maxTagPairs",
                     "maxTagCoverWork", "maxWitnessPairs", "maxPrunePairs",
                     "maxSiblingRecipes", "maxWitnessProducers", "maxSubstitutionWork",
                     "maxSubstitutionCostWork", "maxSubstitutionDepth", "maxCostDepth",
                     "maxCostMemo", "maxSubstitutionGuardItems",
                     "maxSubstitutionGuardTotal", "maxNeed", "maxPropIterations"},
                    error))
      return false;

    options = next;
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  } catch (...) {
    error = "could not parse the planner options";
    return false;
  }
}

bool applySolverOptionsJson(std::string_view text, std::string &error) noexcept {
  try {
    const json j = json::parse(text.begin(), text.end());
    if (!j.is_object()) {
      error = "solver options must be a JSON object";
      return false;
    }

    solver::Options next = solverOptions;
    if (!readDouble(j, "relativeGap", next.relativeGap, error) ||
        !readI64(j, "absoluteGap", next.absoluteGap, error) ||
        !readDouble(j, "maxTimeSeconds", next.maxTimeSeconds, error) ||
        !readInt(j, "numWorkers", next.numWorkers, error) ||
        !readInt(j, "randomSeed", next.randomSeed, error) ||
        !readI64(j, "objectiveCap", next.objectiveCap, error) ||
        !readI64(j, "objectiveUpperBound", next.objectiveUpperBound, error) ||
        !readDouble(j, "reducedCostGap", next.reducedCostGap, error) ||
        !readInt(j, "maxCycleRetries", next.maxCycleRetries, error) ||
        !readU32(j, "maxStartupGroups", next.maxStartupGroups, error) ||
        !readU32(j, "maxStartupGroupMembers", next.maxStartupGroupMembers, error))
      return false;

    if (!checkKnown(j,
                    {"relativeGap", "absoluteGap", "maxTimeSeconds", "numWorkers",
                     "randomSeed", "objectiveCap", "objectiveUpperBound",
                     "reducedCostGap", "maxCycleRetries", "maxStartupGroups",
                     "maxStartupGroupMembers", "flash"},
                    error))
      return false;

    solverOptions = next;
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  } catch (...) {
    error = "could not parse the solver options";
    return false;
  }
}

std::string plannerOptionsJson() noexcept {
  try {
    json j;
    j["nonoptimal"] = options.nonoptimal;
    j["tagPruning"] = options.tagPruning;
    j["recipePruning"] = options.recipePruning;
    j["directPruning"] = options.directPruning;
    j["substitutionPruning"] = options.substitutionPruning;
    j["seedPruning"] = options.seedPruning;
    j["tagTidy"] = options.tagTidy;
    switch (options.tagInlining) {
      case TagInlineMode::OFF: j["tagInlining"] = "off"; break;
      case TagInlineMode::PRE_PRUNE: j["tagInlining"] = "prePrune"; break;
      case TagInlineMode::QUERY_TIME: j["tagInlining"] = "queryTime"; break;
      case TagInlineMode::BOTH: j["tagInlining"] = "both"; break;
    }
    j["pack"] = {
        {"enabled", options.pack.enabled},
        {"maxPackRecipes", options.pack.maxPackRecipes},
        {"maxPackValue", options.pack.maxPackValue},
        {"maxBranchDepth", options.pack.maxBranchDepth},
        {"maxBranchNodes", options.pack.maxBranchNodes},
        {"maxZeroStockItems", options.pack.maxZeroStockItems},
        {"maxSeconds", options.pack.maxSeconds},
    };
    j["satellite"] = {
        {"enabled", options.satellite.enabled},
        {"maxComponentNodes", options.satellite.maxComponentNodes},
        {"maxIslandNodes", options.satellite.maxIslandNodes},
        {"maxSeconds", options.satellite.maxSeconds},
    };
    j["variantClass"] = {
        {"enabled", options.variantClass.enabled},
        {"maxClassNodes", options.variantClass.maxClassNodes},
        {"maxCandidates", options.variantClass.maxCandidates},
        {"maxSeconds", options.variantClass.maxSeconds},
    };
    j["maxTagMembers"] = options.maxTagMembers;
    j["maxTagPairs"] = options.maxTagPairs;
    j["maxTagCoverWork"] = options.maxTagCoverWork;
    j["maxWitnessPairs"] = options.maxWitnessPairs;
    j["maxPrunePairs"] = options.maxPrunePairs;
    j["maxSiblingRecipes"] = options.maxSiblingRecipes;
    j["maxWitnessProducers"] = options.maxWitnessProducers;
    j["maxSubstitutionWork"] = options.maxSubstitutionWork;
    j["maxSubstitutionCostWork"] = options.maxSubstitutionCostWork;
    j["maxSubstitutionDepth"] = options.maxSubstitutionDepth;
    j["maxCostDepth"] = options.maxCostDepth;
    j["maxSubstitutionGuardItems"] = options.maxSubstitutionGuardItems;
    j["maxSubstitutionGuardTotal"] = options.maxSubstitutionGuardTotal;
    j["maxNeed"] = options.maxNeed;
    j["maxPropIterations"] = options.maxPropIterations;
    return j.dump(2);
  } catch (...) {
    return "{}";
  }
}

std::string solverOptionsJson() noexcept {
  try {
    json j;
    j["relativeGap"] = solverOptions.relativeGap;
    j["absoluteGap"] = solverOptions.absoluteGap;
    j["maxTimeSeconds"] = solverOptions.maxTimeSeconds;
    j["numWorkers"] = solverOptions.numWorkers;
    j["randomSeed"] = solverOptions.randomSeed;
    j["objectiveCap"] = solverOptions.objectiveCap;
    j["objectiveUpperBound"] = solverOptions.objectiveUpperBound;
    j["reducedCostGap"] = solverOptions.reducedCostGap;
    j["maxCycleRetries"] = solverOptions.maxCycleRetries;
    j["maxStartupGroups"] = solverOptions.maxStartupGroups;
    j["maxStartupGroupMembers"] = solverOptions.maxStartupGroupMembers;
    return j.dump(2);
  } catch (...) {
    return "{}";
  }
}

}  // namespace aw
