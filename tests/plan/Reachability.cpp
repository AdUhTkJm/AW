// Reachability tests
//
// Reachability from a query and the seed-unreachable pruning rule.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// A dump for the reachability walk. Handles 1..4 are real (item nodes 0..3) and
// handle 5 is a pseudo-resource (node 4) whose members are handles 1 and 2.
//
//   item 1    <- rA (x1, workstations [3], inputs pseudo-5 x1)
//             <- rB (x1, workstations [4], inputs item-3 x1)
//   pseudo-5  <- rC (x1, no workstations, inputs item-1 x1)   (synthetic)
//             <- rD (x1, no workstations, inputs item-2 x1)   (synthetic)
//
// Recipe ids in file order: rA=0, rB=1, rC=2, rD=3.
aw::vector<std::byte> buildReachSample() {
  AwrWriter w;
  w.header(4, 2);
  w.item(1, 2);
  w.recipe(1, {3}, {{5, 1}});
  w.recipe(1, {4}, {{3, 1}});
  w.item(5, 2);
  w.recipe(1, {}, {{1, 1}});
  w.recipe(1, {}, {{2, 1}});
  return w.out;
}

// A dump for seed-reachability pruning. Handles 1..3 are real.
//
//   item 1 <- rT  (x1, workstations [1], inputs item-2 x1)
//          <- rT2 (x1, workstations [1], inputs item-3 x1)
//   item 2 <- rA  (x1, workstations [2], inputs item-3 x1)
//
// item 3 has no producer. Recipe ids in file order: rT=0, rT2=1, rA=2. When
// only workstation 1 is allowed, item 2 keeps a producer in the source graph
// but has none in the subgraph. With nothing in stock neither item 2 nor item
// 3 can be reached, so both consumers are dropped; stocking one leaf keeps
// exactly its own branch.
aw::vector<std::byte> buildSeedSample() {
  AwrWriter w;
  w.header(3, 2);
  w.item(1, 2);
  w.recipe(1, {1}, {{2, 1}});
  w.recipe(1, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {2}, {{3, 1}});
  return w.out;
}

// A byproduct carries the only route to a needed input:
//
//   item 1 (A) <- r0 (x1, workstations [1], input item 2 (B) x1)
//   item 3 (C) <- r1 (x1, workstations [1], no inputs, byproduct item 2 (B) x1)
//
// B has no recipe of its own; r1 makes it as a byproduct. Seed pruning has to
// credit every output row of a fireable recipe, or nothing produces B and the
// target's only recipe is dropped. Recipe ids in file order: r0=0, r1=1.
aw::vector<std::byte> buildByproductSeedSample() {
  AwrWriter w;
  w.header(3, 2);
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 1}});
  w.item(3, 1);
  w.recipe(1, {1}, {{2, 1}}, {});
  return w.out;
}

}  // namespace

