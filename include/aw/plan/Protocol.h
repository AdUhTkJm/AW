#ifndef AW_PROTOCOL_H
#define AW_PROTOCOL_H

// The wire format between the mod and the planner.
//
// The mod owns one worker thread. It calls `registerCraftingGraph` once per
// recipe reload, then `plan` per query. Everything is serial by construction:
// the mod never starts a second call while one is running, and the kernel
// reports what it is doing through `getKernelStatus` so a UI thread can poll it
// without touching any planner state.
//
// `plan` is a single blob-in / blob-out call, which keeps the JNI surface at
// five methods and makes the whole query atomic. The response is
// self-describing: each executed recipe carries its output handle, output
// amount and inputs, already translated out of subgraph-id space. That is not
// just convenience -- registration-time canonicalization renumbers recipes, so
// a subgraph recipe id is not an index into the blob the mod sent, and
// query-time tag inlining can synthesize recipes that have no blob counterpart
// at all. `Subgraph::itemOrigin` / `recipeOrigin` would not survive the trip;
// the translation has to happen here.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Status.h"

namespace aw {

// What the kernel is doing. Monotone within one call: a caller that sees OK has
// exclusive access to every planner entry point.
//
// The values are part of the ABI and are mirrored in the mod's
// `CraftingGraphKernel` constants.
enum class KernelStatus : int {
  // Nothing running; a call may start.
  OK = 0,
  // Inside registerCraftingGraph: decoding, the parse-time cleanup and the
  // registration-time pruning passes. This is the long one (the pack pass has
  // a 60 s budget by default).
  PREPROCESSING = 1,
  // Inside plan: building the reachable subgraph and solving it. A second
  // submit while this is up would be a concurrent query and is not allowed.
  PLANNING = 2,
};

KernelStatus getKernelStatus() noexcept;

// Raises and lowers the status flag. The JNI entry points bracket their calls
// with a guard; nothing else should need this.
void setKernelStatus(KernelStatus status) noexcept;

// Raises the flag on construction and restores OK on destruction, so an early
// return cannot strand the kernel in a busy state. Used across a call that may
// throw on the Java side of the bridge.
struct KernelStatusGuard {
  explicit KernelStatusGuard(KernelStatus status) noexcept {
    setKernelStatus(status);
  }
  ~KernelStatusGuard() noexcept {
    setKernelStatus(KernelStatus::OK);
  }
  KernelStatusGuard(const KernelStatusGuard &) = delete;
  KernelStatusGuard &operator=(const KernelStatusGuard &) = delete;
};

// A decoded `plan` request.
//
// `target` is a 1-based resource handle, and `inventory` is sparse: only the
// handles the player actually holds. Everything the solver needs is in the
// registered graph, so the request stays small.
struct PlanRequest {
  Handle target = 0;
  Amount amount = 0;
  // Available workstations, ascending handles.
  aw::vector<Handle> workstations;
  // Stock, ascending handles, parallel to `stockAmounts`.
  aw::vector<Handle> stockHandles;
  aw::vector<Amount> stockAmounts;
};

// One recipe the plan executes, described in source-resource terms.
struct PlanRecipeUse {
  Handle outputHandle = 0;
  Amount outputAmount = 0;
  // Byproducts, ascending handles, parallel to `byproductAmounts`. Empty for a
  // single-output recipe.
  aw::vector<Handle> byproductHandles;
  aw::vector<Amount> byproductAmounts;
  // Inputs, ascending handles, parallel to `inputAmounts`.
  aw::vector<Handle> inputHandles;
  aw::vector<Amount> inputAmounts;
  // Number of times the recipe is executed. Always >= 1: zero-use recipes are
  // left out of the response.
  Amount count = 0;
};

struct PlanResponse {
  PlanStatus status = PlanStatus::INVALID_INPUT;
  bool provenOptimal = false;
  double gap = 0.0;
  double bestBound = 0.0;
  int64_t numConflicts = 0;
  int64_t numBranches = 0;
  uint32_t fixedColumns = 0;
  // Ascending by output handle, then by recipe, as the subgraph walked them.
  aw::vector<PlanRecipeUse> uses;
};

// Decodes the request blob described in docs/jni.md. Returns false and fills
// `error` when the blob is malformed, truncated, non-ascending or names a
// handle outside the registered graph.
//
// No exceptions: the callers on the C++ side are compiled with
// -fno-exceptions.
bool decodePlanRequest(std::span<const std::byte> bytes, PlanRequest &out,
                       std::string &error) noexcept;

// Runs the query: reachableSubgraph, then planCrafting. Never throws. On a
// failure the returned status is INVALID_INPUT and `uses` is empty.
PlanResponse runPlan(const PlanRequest &request) noexcept;

// Serialises a response. Cheap even for a full subgraph: only the recipes the
// plan actually executes are written.
aw::vector<std::byte> encodePlanResponse(const PlanResponse &response) noexcept;

// decodePlanRequest + runPlan + encodePlanResponse, with a status guard. On a
// malformed request returns an empty blob and fills `error`.
aw::vector<std::byte> planBlob(std::span<const std::byte> request,
                               std::string &error) noexcept;

}  // namespace aw

#endif
