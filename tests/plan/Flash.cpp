// Flash tests
//
// The flash pre-pass and its probe.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

#include "aw/utils/Int128.h"
#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// Delta-debugged from the NAST corpus, the same graph Thunderbolt keeps as
// `ProducibilityRankedOrientationTest`:
//
//   sophisticatedstorage:blasting_upgrade x1, no stock.
//
// Every item is producible from the single input-free recipe 19557 (item 9809),
// yet the plain DFS cycle cut drops every producer of some needed cycle member.
// The ranks are exercised by first failing on the DFS view, so this graph only
// plans when the order-independent cut keeps a producible direction. Recipe
// numbers are the corpus recipe ids; handles without a block are raw leaves.
// Workstation handle 1 is the only one the test makes available.
aw::vector<std::byte> buildRankedOrientationSample() {
  AwrWriter w;
  w.header(12199, 25);
  w.item(1072, 1);
  w.recipe(1, {1}, {{5754, 5}, {9768, 1}, {9798, 3}});            // 1760
  w.item(1077, 1);
  w.recipe(1, {1}, {{1078, 1}});                                  // 1772
  w.item(1078, 1);
  w.recipe(1, {1}, {{11625, 100}, {11704, 20}});                  // 1778
  w.item(2224, 2);
  w.recipe(1, {1}, {{11624, 1000}, {11713, 1000}});               // 4212
  w.recipe(1, {1}, {{4614, 1}});                                  // 4213
  w.item(2229, 1);
  w.recipe(3, {1}, {{11704, 1}});                                 // 4230
  w.item(4614, 1);
  w.recipe(1, {1}, {{12158, 8}});                                 // 10063
  w.item(5754, 2);
  w.recipe(1, {1}, {{9325, 1}});                                  // 12619
  w.recipe(5, {1}, {{11625, 100}, {11704, 20}});                  // 12623
  w.item(5906, 1);
  w.recipe(4, {1}, {{11704, 1}});                                 // 12847
  w.item(5907, 1);
  w.recipe(1, {1}, {{5911, 2}});                                  // 12848
  w.item(5911, 1);
  w.recipe(24, {1}, {{5906, 3}});                                 // 12873
  w.item(8990, 2);
  w.recipe(1, {1}, {{4359, 1}});                                  // 18274
  w.recipe(8, {1}, {{11625, 100}, {11704, 20}});                  // 18275
  w.item(9314, 1);
  w.recipe(1, {1}, {{11704, 1}});                                 // 18770
  w.item(9317, 1);
  w.recipe(1, {1}, {{7378, 1}, {9314, 4}});                       // 18778
  w.item(9768, 1);
  w.recipe(1, {1}, {{4614, 1}, {5754, 3}, {8990, 4}, {10834, 1}});  // 19482
  w.item(9798, 1);
  w.recipe(1, {1}, {{2224, 1}});                                  // 19542
  w.item(9809, 1);
  w.recipe(1, {1}, {});                                          // 19557, input-free
  w.item(10834, 1);
  w.recipe(1, {1}, {{5754, 4}, {12098, 5}});                      // 21464
  w.item(11624, 2);
  w.recipe(250, {1}, {{1077, 1}});                                // 23041
  w.recipe(250, {1}, {{6559, 1}});                                // 23042
  w.item(11625, 1);
  w.recipe(250, {1}, {{11713, 1000}, {12199, 1}});                // 23056
  w.item(11675, 1);
  w.recipe(1000, {1}, {{11713, 1000}});                           // 23209
  w.item(11704, 1);
  w.recipe(1000, {1}, {{11675, 1000}});                           // 23348
  w.item(11713, 2);
  w.recipe(8000, {1}, {{3418, 2}, {3431, 2}, {6458, 1}, {8079, 2}, {9317, 1},
                       {11629, 8000}});                           // 23371
  w.recipe(250, {1}, {{9809, 1}});                                // 23380
  w.item(12098, 2);
  w.recipe(1, {1}, {{4349, 1}});                                  // 25547
  w.recipe(1, {1}, {{5907, 1}});                                  // 25548
  w.item(12158, 1);
  w.recipe(1, {1}, {{2224, 1}});                                  // 27653
  w.item(12199, 1);
  w.recipe(1, {1}, {{2229, 1}});                                  // 27930
  return w.out;
}

