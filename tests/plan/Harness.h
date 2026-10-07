#ifndef AW_TEST_PLAN_HARNESS_H
#define AW_TEST_PLAN_HARNESS_H

// Harness for the crafting planner tests: a self-registering list of tests plus
// a tiny assertion helper. There is no framework, which keeps the build
// dependency-free and lets every test share the process-wide aw::options.
//
//   AW_TEST(testRecipePruning) {
//     ...
//     expect(condition, "what the condition means");
//   }
//
// A test mutates aw::options and aw::solverOptions, so it has to leave them as
// it found them. runAll snapshots both around every test, which keeps a leaked
// switch from changing a later test and makes the run order irrelevant;
// `aw_tests --shuffle` exercises exactly that.

#include <cstdint>

namespace plan {

using TestFn = void (*)();

// Registers `fn` under `name`; constructed by the static initializer AW_TEST
// emits, so the translation unit links itself into the suite.
struct Registration {
  Registration(const char *testName, TestFn fn);
};

// Records a failed check and prints it. The reason strings are the test's
// contract, so they read as assertions rather than as error messages.
void expect(bool condition, const char *what);

// Failed checks so far.
int failures();

// Prints every registered test name, one per line.
void listTests();

// Runs every registered test and returns the process exit code. A non-zero
// `seed` runs the tests in a random order derived from it.
int runAll(std::uint64_t seed = 0);

}  // namespace plan

#define AW_TEST(name)                                                          \
  void name();                                                                 \
  namespace {                                                                  \
  const ::plan::Registration aw_registration_##name{#name, &name};             \
  }                                                                            \
  void name()

#endif  // AW_TEST_PLAN_HARNESS_H
