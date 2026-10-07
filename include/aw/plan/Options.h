#ifndef AW_OPTIONS_H
#define AW_OPTIONS_H

#include <cstddef>
#include <cstdint>

#include "aw/plan/Solver.h"

namespace aw {

// Where single-use tag inlining runs. Off by default; it is an experiment.
//
// A tag T with member edges `T <- m1 .. T <- mn` and exactly one real consumer
// `R: a <- T, ...` (with the tag consumed once at amount 1) is replaced by the
// n real recipes `a <- m1, ...`. The tag node and its n edges disappear and the
// recipe count drops by one. A larger consumed amount is left alone: it could
// be served by mixed members, which a homogeneous expansion cannot express.
//
// Inlining changes the objective, because the removed tag edges were charged 1
// each in the solver model. Tag edges are conceptually free; the unit cost is
// only there to stop the solver turning arbitrary amounts of items into tags.
// So an inlined optimum is meant to be lower, and the two optima are not
// compared for equality.
enum class TagInlineMode : uint8_t {
  // No inlining.
  OFF = 0,
  // Flatten every single-use tag at registration time, using every member
  // edge, before the dominance passes run. This can re-introduce members that
  // tag pruning would have dropped, so it can grow the query-time subgraph, but
  // it gives the passes the flattest graph to reason about.
  PRE_PRUNE = 1,
  // Flatten single-use tags at query time, inside reachableSubgraph, after the
  // walk and every pruning pass. Only recipes that survived are inlined, so a
  // tag's surviving member edges -- which include a dominated member the player
  // holds stock of -- are exactly the members that can still be spent. This
  // keeps the subgraph from growing and keeps stock usable.
  QUERY_TIME = 2,
  // PRE_PRUNE followed by QUERY_TIME: persistent single-use tags are flattened
  // once for the whole corpus, and the subgraph-specific ones are flattened per
  // query.
  BOTH = 3,
};

// Pack-prune budget. The pass looks for a multi-item "wasteful pack"
// certificate at registration time. The defaults keep it bounded on the full
// 12k-item graphs, and running out of budget only turns a search branch into a
// leaf, which weakens the pack but never makes it unsound. See docs/options.typ.
//
//   X(type, member, default)
#define AW_PACK_OPTION_FIELDS(X)    \
  X(bool, enabled, true)            \
  X(uint32_t, maxPackRecipes, 128)  \
  X(int64_t, maxPackValue, 1024)    \
  X(uint32_t, maxBranchDepth, 8)    \
  X(uint32_t, maxBranchNodes, 4096) \
  X(uint32_t, maxZeroStockItems, 32) \
  X(double, maxSeconds, 60.0)

struct PackPruneOptions {
#define AW_OPTION_MEMBER(type, name, default) type name = default;
  AW_PACK_OPTION_FIELDS(AW_OPTION_MEMBER)
#undef AW_OPTION_MEMBER
};

// Query-time satellite elimination, run inside reachableSubgraph where the
// target and the inventory are known: it drops groups of recipes whose only net
// output to the rest of the subgraph is one item, hold no stock inside, and
// cannot repay that item. Running out of a budget only leaves a group alone,
// which can weaken the pass but never makes it unsound. See docs/options.typ.
// The undirected articulation components and the wider "input-leaking" islands
// found by the escape enumeration are the two ways to find such a group.
#define AW_SATELLITE_OPTION_FIELDS(X)   \
  X(bool, enabled, true)                \
  X(bool, enabledOnFlash, false)        \
  X(uint32_t, maxComponentNodes, 512)   \
  X(uint32_t, maxIslandNodes, 4096)     \
  X(double, maxSeconds, 0.05)

struct SatellitePruneOptions {
#define AW_OPTION_MEMBER(type, name, default) type name = default;
  AW_SATELLITE_OPTION_FIELDS(AW_OPTION_MEMBER)
#undef AW_OPTION_MEMBER
};

// Interchangeable-variant (tag-orbit) elimination, run inside
// reachableSubgraph on the assembled subgraph. For every tag whose members
// could be an interchangeable class it shrinks the class until it is closed
// under the conversions, then drops the class's internal conversions. Running
// out of budget or hitting a bound only leaves a class alone, so it can weaken
// the pass but cannot make it unsound. See docs/options.typ.
#define AW_VARIANT_CLASS_OPTION_FIELDS(X) \
  X(bool, enabled, true)                  \
  X(uint32_t, maxClassNodes, 4096)        \
  X(uint32_t, maxCandidates, 4096)        \
  X(double, maxSeconds, 0.05)

struct VariantClassPruneOptions {
#define AW_OPTION_MEMBER(type, name, default) type name = default;
  AW_VARIANT_CLASS_OPTION_FIELDS(AW_OPTION_MEMBER)
#undef AW_OPTION_MEMBER
};

// Tag-exclusive producer elimination, run inside reachableSubgraph on the
// assembled subgraph. A real item is tag-exclusive for a tag T when every
// consumer of it is a member edge of T. A real recipe whose outputs are all
// exclusive for the same T is dropped when it produces no more M(T)-capacity
// than it consumes. The target is the only guard; the inventory needs none.
// Running out of budget only leaves recipes alone, so it can weaken the pass
// but cannot make it unsound. See docs/options.typ.
#define AW_TAG_EXCLUSIVE_OPTION_FIELDS(X) \
  X(bool, enabled, true)                  \
  X(double, maxSeconds, 0.05)

struct TagExclusivePruneOptions {
#define AW_OPTION_MEMBER(type, name, default) type name = default;
  AW_TAG_EXCLUSIVE_OPTION_FIELDS(AW_OPTION_MEMBER)
#undef AW_OPTION_MEMBER
};

// Variant-folding elimination, run inside reachableSubgraph on the assembled
// subgraph. It looks for the decorative twins of a resource: items that a tag,
// or a chain of "same recipe, other item" copies, makes interchangeable with a
// more mundane base, and that no real recipe outside that structure ever
// demands by name. Such an item is folded into its base, and every recipe that
// mentions it is dropped. The fold is only applied when a signature check
// proves that every dropped recipe can be replayed by recipes that survive, so
// an exhausted budget or a failed check only means nothing is pruned. See
// docs/algorithm.typ (多出口的推广) and VariantFoldPrune.cpp; the fields are in
// docs/options.typ.
#define AW_VARIANT_FOLD_OPTION_FIELDS(X) \
  X(bool, enabled, true)                 \
  X(uint32_t, maxFoldItems, 4096)        \
  X(double, maxSeconds, 1.0)

struct VariantFoldPruneOptions {
#define AW_OPTION_MEMBER(type, name, default) type name = default;
  AW_VARIANT_FOLD_OPTION_FIELDS(AW_OPTION_MEMBER)
#undef AW_OPTION_MEMBER
};

// Query-time re-pruning of the reachable subgraph. Not serialized; it is a
// development switch, see tests/plan/Main.cpp.
struct SubgraphRepruneOptions {
  bool enabled = true;
  bool enabledOnFlash = false;
  // Use the exact relation instead of the nonoptimal relaxation. The
  // nonoptimal mode might cause solution downgrade.
  bool exact = true;
};

// The planner's own pruning switches and budgets. The X-macro lists below are
// the only place a serialized field is declared: this struct expands them into
// members, and OptionsJson.cpp expands the same lists into the JSON reader,
// writer and known-key set. A field is therefore written once. See
// docs/options.typ for what each one means.
//
// Minecraft is serial and the crafting graph is a singleton, so there is no
// thread safety to worry about. The registration-time knobs (tag inlining and
// the dominance and pack passes) are read once by registerCraftingGraph, so
// they must be set before registering a graph; the query-time ones (satellite
// elimination, seed pruning and the query-time half of tag inlining) can be
// flipped at any time.
//
//   X(type, member, default)
#define AW_PLANNER_OPTION_FIELDS(X)                        \
  X(bool, nonoptimal, true)                                \
  X(bool, tagPruning, true)                                \
  X(bool, recipePruning, true)                             \
  X(bool, directPruning, true)                             \
  X(bool, substitutionPruning, true)                       \
  X(bool, seedPruning, true)                               \
  X(bool, tagTidy, true)                                   \
  X(bool, flash, false)                                    \
  X(bool, flashProbe, true)                                \
  X(TagInlineMode, tagInlining, TagInlineMode::QUERY_TIME) \
  X(size_t, maxTagMembers, 1024)                           \
  X(uint64_t, maxTagPairs, 4'000'000)                      \
  X(uint64_t, maxTagCoverWork, 64'000'000)                 \
  X(uint64_t, maxWitnessPairs, 4'000'000)                  \
  X(uint64_t, maxPrunePairs, 4'000'000)                    \
  X(size_t, maxSiblingRecipes, 2048)                       \
  X(size_t, maxWitnessProducers, 2048)                     \
  X(uint64_t, maxSubstitutionWork, 64'000'000)             \
  X(uint64_t, maxSubstitutionCostWork, 256'000'000)        \
  X(uint32_t, maxSubstitutionDepth, 4)                     \
  X(uint32_t, maxCostDepth, 16)                            \
  X(size_t, maxSubstitutionGuardItems, 256)                \
  X(size_t, maxSubstitutionGuardTotal, 1'000'000)          \
  X(int64_t, maxNeed, int64_t{1} << 40)                    \
  X(int, maxPropIterations, 4096)

// The nested option blocks, read as JSON objects.
//   X(type, member)
#define AW_PLANNER_OPTION_OBJECTS(X)          \
  X(PackPruneOptions, pack)                   \
  X(SatellitePruneOptions, satellite)         \
  X(VariantClassPruneOptions, variantClass)   \
  X(TagExclusivePruneOptions, tagExclusive)   \
  X(VariantFoldPruneOptions, variantFold)

struct Options {
#define AW_OPTION_MEMBER(type, name, default) type name = default;
  AW_PLANNER_OPTION_FIELDS(AW_OPTION_MEMBER)
#undef AW_OPTION_MEMBER
#define AW_OPTION_OBJECT(type, name) type name;
  AW_PLANNER_OPTION_OBJECTS(AW_OPTION_OBJECT)
#undef AW_OPTION_OBJECT

  // The single-member half of the inliner, see inlineSingleUseTagsCore. On by
  // default; it is a plain substitution and never grows the recipe list, but
  // it is separable because the many-member half has a very different cost.
  // Not serialized; it is an experiment.
  bool inlineSingleMemberTags = true;

  // Query-time re-pruning of the reachable subgraph. Read by
  // `reachableSubgraph` on every query, so it can be flipped at any time.
  // Not serialized.
  SubgraphRepruneOptions reprune;

#ifdef AW_PROFILE_PRUNING
  // Profiling switches, compiled in only for the desktop development build.
  // Not serialized.
  bool outputPruningProfile = false;
  bool outputRepruningProfile = false;
#endif
};

// The singleton option block.
extern Options options;

// The solver budget the JNI plan entry point uses (see aw/Protocol.h). It lives
// apart from `options` because it is a different struct with a different
// lifetime: `options` holds the planner's own pruning switches, and the solver
// budget is re-read per query so the mod config can retune it without
// re-registering the graph. Flash is not here; it is per-solve state, see
// solver::State.
extern solver::Options solverOptions;

}  // namespace aw

#endif