// Recomputes the balance `planCrafting` hands the solver and returns whether
// `exec` satisfies it. Mirrors `deriveMissing` in tools/Bench.cpp.
bool greedyBalances(const aw::Subgraph &sub, aw::ItemId target, aw::Amount amount,
                    const aw::vector<aw::Amount> &inventory,
                    const aw::vector<int64_t> &exec) {
  const aw::BaseCraftingGraph &g = sub.graph;
  aw::vector<aw::int128> balance(g.nItem, 0);
  for (uint32_t r = 0; r < g.nRecipe; r++) {
    const aw::int128 times = exec[r];
    if (times == 0)
      continue;
    balance[g.output[r]] += (aw::int128) g.outputAmt[r] * times;
    const auto inputs = g.inputsOf(r);
    const auto weights = g.inputAmountsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      balance[inputs[k]] -= (aw::int128) weights[k] * times;
  }
  for (uint32_t i = 0; i < g.nItem; i++) {
    const aw::ItemId source = sub.itemOrigin[i];
    const aw::Amount available = source < inventory.size() ? inventory[source] : 0;
    const aw::int128 required = i == target ? (aw::int128) amount : -(aw::int128) available;
    if (balance[i] < required)
      return false;
  }
  return true;
}

}  // namespace

AW_TEST(testGreedyDag) {
  std::cout << "[Test] greedy DAG pre-pass\n";

  // A DAG with a zero-input route: greedy must find it and produce a plan that
  // balances.
  aw::registerCraftingGraph(buildZeroInputSample());
  expect(aw::getCraftingError() == nullptr, "greedy DAG sample parses");
  {
    const aw::Handle all[] = {1, 2};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    const aw::ItemId target = sub.translate(0);
    const aw::vector<int64_t> exec = aw::greedyDagPlan(sub, target, 7, {});
    expect(!exec.empty(), "greedy finds a zero-input route");
    expect(exec.size() == sub.graph.nRecipe, "greedy result has one count per recipe");
    if (!exec.empty())
      expect(greedyBalances(sub, target, 7, {}, exec), "greedy DAG plan balances");
    // Flash mode must pass the same plan through untouched and still feasible.
    aw::solver::Options options;
    aw::options.flash = true;
    const aw::PlanResult flash = aw::planCrafting(sub, target, 7, {}, options);
    expect(flash.status == aw::PlanStatus::OK && greedyBalances(sub, target, 7, {}, flash.exec),
           "flash returns the greedy plan and it balances");
    aw::options.flash = false;
  }

  // A pure cycle with no stock: cutting the back-edge removes the only producer
  // of the second item, so greedy declines instead of inventing a plan.
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "greedy cycle sample parses");
  {
    const aw::Handle all[] = {1, 2};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    const aw::ItemId target = sub.translate(0);
    expect(aw::greedyDagPlan(sub, target, 4, {}).empty(), "greedy declines an unseeded cycle");
  }

  // A missing leaf has no producer, so greedy returns nothing and the planner
  // still reports the leaf rather than a false positive.
  aw::registerCraftingGraph(buildPlanLeafSample());
  expect(aw::getCraftingError() == nullptr, "greedy leaf sample parses");
  {
    aw::options.satellite.enabled = false;
    const aw::Handle all[] = {1};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    aw::options.satellite.enabled = true;
    const aw::ItemId target = sub.translate(0);
    expect(aw::greedyDagPlan(sub, target, 4, {}).empty(), "greedy declines a missing leaf");
    expect(aw::planCrafting(sub, target, 4, {}).status == aw::PlanStatus::INFEASIBLE,
           "planner still reports the missing leaf");
  }

  // The DFS back-edge cut depends on arrival order and can drop every producer
  // of a needed cycle member. This graph is the NAST regression Thunderbolt
  // keeps for its ranked orientation: the plain cut finds nothing, and only the
  // order-independent cut keeps a producible direction. Workstation 1 is the
  // only one the query makes available.
  aw::registerCraftingGraph(buildRankedOrientationSample());
  expect(aw::getCraftingError() == nullptr, "ranked orientation sample parses");
  {
    const bool savedSeed = aw::options.seedPruning;
    const bool savedDirect = aw::options.directPruning;
    const bool savedRecipe = aw::options.recipePruning;
    const bool savedSubstitution = aw::options.substitutionPruning;
    const bool savedTag = aw::options.tagPruning;
    const bool savedPack = aw::options.pack.enabled;
    const bool savedSatellite = aw::options.satellite.enabled;
    aw::options.seedPruning = false;
    aw::options.directPruning = false;
    aw::options.recipePruning = false;
    aw::options.substitutionPruning = false;
    aw::options.tagPruning = false;
    aw::options.pack.enabled = false;
    aw::options.satellite.enabled = false;
    const aw::Handle all[] = {1};
    const aw::Subgraph sub = aw::reachableSubgraph(1072, all);
    const aw::ItemId target = sub.translate(1072 - 1);
    const aw::vector<int64_t> exec = aw::greedyDagPlan(sub, target, 1, {});
    expect(!exec.empty(), "ranked cut finds a plan the DFS cut alone loses");
    if (!exec.empty())
      expect(greedyBalances(sub, target, 1, {}, exec), "ranked cut plan balances");
    aw::options.seedPruning = savedSeed;
    aw::options.directPruning = savedDirect;
    aw::options.recipePruning = savedRecipe;
    aw::options.substitutionPruning = savedSubstitution;
    aw::options.tagPruning = savedTag;
    aw::options.pack.enabled = savedPack;
    aw::options.satellite.enabled = savedSatellite;
  }
}

