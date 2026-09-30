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

// Budget for the multi-item "wasteful pack" certificates. The defaults keep
// registration bounded on the full 12k-item graphs; running out of budget only
// turns a search branch into a leaf, which weakens the pack but never makes it
// unsound.
struct PackPruneOptions {
  bool enabled = true;

  // Pack size: at most this many distinct recipes, and at most this total
  // count across the pack.
  uint32_t maxPackRecipes = 128;
  int64_t maxPackValue = 1024;

  // R3 search budget. Exceeding either turns the node into a leaf.
  uint32_t maxBranchDepth = 8;
  uint32_t maxBranchNodes = 4096;

  // Cap on the pack's zero-stock set. A derivation that needs more is dropped.
  uint32_t maxZeroStockItems = 32;

  // Wall-clock budget for the whole pass. <= 0 means no limit.
  double maxSeconds = 60.0;
};

// Query-time re-pruning of the reachable subgraph. Off by default; it is an
// experiment.
//
// The registration-time dominance passes run on the whole graph, where the
// workstation and stock guards cannot be resolved. By the time reachability has
// finished, the subgraph holds only recipes the query can actually run, so the
// cheap passes can be run again on it: the composite and direct passes no
// longer need a workstation guard (a dominator that is present is runnable), and
// a single-member tag inlined just before them removes the pseudo-node the
// composite pass could not see through. Only recipes with a real output are
// dropped here; the tag edges keep their own passes.
struct SubgraphRepruneOptions {
  bool enabled = false;

  // Re-run the pack certificates on the subgraph too.
  bool pack = true;

  // Use the exact relation instead of the nonoptimal relaxation. Exact is the
  // safe default; the relaxation was only argued for one application on the
  // whole graph, see Prune.cpp.
  bool exact = true;

  // Wall-clock budget for the pack pass on the subgraph. <= 0 means no limit.
  double packSeconds = 0.3;
};

// Budget for the satellite-elimination pass (docs/algorithm.typ, "孤岛消除").
//
// The pass runs inside reachableSubgraph, where the target and the inventory
// are known: it looks for groups of recipes whose only net output to the rest
// of the subgraph is one item, hold no stock inside, and cannot repay that
// item, and drops them. The undirected articulation components and the wider
// "input-leaking" islands found by the escape enumeration are two ways to find
// such groups. Everything else is a budget, and running out of one only means a
// group is left alone.
struct SatellitePruneOptions {
  bool enabled = true;

  // A component with more nodes than this is skipped. A wide component is
  // never a "variants hanging off a basic form" island, and its LP would
  // dominate the pass.
  uint32_t maxComponentNodes = 512;

  // Budget for the generalized escape enumeration. The island of a single
  // escape can be much wider than an undirected component (the maximal closed
  // set is taken), so it has its own, larger bound; `maxComponentNodes` still
  // bounds the undirected pass. The count is items plus recipes.
  uint32_t maxIslandNodes = 4096;

  // Wall-clock budget for one round of the pass, on top of the reachability
  // walk. reachableSubgraph runs at most four rounds. <= 0 means no limit.
  double maxSeconds = 0.05;
};

// Process-wide planner options. Minecraft is serial and the crafting graph is
// a singleton, so there is no thread safety to worry about.
//
// This is the single home for every pruning switch and tuning bound. The
// registration-time knobs (tag inlining, the dominance and pack passes) are
// read once by registerCraftingGraph, so they must be set before registering a
// graph; the query-time ones (satellite elimination, seed pruning and the
// query-time half of tag inlining) can be flipped at any time.
struct Options {
  // Integrality relaxation for the dominance passes. On by default. Exact mode
  // (false) keeps every guard the relaxed relation drops and computes the
  // exact cost relation instead; see Prune.cpp for the precise difference.
  bool nonoptimal = true;

  // Dominance pruning switches. All on by default; each pass exists so it can
  // be A/B tested against the unpruned graph.
  bool tagPruning = true;
  bool recipePruning = true;
  bool directPruning = true;
  bool substitutionPruning = true;

  // Seed-reachability pruning. Drops every recipe with an input that cannot be
  // produced from the player's stock; input-less recipes are the seeds. Such a
  // recipe can never be executed, because the balance the solver sees is a net
  // condition and does not know that a consumed item has to exist before the
  // recipe runs. This also covers the acyclic dead branches that dead-node
  // cleanup used to remove -- a zero-stock item with no surviving producer is
  // simply never attainable -- so there is no separate dead-node pass. Sound
  // (never removes a realizable recipe) and toggleable. See docs/algorithm.typ,
  // "启动可达性".
  bool seedPruning = true;

#ifdef AW_PROFILE_PRUNING
  bool outputPruningProfile = false;
#endif

