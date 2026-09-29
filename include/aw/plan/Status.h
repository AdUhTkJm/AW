#ifndef STATUS_H
#define STATUS_H

namespace aw {

enum class PlanStatus {
  OK = 0,
  INFEASIBLE,
  NUMERICAL_FAIL,
  ITER_LIMIT,
  INVALID_INPUT,
  // The solver found a balance-feasible plan, but the post-solve fireability
  // check could not turn it into a firing sequence: some cycle in the plan has
  // no way to start from the stock and the upstream output. The plan is not
  // returned as a usable answer. This is unproven: the plan is unrealizable,
  // but a different plan may still exist, so a caller may retry with a no-good
  // or fall back to the acyclic plan. See docs/algorithm.typ, "启动可达性".
  CYCLE_UNFULFILLED,
};

} // namespace aw

#endif
