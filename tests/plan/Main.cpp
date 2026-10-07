// Entry point for the crafting planner test suite. The tests themselves
// self-register; see Harness.h.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "aw/plan/Options.h"
#include "plan/Harness.h"

namespace {

constexpr const char *k_shufflePrefix = "--shuffle=";

}  // namespace

int main(int argc, char **argv) {
  bool list = false;
  std::uint64_t seed = 0;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--list") {
      list = true;
    } else if (arg == "--shuffle") {
      seed = 1;
    } else if (arg.rfind(k_shufflePrefix, 0) == 0) {
      seed = std::strtoull(arg.c_str() + std::string(k_shufflePrefix).size(), nullptr, 10);
    } else {
      std::cout << "usage: aw_tests [--list] [--shuffle[=SEED]]\n";
      return 2;
    }
  }
  if (list) {
    plan::listTests();
    return 0;
  }

  // The pruning parity tests assert that a pass preserves the optimum, so they
  // need the relaxed modes off. The dedicated tests turn them on themselves.
  aw::options.nonoptimal = false;
  aw::options.tagInlining = aw::TagInlineMode::OFF;
  aw::options.reprune.enabled = false;

  return plan::runAll(seed);
}