  // Where single-use tag inlining runs. Read at registration time.
  TagInlineMode tagInlining = TagInlineMode::OFF;

  // The single-member half of the inliner, see inlineSingleUseTagsCore. On by
  // default; it is a plain substitution and never grows the recipe list, but it
  // is separable because the many-member half has a very different cost.
  bool inlineSingleMemberTags = true;

  // Pack and satellite budgets. Their `enabled` field is the switch for the
  // corresponding pass.
  PackPruneOptions pack;
  SatellitePruneOptions satellite;

  // Query-time re-pruning of the reachable subgraph. Read by
  // `reachableSubgraph` on every query, so it can be flipped at any time.
  SubgraphRepruneOptions reprune;

  // -------------------------------------------------------------------------
  // Tag-pruning budgets.
  // -------------------------------------------------------------------------

  // Only prune tags with at most this member count.
  size_t maxTagMembers = 1024;

  // Only prune tags with at most this amount of alive-pairs.
  uint64_t maxTagPairs = 4'000'000;

  // Global ceiling on the number of column-cover tests.
  uint64_t maxTagCoverWork = 64'000'000;

  // Ceiling on the gating pairs loop (a) contributes.
  uint64_t maxWitnessPairs = 4'000'000;

  // Ceiling on the pair universe. The seeds are always kept; the transitive
  // closure stops once the total reaches this. It is a heuristic bound: on the
  // 12k-item NAST pack it keeps registration near one second, and running out
  // only leaves pairs out of the relation, which can only make the pass prune
  // less. Paying more here mostly buys the deeper real-input chains, so the
  // marginal tag edges fall off quickly past this point.
  uint64_t maxPrunePairs = 4'000'000;

  // A real item with more recipes than this is left unpruned. The composite
  // pass compares every pair of an item's recipes; ATM10 has items with 35k
  // recipes, where comparing all pairs is both quadratic in time and gigabytes
  // of adjacency. Keeping recipes can only cost the planner time, never
  // correctness.
  size_t maxSiblingRecipes = 2048;

  // A witness input produced by more recipes than this is skipped, because the
  // composite test has to hold for every producer of the witness. Skipping a
  // witness just leaves its recipe without a guard. ATM10 has 8 items above
  // this (up to 35k producers); the test graphs top out at 1985.
  size_t maxWitnessProducers = 2048;

  // -------------------------------------------------------------------------
  // Substitution-pruning budgets.
  // -------------------------------------------------------------------------

  // Substitution budgets. The pass compares every pair of an item's recipes and
  // asks the cost relation about the pairs that could cover a deficit. Running
  // out only leaves recipes unpruned. A collapse also produces a stock guard: a
  // real input is a single item, but a tag expands into its members, and a
  // catch-all tag is left alone rather than recorded as a per-recipe guard list.
  //
  // The cost relation is solved one witness at a time by an upward closure, so
  // maxSubstitutionCostWork counts closure steps and maxCostMemo bounds the
  // total number of true pairs kept. The true side of the relation is tiny (a
  // few thousand pairs on the large packs) while the false bulk is huge, which
  // is why the closure only ever visits the true side.
  uint64_t maxSubstitutionWork = 64'000'000;
  uint64_t maxSubstitutionCostWork = 256'000'000;
  uint32_t maxSubstitutionDepth = 4;
  // Longest derivation chain the cost relation accepts, taking one qualified
  // input per recipe.
  uint32_t maxCostDepth = 16;
  size_t maxSubstitutionGuardItems = 256;
  size_t maxSubstitutionGuardTotal = 1'000'000;

  // -------------------------------------------------------------------------
  // Pack-pruning budgets.
  // -------------------------------------------------------------------------
  int64_t maxNeed = int64_t{1} << 40;
  int maxPropIterations = 4096;
};

// The singleton option block.
extern Options options;

// The solver budget the JNI plan entry point uses (see aw/Protocol.h). It lives
// apart from `options` because it is a different struct with a different
// lifetime: `options` holds the planner's own pruning switches, and the solver
// budget is re-read per query so the mod config can retune it without
// re-registering the graph.
extern solver::Options solverOptions;

}  // namespace aw

#endif
