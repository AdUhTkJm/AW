// Variant tests
//
// Rules that reason about a recipe's variant class, tag exclusivity and
// variant folding: which member of a variant group may be spent.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <utility>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "AwrWriter.h"

namespace plan {

namespace {

aw::vector<std::byte> buildVariantClassSample(bool namedConsumer) {
  AwrWriter w;
  w.header(7, 6);
  w.item(1, 1);
  if (namedConsumer)
    w.recipe(1, {1}, {{2, 1}, {8, 1}});
  else
    w.recipe(1, {1}, {{8, 1}});
  w.item(2, 2);
  w.recipe(1, {1}, {{4, 1}});
  w.recipe(1, {1}, {{6, 1}, {9, 1}});
  w.item(3, 2);
  w.recipe(1, {1}, {{5, 1}});
  w.recipe(1, {1}, {{7, 1}, {10, 1}});
  w.item(8, 2);
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{3, 1}});
  w.item(9, 1);
  w.recipe(1, {}, {{3, 1}});
  w.item(10, 1);
  w.recipe(1, {}, {{2, 1}});
  return w.out;
}

// A per-item tag-exclusive shape. Handles 1..9 are real, 10 is the tag; 2, 4
// and 6 are leaves and 8 (side) is only ever a byproduct. Handle 1 is the
// target.
//
//   target <- r0 (x1, ws [1], TE x1, other x1, side x1)
//   B      <- r1 (x1, ws [1], resourceB x1)            stays, a net source
//          <- r2 (x1, ws [1], A x1, dye x1)           drop, B is exclusive
//          <- r3 (x1, ws [1], side x1; A x1, dye x1)  stays, side leaks out
//   A      <- r4 (x1, ws [1], resourceA x1)
//   other  <- r5 (x1, ws [1], A x1)
//   C      <- r6 (x1, ws [1], TE x1)                   drop, TE itself pays
//   TE = {A, B, C} <- r7, r8, r9 (tag edges)
//
// A has a real consumer (r5), so it leaves the variant class and r2's input A
// stops paying under the class test; it is the per-item credit of any M(TE)
// member that drops r2 here. r1 shows a net source must stay, r3 shows a
// byproduct with a real consumer must stay, and r6 shows a tag input pays.

aw::vector<std::byte> buildTagExclusiveSample() {
  AwrWriter w;
  w.header(9, 6);

  w.item(1, 1);
  w.recipe(1, {1}, {{7, 1}, {8, 1}, {10, 1}});

  w.item(3, 3);
  w.recipe(1, {1}, {{2, 1}});
  w.recipe(1, {1}, {{5, 1}, {6, 1}});
  w.recipe(1, {1}, {{8, 1}}, {{5, 1}, {6, 1}});

  w.item(5, 1);
  w.recipe(1, {1}, {{4, 1}});

  w.item(7, 1);
  w.recipe(1, {1}, {{5, 1}});

  w.item(9, 1);
  w.recipe(1, {1}, {{10, 1}});

  w.item(10, 3);
  w.recipe(1, {}, {{5, 1}});
  w.recipe(1, {}, {{3, 1}});
  w.recipe(1, {}, {{9, 1}});
  return w.out;
}

// A target that is itself a decorative twin, which the folding pass must never
// put into D. Handles 1..5 are real and 6, 7 are tags; handle 4 is the target.
//
//   Y (1) <- (leaf)                 D (5) <- (leaf)
//   X (2) <- Y x4                   the folded twin
//   B (3) <- Y x4, D x1             the base's recipe
//   T (4) <- X x4, D x1             the target, touches the folded X
//          <- B x4                  the conversion that puts T in X's orbit
//   D (5) <- #t1 x1, #t2 x1         real tag consumers, so neither tag is dead
//   #t1 = {Y, X}, #t2 = {Y}         X's tag set is a strict subset of Y's
//
// Folding X into Y leaves T's recipe touching D, and its projection is exactly
// B's recipe column. The growth path therefore used to propose folding T into
// B, and the whole fold verified -- but pi maps every D item to 0, so the
// target could no longer be produced and the query came out infeasible.

aw::vector<std::byte> buildVariantFoldTargetSample() {
  AwrWriter w;
  w.header(5, 6);
  w.item(2, 1);
  w.recipe(4, {1}, {{1, 4}});
  w.item(3, 1);
  w.recipe(4, {1}, {{1, 4}, {5, 1}});
  w.item(4, 2);
  w.recipe(4, {1}, {{2, 4}, {5, 1}});
  w.recipe(4, {1}, {{3, 4}});
  w.item(5, 2);
  w.recipe(1, {1}, {{6, 1}});
  w.recipe(1, {1}, {{7, 1}});
  w.item(6, 2);
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{1, 1}});
  w.item(7, 1);
  w.recipe(1, {}, {{1, 1}});
  return w.out;
}

}  // namespace

