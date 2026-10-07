// Option tests
//
// Options JSON patches and the nonoptimal (relaxed) modes.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/OptionsJson.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// The same shape as buildIntegerScalingSample, but Z can be crafted from a leaf
// W. That keeps S undominated in both modes, so R is the only recipe the
// ceiling (or the floored inline count) can affect:
//
//   handle 1 X <- R (x1, ws [1], Y x3)
//              <- S (x1, ws [1], Z x1)
//   handle 2 Y <- r (x2, ws [1], Z x1)
//   handle 3 Z <- z (x1, ws [1], W x1)
//   handle 4 W is a leaf.
//
// ceil(3 / 2) = 2, so `2 Y + R` leaves a spare Y and is not bounded by S. The
// nonoptimal relaxation floors to 1, which wrongly bounds R by S. Recipe ids:
// R=0, S=1, r=2, z=3.
aw::vector<std::byte> buildRelaxedCompositeSample() {
  AwrWriter w;
  w.header(4, 3);
  w.item(1, 2);
  w.recipe(1, {1}, {{2, 3}});
  w.recipe(1, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(2, {1}, {{3, 1}});
  w.item(3, 1);
  w.recipe(1, {1}, {{4, 1}});
  return w.out;
}

// Output-amount normalization for the relaxed direct pass. The free X recipe is
// cheaper per unit than the six-output recipe that eats an essence, so the
// normalized comparison drops the latter although one execution of it yields
// six X and one execution of the former only one:
//
//   handle 1 X <- r0 (x1, ws [4], nothing)
//               <- r1 (x6, ws [4], essence x3)
//   handle 2 essence <- r2 (x1, ws [4], Z x1)
//   handle 3 Z is a leaf, handle 4 is WS.
//
// essence has a producer on purpose, so the composite pass leaves r1 alone and
// the direct flag below is the only thing under test.
aw::vector<std::byte> buildNormalizedDominanceSample() {
  AwrWriter w;
  w.header(4, 2);
  w.item(1, 2);
  w.recipe(1, {4}, {});
  w.recipe(6, {4}, {{2, 3}});
  w.item(2, 1);
  w.recipe(1, {4}, {{3, 1}});
  return w.out;
}

// Two recipes with the same per-unit column but different batch sizes. The
// normalized comparison must leave both alive: for a single X the finer recipe
// is far cheaper, so collapsing the two into one SCC (what a non-strict
// comparison does) would overproduce X and blow the plan up.
//
//   handle 1 X <- r0 (x1, ws [4], A x1)
//               <- r1 (x6, ws [4], A x6)   ; same A per X
//   handle 2 A <- r2 (x1, ws [4], Z x1)
//   handle 3 Z is a leaf, handle 4 is WS.
aw::vector<std::byte> buildEqualRatioSample() {
  AwrWriter w;
  w.header(4, 2);
  w.item(1, 2);
  w.recipe(1, {4}, {{2, 1}});
  w.recipe(6, {4}, {{2, 6}});
  w.item(2, 1);
  w.recipe(1, {4}, {{3, 1}});
  return w.out;
}

}  // namespace

AW_TEST(testOptionsJson) {
  std::cout << "[Test] options json\n";
  const aw::Options savedPlanner = aw::options;
  const aw::solver::Options savedSolver = aw::solverOptions;
  std::string error;

  expect(aw::applyPlannerOptionsJson(
             R"({"tagPruning": false, "maxTagMembers": 12, "tagInlining": "queryTime", "pack": {"enabled": false}})",
             error),
         "a partial planner patch applies");
  expect(!aw::options.tagPruning, "a planner switch is updated");
  expect(aw::options.maxTagMembers == 12, "a planner budget is updated");
  expect(aw::options.tagInlining == aw::TagInlineMode::QUERY_TIME, "the tag-inlining enum is updated");
  expect(!aw::options.pack.enabled, "a nested planner object is updated");
  expect(aw::options.recipePruning, "an absent key keeps its current value");

  expect(aw::applyPlannerOptionsJson(R"({"flashProbe": false})", error),
         "the planner patch accepts flashProbe");
  expect(!aw::options.flashProbe, "the flash probe switch is updated");
  aw::options.flashProbe = true;

  // Flash is a planner option: the process-wide switch lives here, and the
  // solver takes its per-solve copy from it (solver::State::flash).
  expect(aw::applyPlannerOptionsJson(R"({"flash": true})", error),
         "the planner patch accepts flash");
  expect(aw::options.flash, "the flash switch is updated");
  aw::options.flash = false;

  error.clear();
  expect(!aw::applyPlannerOptionsJson(R"({"nope": 1})", error) && !error.empty(),
         "an unknown planner key is rejected");
  expect(aw::options.maxTagMembers == 12, "a rejected patch changes nothing");

  error.clear();
  expect(!aw::applyPlannerOptionsJson(R"({"maxTagMembers": -1})", error) && !error.empty(),
         "a negative budget is rejected");
  expect(!aw::applyPlannerOptionsJson(R"({"pack": 3})", error), "a non-object nested field is rejected");
  expect(!aw::applyPlannerOptionsJson(R"({"tagInlining": "sometimes"})", error),
         "an unknown tag-inlining mode is rejected");
  expect(!aw::applyPlannerOptionsJson("not json", error), "a malformed document is rejected");

  error.clear();
  const std::string dumped = aw::plannerOptionsJson();
  expect(aw::applyPlannerOptionsJson(dumped, error), "the dumped planner options apply verbatim");
  expect(error.empty(), "round-tripping the planner options reports no error");
  expect(dumped.find("\"flash\"") != std::string::npos,
         "the dump carries the flash switch");

  error.clear();
  // "flash" is retired on the solver side and must still be accepted, so an
  // older client that sends it is not rejected; the value is ignored.
  expect(aw::applySolverOptionsJson(R"({"flash": true, "numWorkers": 1, "maxTimeSeconds": 0.5})", error),
         "a partial solver patch applies");
  expect(aw::solverOptions.numWorkers == 1, "solver scalars are updated");
  expect(aw::solverOptions.maxTimeSeconds == 0.5, "a solver double is updated");
  expect(aw::solverOptions.relativeGap == savedSolver.relativeGap, "an absent solver key keeps its value");
  expect(!aw::applySolverOptionsJson(R"({"nope": true})", error),
         "an unknown solver key is rejected");
  expect(!aw::applySolverOptionsJson(R"({"maxTimeSeconds": "slow"})", error),
         "a wrong solver type is rejected");

  error.clear();
  const std::string dumpedSolver = aw::solverOptionsJson();
  expect(aw::applySolverOptionsJson(dumpedSolver, error), "the dumped solver options apply verbatim");

  aw::options = savedPlanner;
  aw::solverOptions = savedSolver;
}

