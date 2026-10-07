#ifndef AW_PLAN_PROFILE_STEP_H
#define AW_PLAN_PROFILE_STEP_H

// Step timers for the passes that run at query time.
//
// AW_PROFILE_PRUNING is defined on the desktop development build only (see
// CMakeLists.txt). When it is not defined, both macros below expand to nothing,
// so a call site costs no code and no runtime branch -- not even the flag test.
// When it is defined, the flag argument stays a per-query runtime switch that
// decides whether a line is printed.
//
// A timed function opens the clock once and closes it after every step:
//
//   void run() {
//     AW_PROFILE_BEGIN();
//     stepOne();
//     AW_PROFILE_END(options.outputPruningProfile, "[time]", "step one");
//     stepTwo();
//     AW_PROFILE_END(options.outputPruningProfile, "[time]", "step two");
//   }
//
// AW_PROFILE_BEGIN() declares a local clock named `awProfileT0`, so the flag
// and the label remain the only arguments at the call site. `prefix` has to be
// a string literal, `label` may be any const char*. Every AW_PROFILE_END()
// prints the elapsed time and restarts the clock, so consecutive steps are
// reported as deltas instead of running totals. The macros nest: a lambda that
// times one invocation may open its own clock, and the inner declaration
// deliberately shadows the outer one.

#ifdef AW_PROFILE_PRUNING

#include <chrono>
#include <cstdio>

#define AW_PROFILE_BEGIN()                                                  \
  std::chrono::steady_clock::time_point awProfileT0 =                       \
      std::chrono::steady_clock::now()

#define AW_PROFILE_END(enabled, prefix, label)                              \
  do {                                                                      \
    if (enabled) {                                                          \
      const std::chrono::steady_clock::time_point awProfileNow =            \
          std::chrono::steady_clock::now();                                 \
      const double awProfileDelta =                                         \
          std::chrono::duration<double>(awProfileNow - awProfileT0).count(); \
      awProfileT0 = awProfileNow;                                           \
      std::fprintf(stderr, prefix " %s: %.6f s\n", label, awProfileDelta); \
    }                                                                       \
  } while (0)

#else

#define AW_PROFILE_BEGIN() ((void) 0)
#define AW_PROFILE_END(enabled, prefix, label) ((void) 0)

#endif

#endif
