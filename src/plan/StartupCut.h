#ifndef AW_PLAN_STARTUP_CUT_H
#define AW_PLAN_STARTUP_CUT_H

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Solver.h"

namespace aw {

// Eager startup entry cuts for the 1- and 2-cycles of `sub`.
//
// Appends to `out`, up to `maxGroups` of them, one group per connected
// component of the net-gain mutual-consumption relation and per net-gain
// self-loop. `maxMembers` skips a component too large to cut usefully, which is
// how a whole conversion soup is kept out of the model. `solver::Options::
// EntryGroup` says what a group means and why every fireable plan obeys it.
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
void buildStartupCuts(const Subgraph &sub, aw::vector<solver::Options::EntryGroup> &out,
                      uint32_t maxGroups, uint32_t maxMembers) noexcept;

}  // namespace aw

#endif