// Nonoptimal mode is on purpose: it may plan worse, but it must never break
// feasibility. These are the samples that witness the guards it drops, so the
// relaxed run must actually mark the edge/recipe -- and, for the batch sample,
// plan worse.
AW_TEST(testNonoptimal) {
  std::cout << "[Test] nonoptimal mode\n";
  const aw::Handle all[] = {1, 2, 3, 4};

  // A batched member: without the guard `T <- m` is marked dominated and the
  // batch surplus is no longer a free sink, so the real optimum rises from 4
  // to 6.
  aw::options.nonoptimal = true;
  aw::registerCraftingGraph(buildBatchRecycleSample());
  expect(aw::getCraftingError() == nullptr, "batch recycle sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    bool batchedEdgeDominated = false;
    for (aw::RecipeId r : graph.producersOf(4)) {
      const auto inputs = graph.inputsOf(r);
      if (inputs.size() == 1 && inputs[0] == 2)
        batchedEdgeDominated = graph.tagEdgeDominated[r] == 1;
    }
    expect(batchedEdgeDominated, "the relaxed pass drops the batched tag edge");

    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    aw::options.seedPruning = true;
    const aw::ItemId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);
    // The relaxed pass still drops the tag edge, but the sample is an unseeded
    // cycle, so the net-balanced plan is rejected instead of reported feasible.
    expect(r.status == aw::PlanStatus::CYCLE_UNFULFILLED,
           "the relaxed batch cycle is rejected");
  }

  // The composite ceiling: R needs 3 Y but the only producer emits 2, so
  // floor(3 / 2) = 1 wrongly dominates R. The exact run leaves R alone.
  aw::registerCraftingGraph(buildRelaxedCompositeSample());
  expect(aw::getCraftingError() == nullptr, "relaxed composite sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 1,
         "the nonoptimal pass floors the inline count");

  aw::options.nonoptimal = false;
  aw::registerCraftingGraph(buildRelaxedCompositeSample());
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "the ceiling keeps R undominated in exact mode");

  // Output normalization in the direct pass: the free single-output recipe is
  // cheaper per unit than the six-output recipe that eats an essence, so the
  // relaxed pass drops the latter although one execution of it is not
  // replaceable by one execution of the former.
  aw::options.nonoptimal = true;
  aw::registerCraftingGraph(buildNormalizedDominanceSample());
  expect(aw::getCraftingError() == nullptr, "normalized dominance sample parses");
  expect(aw::getCraftingGraph().recipeDirectDominated[1] == 1,
         "the relaxed pass normalizes the direct comparison");
  aw::options.nonoptimal = false;
  aw::registerCraftingGraph(buildNormalizedDominanceSample());
  expect(aw::getCraftingGraph().recipeDirectDominated[1] == 0,
         "raw column dominance keeps the six-output recipe");

  // End to end the relaxed pass trades steps for materials: six X cost one r1
  // plus three essence crafts (4 steps), but with r1 dropped they cost six free
  // r0 crafts (6 steps) and no essence.
  {
    const aw::Handle all[] = {1, 2, 3, 4};
    auto plan = [&](bool relaxed) {
      aw::options.nonoptimal = relaxed;
      aw::registerCraftingGraph(buildNormalizedDominanceSample());
      const aw::CraftingGraph &g = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(g.nItem, 0);
      inventory[2] = 1000;  // Z, the leaf behind essence
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), 6, inventory);
      int64_t total = 0;
      for (int64_t x : r.exec)
        total += x;
      return total;
    };
    expect(plan(false) == 4, "the raw plan batches the six-output recipe");
    expect(plan(true) == 6, "the normalized plan falls back to single crafts");
    aw::options.nonoptimal = false;
  }

  // Equal per-unit columns must not collapse: a coarse batch cannot replace a
  // fine one without overproducing, which is how the relaxed pass once turned
  // one nether brick wall into six.
  {
    const aw::Handle all[] = {1, 2, 3, 4};
    aw::options.nonoptimal = true;
    aw::registerCraftingGraph(buildEqualRatioSample());
    const aw::CraftingGraph &g = aw::getCraftingGraph();
    expect(g.recipeDirectDominated[0] == 0 && g.recipeDirectDominated[1] == 0,
           "equal per-unit columns stay incomparable");
    aw::vector<aw::Amount> inventory(g.nItem, 0);
    inventory[2] = 1000;  // Z
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), 1, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    expect(total == 2, "the finer recipe serves a single X");
    aw::options.nonoptimal = false;
  }
}

}  // namespace plan
