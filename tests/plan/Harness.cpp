// Test registry and assertion helper; see Harness.h for the contract.

#include "plan/Harness.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <random>
#include <vector>

#include "aw/plan/Options.h"

namespace plan {
namespace {

struct Entry {
  const char *name;
  TestFn fn;
};

std::vector<Entry> &registry() {
  static std::vector<Entry> entries;
  return entries;
}

int g_failures = 0;

}  // namespace

Registration::Registration(const char *testName, TestFn fn) {
  registry().push_back({testName, fn});
}

void expect(bool condition, const char *what) {
  if (!condition) {
    std::cout << "  [FAIL] " << what << '\n';
    ++g_failures;
  }
}

int failures() { return g_failures; }

void listTests() {
  for (const Entry &entry : registry())
    std::cout << entry.name << '\n';
}

int runAll(std::uint64_t seed) {
  std::vector<Entry> &entries = registry();
  std::vector<std::size_t> order(entries.size());
  for (std::size_t i = 0; i < order.size(); i++)
    order[i] = i;
  if (seed != 0)
    std::shuffle(order.begin(), order.end(), std::mt19937_64(seed));

  for (std::size_t i : order) {
    // Every test starts from the switches main() installed, whatever the tests
    // before it did.
    const aw::Options savedOptions = aw::options;
    const aw::solver::Options savedSolver = aw::solverOptions;
    entries[i].fn();
    aw::options = savedOptions;
    aw::solverOptions = savedSolver;
  }

  if (g_failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cout << g_failures << " check(s) failed\n";
  return 1;
}

}  // namespace plan