AW_TEST(testVariantClassPruning) {
  std::cout << "[Test] interchangeable-variant elimination\n";
  const aw::Handle all[] = {1, 2, 3};

  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.pack.enabled = false;
  aw::options.satellite.enabled = false;

  // Stock the four leaves so seed pruning keeps the chain alive.
  auto stockedLeaves = []() {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[3] = 1000;  // handle 4, resourceA
    inventory[4] = 1000;  // handle 5, resourceB
    inventory[5] = 1000;  // handle 6, dyeA
    inventory[6] = 1000;  // handle 7, dyeB
    return inventory;
  };

  // The clean orbit: the dyes (r2, r4) and the P_A/P_B member edges (r7, r8)
  // go, the direct producers and the TE member edges stay.
  aw::registerCraftingGraph(buildVariantClassSample(false));
  expect(aw::getCraftingError() == nullptr, "variant sample parses");
  {
    const aw::vector<aw::Amount> inventory = stockedLeaves();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(!subgraphHasRecipe(sub, 2), "the A dye conversion is dropped");
    expect(!subgraphHasRecipe(sub, 4), "the B dye conversion is dropped");
    expect(!subgraphHasRecipe(sub, 7) && !subgraphHasRecipe(sub, 8),
           "the dead conversion tags' member edges go with them");
    expect(subgraphHasRecipe(sub, 0) && subgraphHasRecipe(sub, 5) &&
               subgraphHasRecipe(sub, 6),
           "the exit tag and its member edges survive");
    expect(subgraphHasRecipe(sub, 1) && subgraphHasRecipe(sub, 3),
           "the direct producers survive");
  }

  // A real recipe that wants A by name removes A from the orbit, so the
  // conversion into A must stay.
  aw::registerCraftingGraph(buildVariantClassSample(true));
  expect(aw::getCraftingError() == nullptr, "variant sample with a consumer parses");
  {
    const aw::vector<aw::Amount> inventory = stockedLeaves();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 2), "a named member keeps its conversion");
    expect(subgraphHasRecipe(sub, 0),
           "the exit that names the member survives");
  }

  // Disabling the pass keeps everything.
  aw::options.variantClass.enabled = false;
  aw::registerCraftingGraph(buildVariantClassSample(false));
  {
    const aw::vector<aw::Amount> inventory = stockedLeaves();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 2) && subgraphHasRecipe(sub, 7),
           "the pass can be switched off");
  }
  aw::options.variantClass.enabled = true;

  // The optimum must not move for any amount: the direct route is one step,
  // and so is a conversion, so the class is a pure wash the pass may remove.
  {
    const aw::vector<std::byte> bytes = buildVariantClassSample(false);
    auto total = [](const aw::PlanResult &r) {
      int64_t sum = 0;
      for (int64_t x : r.exec)
        sum += x;
      return sum;
    };
    auto plan = [&](bool prune, aw::Amount amount) {
      aw::options.variantClass.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      for (aw::ItemId leaf : {3u, 4u, 5u, 6u})
        inventory[leaf] = 1000;
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 3, 8}) {
      const auto full = plan(false, amount);
      const auto pruned = plan(true, amount);
      expect(full.first == pruned.first,
             "variant: status agrees with and without the pass");
      expect(full.second == pruned.second,
             "variant: optimum agrees with and without the pass");
    }
    aw::options.variantClass.enabled = true;
  }

  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.substitutionPruning = true;
  aw::options.pack.enabled = true;
  aw::options.satellite.enabled = true;
}

