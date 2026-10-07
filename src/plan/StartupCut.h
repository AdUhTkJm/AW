#ifndef AW_PLAN_STARTUP_CUT_H
#define AW_PLAN_STARTUP_CUT_H

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Solver.h"

#include <span>

namespace aw {

// Eager startup cuts for the 1- and 2-cycles of `sub`.
//
// Appends to `groups`, up to `maxGroups` of them, one group per connected
// component of the net-gain mutual-consumption relation and per net-gain
// self-loop. `maxMembers` skips a component too large to cut usefully, which is
// how a whole conversion soup is kept out of the model. `solver::State::
// EntryGroup` says what a group means and why every fireable plan obeys it.
//
// For a component with exactly two members it also appends to `seeds`, up to
// `maxGroups` of them, the *joint* cut of the ordered pair. An entry group
// prices one firing against one row at a time, which a two-recipe cycle can
// pass and still not start; the seed cut weighs the two rows against each other
// instead. `solver::State::SeedCut` carries the derivation.
//
// The cuts are plan-independent, so they can be posted before the first solve.
// That is the point: the unstartable plan the post-solve fireability check used
// to reject is excluded while the solver is still searching, which saves a
// whole rejected solve, and on instances where no firing sequence exists at all
// the augmented model reports INFEASIBLE instead of the weaker
// CYCLE_UNFULFILLED.
//
// The relation is *net-gain*: a pair is kept when summing its columns leaves a
// positive net entry somewhere, which is what lets the balance run it from
// nothing. Exact inverse pairs (compress and decompress) sum to zero and are
// skipped, as are cycles that destroy an item. The filter only decides
// relevance, so a budget that runs out early is safe.
void buildStartupCuts(const Subgraph &sub, aw::vector<solver::State::EntryGroup> &groups,
                      aw::vector<solver::State::SeedCut> &seeds, uint32_t maxGroups,
                      uint32_t maxMembers) noexcept;

// Builds one entry group over `columns`: every member is a candidate entry, and
// the group's needs are the merged gross inputs of all of them.
//
// `buildStartupCuts` uses this for its components, and `planCrafting` uses it
// for the component a rejected plan deadlocked in. Grouping the cycle's own
// members is what makes the cut bite: `State::EntryGroup` leaves the group
// out of its own funding sum, so a member can only be named the entry when its
// input is seedable from the stock or from a recipe outside the group. A
// single-column group cannot see that, because the rest of the cycle counts as
// outside and funds the entry.
//
// Returns false when `columns` is empty or no member has a gross input, in
// which case `out` is left empty. The columns are not deduplicated; the caller
// passes a set.
bool buildEntryGroup(const Subgraph &sub, std::span<const uint32_t> columns,
                     solver::State::EntryGroup &out) noexcept;

}  // namespace aw

#endif
