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
  // but a different plan may still exist, so `planCrafting` spends up to
  // `solver::Options::maxCycleRetries` extra solves looking for one, forbidding
  // each rejected plan in turn. This status is what comes back when the retries
  // are exhausted (or disabled). See docs/algorithm.typ, "启动可达性" and
  // "no-good 重试".
  CYCLE_UNFULFILLED,
};

} // namespace aw

#endif