AW_TEST(testReachability) {
  std::cout << "[Test] reachable subgraph\n";
  const aw::vector<std::byte> bytes = buildReachSample();
  aw::registerCraftingGraph(bytes);
  expect(aw::getCraftingError() == nullptr, "reach sample parses");
  const aw::CraftingGraph& graph = aw::getCraftingGraph();
  expect(graph.nReal == 4 && graph.nItem == 5 && graph.nRecipe == 4, "reach sample shape");
  expect(graph.isRealItem(3) && !graph.isRealItem(4), "handle 5 is a pseudo-resource");

  const aw::Handle allowed[] = {3};  // -> item node 2
  // Item node 1 is stocked, so the rD branch stays: satellite elimination keeps
  // a component that holds stock (it is a way to spend it) even though nothing
  // produces item node 1. With no stock that branch would be dropped, which is
  // what testSatellitePruning covers.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[1] = 10;
  aw::Subgraph sub = aw::reachableSubgraph(1, allowed, inventory);

  // Kept items are the output (0), the pseudo (4) and the pseudo's reachable
  // member (1)
  expect(sub.graph.nItem == 3 && sub.graph.nReal == 2, "subgraph item counts");
  const aw::ItemId items[] = {0, 1, 4};
  for (int i = 0; i < 3; i++)
    expect(sub.itemOrigin[i] == items[i], "itemOrigin is ascending");
  expect(!sub.graph.isRealItem(3), "the renumbered pseudo is not a real item");

  // rB is dropped because its only workstation is not allowed; rA and both
  // synthetic recipes survive.
  expect(sub.graph.nRecipe == 3, "rB is filtered out");
  const uint32_t recipes[] = {0, 2, 3};
  for (int i = 0; i < 3; i++)
    expect(sub.recipeOrigin[i] == recipes[i], "recipeOrigin is ascending");

  // rA consumes the pseudo (subgraph node 2) and outputs the start item.
  const auto inputs = sub.graph.inputsOf(0);
  expect(inputs.size() == 1 && inputs[0] == 2, "rA consumes the renumbered pseudo");
  expect(sub.graph.output[0] == 0, "rA output item");

  // The pseudo's synthetic recipes need no workstation and are always kept.
  expect(sub.graph.output[1] == 2 && sub.graph.output[2] == 2, "synthetic outputs");
  const auto pseudoRecipes = sub.graph.producersOf(2);
  expect(pseudoRecipes.size() == 2 && pseudoRecipes[0] == 1 &&
         pseudoRecipes[1] == 2,
         "the pseudo expands to both synthetic recipes");

  // With nothing allowed, no real recipe survives and only the output remains.
  const std::span<const aw::Handle> noWorkstations;
  aw::Subgraph none = aw::reachableSubgraph(1, noWorkstations);
  expect(none.graph.nItem == 1 && none.graph.nRecipe == 0, "empty workstation set");
  expect(none.itemOrigin.size() == 1 && none.itemOrigin[0] == 0, "only the output");

  // An out-of-range output produces an empty subgraph rather than reading OOB.
  aw::Subgraph bad = aw::reachableSubgraph(99, allowed);
  expect(bad.graph.nItem == 0 && bad.graph.nRecipe == 0, "invalid output");

  // The renumbered subgraph must satisfy the same CSR invariants as a parsed
  // graph, with every recipe sitting in its output item's row.
  bool ok = sub.graph.i2r.offsets.size() == sub.graph.nItem + 1 &&
            sub.graph.i2r.targets.size() == sub.graph.nRecipe &&
            sub.graph.i2r.weights.size() == sub.graph.nRecipe &&
            sub.graph.r2i.offsets.size() == sub.graph.nRecipe + 1 &&
            sub.graph.output.size() == sub.graph.nRecipe;
  for (uint32_t i = 0; ok && i < sub.graph.nItem; i++)
    for (aw::RecipeId target : sub.graph.producersOf(i))
      if (target >= sub.graph.nRecipe)
        ok = false;
  for (uint32_t r = 0; ok && r < sub.graph.nRecipe; ++r) {
    if (sub.graph.output[r] >= sub.graph.nItem)
      ok = false;
    for (aw::ItemId target : sub.graph.inputsOf(r))
      if (target >= sub.graph.nItem)
        ok = false;
    bool listed = false;
    for (aw::RecipeId target : sub.graph.producersOf(sub.graph.output[r]))
      if (target == r)
        listed = true;
    if (!listed)
      ok = false;
  }
  expect(ok, "subgraph CSRs are well formed");
}

