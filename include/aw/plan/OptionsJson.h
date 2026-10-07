#ifndef AW_OPTIONS_JSON_H
#define AW_OPTIONS_JSON_H

// JSON front door for the two option blocks.
//
// The recipe payload is a binary blob because it is large and cold; the options
// are a few dozen scalars that a human edits in a config file, so they travel
// as JSON and nlohmann parses them on the native side. Field names are the C++
// member names, `camelCase`, and the mapping is one to one in both directions.
//
// The field list is not repeated here: it is the X-macro lists in Options.h and
// Solver.h, which declare the members and this file's readers/writers from one
// source. See OptionsJson.cpp and docs/options.typ.
//
// An update is a partial patch: a key that is absent keeps its current value. A
// key that is present but unknown is an error, because a typo in a config file
// should say so rather than be silently ignored. The whole patch is applied to
// a copy and committed only when every key has parsed, so a failure leaves the
// running configuration untouched.

#include <string>
#include <string_view>

namespace aw {

// Applies a patch to `aw::options`. Returns false and fills `error` on a
// malformed document, an unknown key or a value that does not fit the field.
//
// The registration-time switches (tag inlining and the dominance/pack passes)
// are read once by registerCraftingGraph, so they only take effect on the next
// registration. The query-time ones can be changed at any time.
bool applyPlannerOptionsJson(std::string_view json, std::string &error) noexcept;

// Applies a patch to `aw::solverOptions`. Read once per plan call, so it takes
// effect on the next query.
bool applySolverOptionsJson(std::string_view json, std::string &error) noexcept;

// The current values, as a document that `applyPlannerOptionsJson` accepts
// verbatim. Useful for a `/aw options` command and for the tests.
std::string plannerOptionsJson() noexcept;
std::string solverOptionsJson() noexcept;

}  // namespace aw

#endif