AW_TEST(testFlashProbe) {
  std::cout << "[Test] flash probe\n";

  // The probe is `reachableSubgraph`'s flash shortcut: the greedy pre-pass runs
  // on a subgraph built without satellite elimination or the variant passes, and
  // a hit comes back as `flashExec` on the subgraph it was found on.
  aw::registerCraftingGraph(buildZeroInputSample());
  expect(aw::getCraftingError() == nullptr, "flash probe sample parses");
  {
    const aw::Handle all[] = {1, 2};
    const aw::Amount amount = 7;

    const aw::Subgraph off = aw::reachableSubgraph(1, all, {}, amount);
    expect(off.flashExec.empty(), "a non-flash query carries no probe plan");

    aw::options.flash = true;
    const aw::Subgraph on = aw::reachableSubgraph(1, all, {}, amount);
    const aw::ItemId target = on.translate(0);
    expect(!on.flashExec.empty(), "the probe reports the plan it found");
    expect(target != UINT32_MAX && on.flashExec.size() == on.graph.nRecipe,
           "the probe plan has one count per recipe of the subgraph it came with");
    if (!on.flashExec.empty())
      expect(greedyBalances(on, target, amount, {}, on.flashExec),
             "the probe plan balances against the subgraph it came with");

    // `planCrafting` hands the probe's own vector back, tagged as greedy, and
    // gets the same answer as when it runs the pre-pass itself.
    aw::solver::Options options;
    const aw::PlanResult plan = aw::planCrafting(on, target, amount, {}, options);
    expect(plan.status == aw::PlanStatus::OK && plan.fromGreedy,
           "planCrafting adopts the probe's plan");
    expect(plan.exec == on.flashExec, "planCrafting returns the probe's own vector");
    expect(plan.greedyMs == 0.0, "the probe's plan costs planCrafting no greedy time");

    // The mod sets `flash` on the solver options rather than the planner ones,
    // so the probe has to read both or it never runs in game.
    aw::options.flash = false;
    aw::solverOptions.flash = true;
    const aw::Subgraph viaSolver = aw::reachableSubgraph(1, all, {}, amount);
    expect(!viaSolver.flashExec.empty(), "the probe reads the solver's flash switch too");
    aw::solverOptions.flash = false;

    aw::options.flash = true;
    const bool savedProbe = aw::options.flashProbe;
    aw::options.flashProbe = false;
    const aw::Subgraph unprobed = aw::reachableSubgraph(1, all, {}, amount);
    expect(unprobed.flashExec.empty(), "flashProbe off disables the probe");
    aw::options.flashProbe = savedProbe;

    // Turning it off has to leave the answer alone: the plan comes from the
    // same pre-pass, just later and on the pruned subgraph.
    const aw::PlanResult slow = aw::planCrafting(
        unprobed, unprobed.translate(0), amount, {}, options);
    expect(slow.status == aw::PlanStatus::OK && slow.fromGreedy && slow.exec == plan.exec,
           "the probe returns the plan the unprobed path finds anyway");
    aw::options.flash = false;
  }

  // A miss has to leave the full query untouched: same subgraph, no plan. This
  // is the invariant that makes the probe free when it fails.
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "flash probe cycle sample parses");
  {
    const aw::Handle all[] = {1, 2};
    const aw::Subgraph plain = aw::reachableSubgraph(1, all, {}, 4);
    aw::options.flash = true;
    const aw::Subgraph probed = aw::reachableSubgraph(1, all, {}, 4);
    aw::options.flash = false;
    expect(probed.flashExec.empty(), "a probe miss reports no plan");
    expect(probed.graph.nItem == plain.graph.nItem &&
               probed.graph.nRecipe == plain.graph.nRecipe &&
               probed.itemOrigin == plain.itemOrigin &&
               probed.recipeOrigin == plain.recipeOrigin,
           "a probe miss returns the full query's subgraph");
  }
}

}  // namespace plan