AW_TEST(testSeedUnreachablePruning) {
  std::cout << "[Test] seed reachability\n";
  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.pack.enabled = false;
  aw::options.satellite.enabled = false;

  aw::registerCraftingGraph(buildSeedSample());
  expect(aw::getCraftingError() == nullptr, "seed sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nReal == 3 && graph.nItem == 3 && graph.nRecipe == 3,
           "seed sample shape");
  }

  const aw::Handle onlyT[] = {1};  // rT and rT2 run; rA needs workstation 2.
  aw::vector<aw::Amount> inventory(3, 0);

  // With seed pruning off, the raw-material leaves stay visible.
  aw::options.seedPruning = false;
  {
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, inventory);
    expect(sub.graph.nItem == 3 && sub.graph.nRecipe == 2,
           "seed pruning off keeps the raw-material leaves");
    expect(subgraphHasRecipe(sub, 0), "rT survives with seed pruning off");
    expect(sub.translate(1) != UINT32_MAX, "item 2 survives with seed pruning off");
  }

  // With it on and nothing in stock, neither leaf can be reached, so every
  // recipe that needs one is dropped.
  aw::options.seedPruning = true;
  {
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, inventory);
    expect(sub.graph.nItem == 1 && sub.graph.nRecipe == 0,
           "seed pruning drops the unseeded branches");
    expect(!subgraphHasRecipe(sub, 0), "the dead consumer is dropped");
    expect(!subgraphHasRecipe(sub, 2), "the unreachable producer is kept out");
    expect(sub.translate(0) != UINT32_MAX, "the target survives");
    expect(sub.translate(1) == UINT32_MAX, "the dead item is gone");
  }

  // Stock on item 2 seeds rT, so only that branch survives.
  {
    aw::vector<aw::Amount> stocked = inventory;
    stocked[1] = 5;  // item 2
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, stocked);
    expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 1,
           "stock keeps the branch");
    expect(subgraphHasRecipe(sub, 0), "rT survives when its input is stocked");
    expect(!subgraphHasRecipe(sub, 1), "the unseeded sibling is dropped");
  }

  // Stock on item 3 seeds the other branch instead.
  {
    aw::vector<aw::Amount> stocked = inventory;
    stocked[2] = 10;  // item 3, the shared raw input
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, stocked);
    expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 1,
           "the other raw input keeps its own branch");
    expect(subgraphHasRecipe(sub, 1), "rT2 survives when item 3 is stocked");
    expect(!subgraphHasRecipe(sub, 0), "the unseeded sibling is dropped");
  }

  // Seed pruning must not change the plan optimum.
  {
    aw::vector<aw::Amount> stocked = inventory;
    stocked[2] = 10;  // item 3, the shared raw input
    auto total = [](const aw::PlanResult &plan) {
      int64_t sum = 0;
      for (int64_t x : plan.exec)
        sum += x;
      return sum;
    };
    aw::options.seedPruning = false;
    const aw::Subgraph full = aw::reachableSubgraph(1, onlyT, stocked);
    const aw::PlanResult fullPlan = aw::planCrafting(full, full.translate(0), 1, stocked);
    aw::options.seedPruning = true;
    const aw::Subgraph pruned = aw::reachableSubgraph(1, onlyT, stocked);
    const aw::PlanResult prunedPlan =
        aw::planCrafting(pruned, pruned.translate(0), 1, stocked);
    expect(fullPlan.status == prunedPlan.status, "seed: status agrees");
    expect(fullPlan.status != aw::PlanStatus::OK || total(fullPlan) == total(prunedPlan),
           "seed: optimum agrees");
  }

  // A byproduct that is the only source of a needed input. With nothing in
  // stock the closure only fires r1 (it has no inputs); crediting its byproduct
  // row is what makes r0's input reachable, so both recipes must survive.
  aw::registerCraftingGraph(buildByproductSeedSample());
  expect(aw::getCraftingError() == nullptr, "byproduct seed sample parses");
  {
    aw::vector<aw::Amount> empty(3, 0);
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, empty);
    expect(sub.graph.nItem == 3 && sub.graph.nRecipe == 2,
           "a byproduct keeps its consumer's branch alive");
    expect(subgraphHasRecipe(sub, 0), "the target recipe survives");
    expect(subgraphHasRecipe(sub, 1), "the byproduct producer survives");
    const aw::PlanResult plan = aw::planCrafting(sub, sub.translate(0), 1, empty);
    expect(plan.status == aw::PlanStatus::OK, "the byproduct route plans");
    if (plan.status == aw::PlanStatus::OK)
      expect(plan.exec[1] == 1, "one execution of the byproduct recipe");
  }

  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.pack.enabled = true;
  aw::options.satellite.enabled = true;
}

}  // namespace plan
