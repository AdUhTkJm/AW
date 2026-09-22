#ifndef STATUS_H
#define STATUS_H

namespace aw {

enum class PlanStatus {
  OK = 0,
  INFEASIBLE,
  NUMERICAL_FAIL,
  ITER_LIMIT,
  INVALID_INPUT,
};

} // namespace aw

#endif
