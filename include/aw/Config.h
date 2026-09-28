#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>
#include <cstdlib>

namespace aw {

// ---------------------------------------------------------------------------
// Tag Pruning
// ---------------------------------------------------------------------------

// Only prune tags with at most this member count.
constexpr size_t MAX_TAG_MEMBERS = 1024;

// Only prune tags with at most this amount of alive-pairs.
constexpr uint64_t MAX_TAG_PAIRS = 4'000'000;

// Global ceiling on the number of column-cover tests.
constexpr uint64_t MAX_TAG_COVER_WORK = 64'000'000;

// Ceiling on the gating pairs loop (a) contributes.
constexpr uint64_t MAX_WITNESS_PAIRS = 4'000'000;

// Ceiling on the pair universe. The seeds are always kept; the transitive
// closure stops once the total reaches this. It is a heuristic bound: on the
// 12k-item NAST pack it keeps registration near one second, and running out
// only leaves pairs out of the relation, which can only make the pass prune
// less. Paying more here mostly buys the deeper real-input chains, so the
// marginal tag edges fall off quickly past this point.
constexpr uint64_t MAX_PRUNE_PAIRS = 4'000'000;

// A real item with more recipes than this is left unpruned. The composite pass
// compares every pair of an item's recipes; ATM10 has items with 35k recipes,
// where comparing all pairs is both quadratic in time and gigabytes of
// adjacency. Keeping recipes can only cost the planner time, never correctness.
constexpr size_t MAX_SIBLING_RECIPES = 2048;

// A witness input produced by more recipes than this is skipped, because the
// composite test has to hold for every producer of the witness. Skipping a
// witness just leaves its recipe without a guard. ATM10 has 8 items above this
// (up to 35k producers); the test graphs top out at 1985.
constexpr size_t MAX_WITNESS_PRODUCERS = 2048;

// ---------------------------------------------------------------------------
// Substitution Pruning
// ---------------------------------------------------------------------------

// Substitution budgets. The pass compares every pair of an item's recipes and
// asks the cost relation about the pairs that could cover a deficit, so both
// the query count and the memo it fills are bounded. Running out only leaves
// recipes unpruned. A collapse also produces a stock guard: a real input is a
// single item, but a tag expands into its members, and a catch-all tag is left
// alone rather than recorded as a per-recipe guard list.
constexpr uint64_t MAX_SUBSTITUTION_WORK = 64'000'000;
constexpr uint64_t MAX_SUBSTITUTION_COST_WORK = 256'000'000;
constexpr uint32_t MAX_SUBSTITUTION_DEPTH = 4;
constexpr uint32_t MAX_COST_DEPTH = 16;
constexpr size_t MAX_COST_MEMO = 1'000'000;
constexpr size_t MAX_SUBSTITUTION_GUARD_ITEMS = 256;
constexpr size_t MAX_SUBSTITUTION_GUARD_TOTAL = 1'000'000;

// ---------------------------------------------------------------------------
// Pack Pruning
// ---------------------------------------------------------------------------
constexpr int64_t MAX_NEED = int64_t{1} << 40;
constexpr int MAX_PROP_ITERATIONS = 4096;

}

#endif