AW_TEST(testTagExclusivePruning) {
  std::cout << "[Test] tag-exclusive producer elimination\n";
  const aw::Handle all[] = {1, 2, 3};

  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.pack.enabled = false;
  aw::options.satellite.enabled = false;
  aw::options.variantClass.enabled = false;

  // Stock the leaves so seed pruning keeps the chain alive. Handles 2, 4 and 6
  // are items 1, 3 and 5.
  auto stockedLeaves = []() {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[1] = 1000;  // handle 2, resourceB
    inventory[3] = 1000;  // handle 4, resourceA
    inventory[5] = 1000;  // handle 6, dye
    return inventory;
  };

  // Canonical recipe order for this sample: 0 target, 1 B <- resourceB,
  // 2 B <- A + dye, 3 B + side <- A + dye, 4 A <- resourceA, 5 other <- A,
  // 6 C <- TE, 7 TE <- B, 8 TE <- A, 9 TE <- C.
  aw::registerCraftingGraph(buildTagExclusiveSample());
  expect(aw::getCraftingError() == nullptr, "tag-exclusive sample parses");
  {
    const aw::vector<aw::Amount> inventory = stockedLeaves();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(!subgraphHasRecipe(sub, 2), "the neutral conversion into B is dropped");
    expect(!subgraphHasRecipe(sub, 6), "the tag-fed C is dropped");
    expect(subgraphHasRecipe(sub, 1), "the net source producer survives");
    expect(subgraphHasRecipe(sub, 3), "the byproduct producer survives");
    expect(subgraphHasRecipe(sub, 7) && subgraphHasRecipe(sub, 8),
           "the member edges survive");
  }

  // A query that names B keeps its producers.
  {
    const aw::vector<aw::Amount> inventory = stockedLeaves();
    const aw::Subgraph sub = aw::reachableSubgraph(3, all, inventory);
    expect(subgraphHasRecipe(sub, 2), "a query for B keeps its producer");
  }

  // Switching the pass off keeps the conversions.
  aw::options.tagExclusive.enabled = false;
  aw::registerCraftingGraph(buildTagExclusiveSample());
  {
    const aw::vector<aw::Amount> inventory = stockedLeaves();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 2) && subgraphHasRecipe(sub, 6),
           "the pass can be switched off");
  }
  aw::options.tagExclusive.enabled = true;

  // Parity: the dropped conversions are a wash, so the optimum must not move.
  {
    const aw::vector<std::byte> bytes = buildTagExclusiveSample();
    auto total = [](const aw::PlanResult &r) {
      int64_t sum = 0;
      for (int64_t x : r.exec)
        sum += x;
      return sum;
    };
    auto plan = [&](bool prune, aw::Amount amount) {
      aw::options.tagExclusive.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      for (aw::ItemId leaf : {1u, 3u, 5u})
        inventory[leaf] = 1000;
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 2, 5}) {
      const auto full = plan(false, amount);
      const auto pruned = plan(true, amount);
      expect(full.first == pruned.first,
             "tag-exclusive: status agrees with and without the pass");
      expect(full.second == pruned.second,
             "tag-exclusive: optimum agrees with and without the pass");
    }
    aw::options.tagExclusive.enabled = true;
  }

  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.substitutionPruning = true;
  aw::options.pack.enabled = true;
  aw::options.satellite.enabled = true;
  aw::options.variantClass.enabled = true;
}

AW_TEST(testVariantFoldTarget) {
  std::cout << "[Test] variant folding keeps the target out of D\n";
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  aw::options.tagPruning = false;
  aw::options.satellite.enabled = false;
  aw::options.variantClass.enabled = false;
  aw::options.tagExclusive.enabled = false;

  // Handle 4 is the target, so its node is 3. Y (handle 1) and D (handle 5)
  // are the leaves, stocked so the walk keeps the chain.
  auto run = [&](bool fold) {
    aw::options.variantFold.enabled = fold;
    aw::registerCraftingGraph(buildVariantFoldTargetSample());
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    inventory[0] = 1000;  // Y
    inventory[4] = 1000;  // D
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::ItemId target = sub.translate(3);
    bool producer = false;
    for (uint32_t r = 0; r < sub.graph.nRecipe; r++)
      for (aw::ItemId o : sub.graph.outputsOf(r))
        producer |= o == target;
    const aw::PlanResult plan = aw::planCrafting(sub, target, 1, inventory);
    int64_t total = 0;
    for (int64_t x : plan.exec)
      total += x;
    return std::make_tuple(plan.status, total, producer);
  };

  aw::registerCraftingGraph(buildVariantFoldTargetSample());
  expect(aw::getCraftingError() == nullptr, "variant-fold sample parses");
  const auto off = run(false);
  const auto on = run(true);
  expect(std::get<0>(off) == aw::PlanStatus::OK,
         "the target plans with folding disabled");
  expect(std::get<2>(on), "folding keeps a producer for the target");
  expect(std::get<0>(on) == std::get<0>(off),
         "folding keeps the target feasible");
  expect(std::get<1>(on) == std::get<1>(off),
         "folding keeps the target's optimum");

  aw::options.variantFold.enabled = true;
  aw::options.tagPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.variantClass.enabled = true;
  aw::options.tagExclusive.enabled = true;
}

}  // namespace plan
