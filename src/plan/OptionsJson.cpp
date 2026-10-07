#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <type_traits>

#include "aw/plan/Options.h"
#include "aw/plan/OptionsJson.h"
#include "aw/plan/Solver.h"

namespace aw {

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// The option codec.
//
// A field is declared once, in one of the X-macro lists in Options.h and
// Solver.h, as `X(type, member, default)`. The list expands into the struct's
// members (see the headers) and into `readOption`/`writeOption` calls below.
// The reader is chosen from the member's type, so there is no per-field
// boilerplate left to keep in sync. docs/options.typ holds the per-field prose.
//
// A reader returns true when the key is absent, so a patch only overrides what
// it names; an update is a partial patch on purpose.
// ---------------------------------------------------------------------------

// Enum <-> JSON name table. A newly serialized enum needs one specialization.
template <typename E>
struct EnumCodec;

template <>
struct EnumCodec<TagInlineMode> {
  static constexpr std::array<std::pair<std::string_view, TagInlineMode>, 4> kValues{{
      {"off", TagInlineMode::OFF},
      {"prePrune", TagInlineMode::PRE_PRUNE},
      {"queryTime", TagInlineMode::QUERY_TIME},
      {"both", TagInlineMode::BOTH},
  }};
};

// Defined once, then expanded by each list below. `j`, `out` and `error` are
// the enclosing function's parameters; the definitions name them that way so
// the macro needs no arguments beyond the field.
#define AW_FIELD_READ(type, name, default) \
  if (!readOption(j, #name, out.name, error)) return false;
#define AW_FIELD_WRITE(type, name, default) writeOption(j, #name, out.name);
#define AW_FIELD_KEY(type, name, default) #name,
#define AW_OBJECT_READ(type, name) \
  if (!readOption(j, #name, out.name, error)) return false;
#define AW_OBJECT_WRITE(type, name) writeOption(j, #name, out.name);
#define AW_OBJECT_KEY(type, name) #name,

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

// The nested option blocks read and write as objects. Declared here so the
// generic codec below can recurse into them; defined after it.
bool readObject(const json &j, PackPruneOptions &out, std::string &error);
bool readObject(const json &j, SatellitePruneOptions &out, std::string &error);
bool readObject(const json &j, VariantClassPruneOptions &out, std::string &error);
bool readObject(const json &j, TagExclusivePruneOptions &out, std::string &error);
bool readObject(const json &j, VariantFoldPruneOptions &out, std::string &error);
json writeObject(const PackPruneOptions &out);
json writeObject(const SatellitePruneOptions &out);
json writeObject(const VariantClassPruneOptions &out);
json writeObject(const TagExclusivePruneOptions &out);
json writeObject(const VariantFoldPruneOptions &out);

// The one reader. A missing key leaves `out` untouched.
template <typename T>
bool readOption(const json &j, const char *key, T &out, std::string &error) {
  const auto it = j.find(key);
  if (it == j.end())
    return true;

  if constexpr (std::is_class_v<T>) {
    // A nested object is optional; when present it must really be an object.
    if (!it->is_object()) {
      error = std::string(key) + " must be an object";
      return false;
    }
    return readObject(*it, out, error);
  } else if constexpr (std::is_same_v<T, bool>) {
    if (!it->is_boolean()) {
      error = std::string(key) + " must be a boolean";
      return false;
    }
    out = it->get<bool>();
    return true;
  } else if constexpr (std::is_enum_v<T>) {
    if (it->is_string()) {
      const std::string text = it->get<std::string>();
      for (const auto &[name, value] : EnumCodec<T>::kValues)
        if (text == name) {
          out = value;
          return true;
        }
      error = std::string(key) + " is not a known mode";
      return false;
    }
    // The raw ordinal is accepted too, which keeps the Java side free to send
    // either a name or a number.
    if (!it->is_number_integer() && !it->is_number_unsigned()) {
      error = std::string(key) + " must be a string or an integer";
      return false;
    }
    if (!it->is_number_unsigned() ||
        it->get<uint64_t>() < (uint64_t) EnumCodec<T>::kValues.size()) {
      const int64_t ordinal = it->get<int64_t>();
      if (ordinal >= 0 && ordinal < (int64_t) EnumCodec<T>::kValues.size()) {
        out = EnumCodec<T>::kValues[(size_t) ordinal].second;
        return true;
      }
    }
    error = std::string(key) + " is out of range";
    return false;
  } else if constexpr (std::is_floating_point_v<T>) {
    if (!it->is_number()) {
      error = std::string(key) + " must be a number";
      return false;
    }
    out = (T) it->get<double>();
    return true;
  } else if constexpr (std::is_integral_v<T>) {
    if (!it->is_number_integer() && !it->is_number_unsigned()) {
      error = std::string(key) +
              (std::is_signed_v<T> ? " must be an integer"
                                   : " must be a non-negative integer");
      return false;
    }
    if (it->is_number_unsigned()) {
      const uint64_t value = it->get<uint64_t>();
      if (value > (uint64_t) std::numeric_limits<T>::max()) {
        error = std::string(key) + " does not fit";
        return false;
      }
      out = (T) value;
      return true;
    }
    const int64_t value = it->get<int64_t>();
    if constexpr (std::is_signed_v<T>) {
      if (value < (int64_t) std::numeric_limits<T>::min() ||
          value > (int64_t) std::numeric_limits<T>::max()) {
        error = std::string(key) + " does not fit";
        return false;
      }
    } else {
      if (value < 0) {
        error = std::string(key) + " must not be negative";
        return false;
      }
      if ((uint64_t) value > (uint64_t) std::numeric_limits<T>::max()) {
        error = std::string(key) + " does not fit";
        return false;
      }
    }
    out = (T) value;
    return true;
  } else {
    static_assert(!sizeof(T), "no JSON codec for this option type");
  }
}

template <typename T>
void writeOption(json &j, const char *key, const T &value) {
  if constexpr (std::is_class_v<T>) {
    j[key] = writeObject(value);
  } else if constexpr (std::is_enum_v<T>) {
    for (const auto &[name, candidate] : EnumCodec<T>::kValues)
      if (candidate == value) {
        j[key] = std::string(name);
        return;
      }
    j[key] = nullptr;
  } else {
    j[key] = value;
  }
}

bool readObject(const json &j, PackPruneOptions &out, std::string &error) {
  AW_PACK_OPTION_FIELDS(AW_FIELD_READ)
  return checkKnown(j, {AW_PACK_OPTION_FIELDS(AW_FIELD_KEY)}, error);
}

bool readObject(const json &j, SatellitePruneOptions &out, std::string &error) {
  AW_SATELLITE_OPTION_FIELDS(AW_FIELD_READ)
  return checkKnown(j, {AW_SATELLITE_OPTION_FIELDS(AW_FIELD_KEY)}, error);
}

bool readObject(const json &j, VariantClassPruneOptions &out, std::string &error) {
  AW_VARIANT_CLASS_OPTION_FIELDS(AW_FIELD_READ)
  return checkKnown(j, {AW_VARIANT_CLASS_OPTION_FIELDS(AW_FIELD_KEY)}, error);
}

bool readObject(const json &j, TagExclusivePruneOptions &out, std::string &error) {
  AW_TAG_EXCLUSIVE_OPTION_FIELDS(AW_FIELD_READ)
  return checkKnown(j, {AW_TAG_EXCLUSIVE_OPTION_FIELDS(AW_FIELD_KEY)}, error);
}

bool readObject(const json &j, VariantFoldPruneOptions &out, std::string &error) {
  AW_VARIANT_FOLD_OPTION_FIELDS(AW_FIELD_READ)
  return checkKnown(j, {AW_VARIANT_FOLD_OPTION_FIELDS(AW_FIELD_KEY)}, error);
}

json writeObject(const PackPruneOptions &out) {
  json j;
  AW_PACK_OPTION_FIELDS(AW_FIELD_WRITE)
  return j;
}

json writeObject(const SatellitePruneOptions &out) {
  json j;
  AW_SATELLITE_OPTION_FIELDS(AW_FIELD_WRITE)
  return j;
}

json writeObject(const VariantClassPruneOptions &out) {
  json j;
  AW_VARIANT_CLASS_OPTION_FIELDS(AW_FIELD_WRITE)
  return j;
}

json writeObject(const TagExclusivePruneOptions &out) {
  json j;
  AW_TAG_EXCLUSIVE_OPTION_FIELDS(AW_FIELD_WRITE)
  return j;
}

json writeObject(const VariantFoldPruneOptions &out) {
  json j;
  AW_VARIANT_FOLD_OPTION_FIELDS(AW_FIELD_WRITE)
  return j;
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
    Options out = options;

    AW_PLANNER_OPTION_FIELDS(AW_FIELD_READ)
    AW_PLANNER_OPTION_OBJECTS(AW_OBJECT_READ)

    {
      // Always accepted, even when the build has no profiling pass to switch:
      // the mod sends one options document on every platform. It is not
      // written back, so it is not part of the macro lists.
      [[maybe_unused]] bool ignored = false;
      if (!readOption(j, "outputPruningProfile",
#ifdef AW_PROFILE_PRUNING
                      out.outputPruningProfile,
#else
                      ignored,
#endif
                      error))
        return false;
    }

    // `deadNodePruning` and `maxCostMemo` are retired knobs. They stay in the
    // known set so an older client that still sends them is not rejected; the
    // values are ignored, and neither is written back by plannerOptionsJson.
    if (!checkKnown(j,
                    {AW_PLANNER_OPTION_FIELDS(AW_FIELD_KEY)
                     AW_PLANNER_OPTION_OBJECTS(AW_OBJECT_KEY)
                     "deadNodePruning", "maxCostMemo", "outputPruningProfile"},
                    error))
      return false;

    options = out;
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

    solver::Options out = solverOptions;

    AW_SOLVER_OPTION_FIELDS(AW_FIELD_READ)

    // `flash` is no longer a solver option: it is per-solve state
    // (solver::State) seeded from the process-wide `aw::options.flash`. It
    // stays in the known set so an older client that still sends it is not
    // rejected; the value is ignored.
    if (!checkKnown(j, {AW_SOLVER_OPTION_FIELDS(AW_FIELD_KEY) "flash"}, error))
      return false;

    solverOptions = out;
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
    const Options &out = options;
    AW_PLANNER_OPTION_FIELDS(AW_FIELD_WRITE)
    AW_PLANNER_OPTION_OBJECTS(AW_OBJECT_WRITE)
    return j.dump(2);
  } catch (...) {
    return "{}";
  }
}

std::string solverOptionsJson() noexcept {
  try {
    json j;
    const solver::Options &out = solverOptions;
    AW_SOLVER_OPTION_FIELDS(AW_FIELD_WRITE)
    return j.dump(2);
  } catch (...) {
    return "{}";
  }
}

}  // namespace aw
