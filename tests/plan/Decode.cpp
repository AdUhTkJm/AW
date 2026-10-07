// Decoding and registration tests
//
// Reading a .awr blob, registering the graph and the registration-time checks.

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

// A dump that exercises: an item with several recipes, an item with none, a
// pseudo-resource, an empty-input recipe and the workstation encoding the
// current Java writer produces.
//
//   item 1 <- r0 (x4, needs item2 x2 + item3 x1), r1 (x1, needs item1 x5,
//        workstations [1, 2])
//   item 3 <- r2 (x1, needs item1 x1 + item3 x1)
aw::vector<std::byte> buildSample() {
  AwrWriter w;
  w.header(2, 2);
  w.item(1, 2);
  w.recipe(4, {}, {{2, 2}, {3, 1}});
  w.recipe(1, {1, 2}, {{2, 5}});
  w.item(3, 1);
  w.recipe(1, {}, {{1, 1}, {3, 1}});
  return w.out;
}

// A dump with three real recipes for handle 1, all with a workstation:
//
//   r0: item 1 x1 <- item 1 x1      (consumes as much as it makes)
//   r1: item 1 x1 <- item 1 x2      (consumes more than it makes)
//   r2: item 1 x3 <- item 1 x1      (the only net producer)
//
// Registration must drop r0 and r1 and keep r2, with no query involved.
aw::vector<std::byte> buildLossySample() {
  AwrWriter w;
  w.header(1, 1);
  w.item(1, 3);
  w.recipe(1, {1}, {{1, 1}});
  w.recipe(1, {1}, {{1, 2}});
  w.recipe(3, {1}, {{1, 1}});
  return w.out;
}

// The case where the choice matters: item 1's *only* recipe consumes 0 of item
// 2, and nothing produces item 2.
//
//   item 1 x1 <- item 2 x0   (r0, workstations [1])
//
// The x0 edge is dropped and the recipe survives, so item 1 stays feasible.
// Dropping the recipe instead would make item 1 unreachable for every query.
aw::vector<std::byte> buildDegenerateOnlyInputSample() {
  AwrWriter w;
  w.header(2, 1);  // real handles 1..2, one entry, for handle 1
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 0}});
  return w.out;
}

}  // namespace

AW_TEST(testSample) {
  std::cout << "[Test] sample dump\n";
  const aw::vector<std::byte> bytes = buildSample();
  aw::registerCraftingGraph(bytes);
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  expect(graph.nReal == 2, "realResourceCount");
  expect(graph.nItem == 3, "itemCount");
  // r0 is a real recipe with an empty workstation set: it can never be entered
  // by the walk, so registration drops it. r2 produces the pseudo-resource and
  // is kept even though its workstation set is empty too, and even though it
  // consumes its own pseudo output: the net-loss rule only touches real items.
  expect(graph.nRecipe == 2, "recipeCount");
  expect(!graph.isRealItem(2), "handle 3 is a pseudo-resource");
  expect(graph.isRealItem(0) && graph.isRealItem(1), "handles 1 and 2 are real");

  const auto itemTargets = graph.producersOf(0);
  expect(itemTargets.size() == 1 && itemTargets[0] == 0, "item 1 -> recipe 0");
  const auto itemWeights = graph.producedAmountsOf(0);
  expect(itemWeights.size() == 1 && itemWeights[0] == 1, "item 1 edge weights");
  expect(graph.producersOf(1).empty(), "item 2 has no producing recipe");
  const auto pseudo = graph.producersOf(2);
  expect(pseudo.size() == 1 && pseudo[0] == 1, "item 3 -> recipe 1");

  const auto r0Targets = graph.inputsOf(0);
  expect(r0Targets.size() == 1 && r0Targets[0] == 1, "recipe 0 input is item 2");
  const auto r0Weights = graph.inputAmountsOf(0);
  expect(r0Weights.size() == 1 && r0Weights[0] == 5, "recipe 0 amount");
  const auto r1Targets = graph.inputsOf(1);
  expect(r1Targets.size() == 2 && r1Targets[0] == 0 && r1Targets[1] == 2,
         "recipe 1 inputs are items 1, 3");

  expect(graph.output[0] == 0 && graph.outputAmt[0] == 1, "recipe 0 output");
  expect(graph.output[1] == 2 && graph.outputAmt[1] == 1, "recipe 1 output");

  expect(graph.workstations.numVertices() == 2, "one workstation row per recipe");
  const auto ws0 = graph.workstations.targetsOf(0);
  expect(ws0.size() == 2 && ws0[0] == 0 && ws0[1] == 1, "recipe 0 workstations");
  expect(graph.workstations.targetsOf(1).empty(), "recipe 1 has no workstations");
}

AW_TEST(testRejectsBadInput) {
  std::cout << "[Test] malformed input\n";
  // The checks below need a known-good graph to be registered already: a
  // rejected blob has to leave it untouched, so install the sample dump first.
  aw::registerCraftingGraph(buildSample());
  expect(aw::getCraftingError() == nullptr, "the sample dump registers");
  auto rejects = [](std::span<const std::byte> bytes) {
    aw::registerCraftingGraph(bytes);
    return aw::getCraftingError() != nullptr;
  };

  expect(rejects({}), "empty blob");
  const aw::vector<std::byte> wrongMagic = {std::byte{'X'}, std::byte{'W'}, std::byte{'R'},
                         std::byte{1}, std::byte{0}, std::byte{0}};
  expect(rejects(wrongMagic), "bad magic");

  aw::vector<std::byte> truncated = buildSample();
  truncated.resize(truncated.size() - 1);
  expect(rejects(truncated), "truncated");

  aw::vector<std::byte> trailing = buildSample();
  trailing.push_back(std::byte{0});
  expect(rejects(trailing), "trailing byte");

  auto zeroDelta = buildSample();
  // This is the first output delta. Handle stays 0.
  zeroDelta[6] = std::byte{0};
  expect(rejects(zeroDelta), "handle 0 output");

  // A workstation must name a real resource (handle <= realResourceCount).
  AwrWriter badStation;
  badStation.header(1, 1);     // one real resource, one entry
  badStation.item(1, 1);
  badStation.recipe(1, {2}, {});  // handle 2 > realResourceCount
  expect(rejects(badStation.out), "workstation handle out of range");

  // A rejected blob must leave the last good graph in place: the sample dump
  // registered above is still installed, and every call since bailed out early.
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  expect(graph.nReal == 2 && graph.nItem == 3 && graph.nRecipe == 2,
         "rejected blob leaves the previous graph in place");
  const auto itemTargets = graph.producersOf(0);
  expect(itemTargets.size() == 1 && itemTargets[0] == 0,
         "previous graph is still coherent after a rejected blob");

  // A failure must not poison the next attempt with a stale error.
  const aw::vector<std::byte> bad = {std::byte{'X'}};
  aw::registerCraftingGraph(bad);
  expect(aw::getCraftingError() != nullptr, "bad blob reports an error");
  aw::registerCraftingGraph(buildSample());
  expect(aw::getCraftingError() == nullptr, "good blob after a bad one succeeds");
  aw::clearCraftingError();
}

// The v3 schema carries a per-recipe cost, which is what lets the mod fold a
// chanced recipe into a batch of executions without the planner charging the
// whole batch as one step. See BaseCraftingGraph::cost.
//
//   item 1 (A) <- r0 (x4, ws [1], item 2 (B) x4)   cost 4
//   item 2 (B) <- r1 (x1, ws [1])                  cost 1
//   item 3 (T) <- r2 (x1, item 1 x1)               cost 0, T is a tag
//
// r0 has the shape of a batched chanced level: it consumes four input sets, so
// one execution of it is four executions of the underlying recipe. r2's cost is
// spelled as 5 in the blob and must still come back as 0, because picking a
// member of a tag is free and the reader owns that invariant.
AW_TEST(testRecipeCost) {
  std::cout << "[Test] recipe cost parsing\n";
  auto rejects = [](std::span<const std::byte> bytes) {
    aw::registerCraftingGraph(bytes);
    return aw::getCraftingError() != nullptr;
  };

  AwrWriter w;
  w.header(2, 3);
  w.item(1, 1);
  w.cost(4);
  w.recipe(4, {1}, {{2, 4}});
  w.item(2, 1);
  w.recipe(1, {1}, {});
  w.item(3, 1);
  w.cost(5);
  w.recipe(1, {}, {{1, 1}});
  aw::registerCraftingGraph(w.out);
  expect(aw::getCraftingError() == nullptr, "a v3 blob parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  expect(graph.nReal == 2 && graph.nItem == 3 && graph.nRecipe == 3, "v3 sample shape");
  expect(graph.cost.size() == graph.nRecipe, "there is one cost per recipe");
  expect(graph.cost[0] == 4, "a batch level keeps its batch size as its cost");
  expect(graph.cost[1] == 1, "an ordinary recipe costs one execution");
  expect(graph.cost[2] == 0, "a tag edge is free whatever the blob spells");

  // The two invariants the solver relies on: a real column is bounded by the
  // objective cap because it costs at least one, and a tag edge is unbounded
  // only because the model caps it from the demand side.
  AwrWriter free;
  free.header(1, 1);
  free.item(1, 1);
  free.cost(0);
  free.recipe(1, {1}, {});
  aw::registerCraftingGraph(free.out);
  expect(aw::getCraftingError() == nullptr, "a v3 blob with a free real recipe parses");
  expect(aw::getCraftingGraph().cost[0] == 1,
         "a real recipe is clamped to one execution");

  // The older layouts have no cost field: every real recipe cost exactly one
  // execution and every pseudo-resource was free, so they must still load.
  AwrWriter v2;
  v2.version(2);
  v2.header(1, 1);
  v2.item(1, 1);
  v2.recipe(1, {1}, {});
  aw::registerCraftingGraph(v2.out);
  expect(aw::getCraftingError() == nullptr, "a v2 blob still parses");
  expect(aw::getCraftingGraph().cost[0] == 1, "a v2 recipe costs one execution");

  AwrWriter v1;
  v1.version(1);
  v1.header(1, 1);
  v1.item(1, 1);
  v1.recipe(1, {1}, {});
  aw::registerCraftingGraph(v1.out);
  expect(aw::getCraftingError() == nullptr, "a v1 blob still parses");
  expect(aw::getCraftingGraph().cost[0] == 1, "a v1 recipe costs one execution");

  // A cost the Java generator cannot produce is a corrupt blob, not a value to
  // clamp: a huge one would empty the objective cap and make every query
  // infeasible.
  AwrWriter tooBig;
  tooBig.header(1, 1);
  tooBig.item(1, 1);
  tooBig.cost((std::int64_t) INT32_MAX + 1);
  tooBig.recipe(1, {1}, {});
  expect(rejects(tooBig.out), "a recipe cost above the generator's range is rejected");

  AwrWriter negative;
  negative.header(1, 1);
  negative.item(1, 1);
  negative.cost(-1);
  negative.recipe(1, {1}, {});
  expect(rejects(negative.out), "a negative recipe cost is rejected");
}

AW_TEST(testNetLossRecipes) {
  std::cout << "[Test] net-loss recipe drop\n";
  aw::registerCraftingGraph(buildLossySample());
  expect(aw::getCraftingError() == nullptr, "lossy sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // The two recipes that consume at least as much as they make are gone, even
  // though each has a workstation. Only the net producer survives.
  expect(graph.nRecipe == 1, "net-loss recipes are dropped at registration");
  expect(graph.output[0] == 0 && graph.outputAmt[0] == 3, "the survivor is r2");
  const auto inputs = graph.inputsOf(0);
  const auto weights = graph.inputAmountsOf(0);
  expect(inputs.size() == 1 && inputs[0] == 0 && weights[0] == 1,
         "the survivor keeps its input");

  // The drop is baked into the graph, so no query can resurrect the losses,
  // not even one that holds stock of the item.
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.satellite.enabled = false;
  const aw::Handle all[] = {1};
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[0] = 100;
  const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
  aw::options.directPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.recipePruning = true;
  expect(sub.graph.nRecipe == 1 && sub.graph.outputAmt[0] == 3,
         "stock cannot resurrect a dropped net-loss recipe");
}

AW_TEST(testNonPositiveInputs) {
  std::cout << "[Test] degenerate input drop\n";
  aw::registerCraftingGraph(buildZeroInputSample());
  expect(aw::getCraftingError() == nullptr, "zero-input sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // The x0 edges are gone before any pass can read an amount.
  for (uint32_t r = 0; r < graph.nRecipe; r++) {
    for (aw::Amount amount : graph.inputAmountsOf(r))
      expect(amount > 0, "no non-positive input amount survives registration");
  }
  expect(graph.nRecipe == 3, "the x0 edges are dropped, not their recipes");
  expect(graph.r2i.numEdges() == 2, "a dropped edge leaves no empty slot behind");
  expect(graph.producersOf(0).size() == 3, "all three survivors still make item 1");

  // r0, then the fold of r1 and r2, then r3. The fold keeps the lowest recipe id
  // and gains both workstations.
  expect(graph.outputAmt[0] == 4 && graph.outputAmt[1] == 1 && graph.outputAmt[2] == 5,
         "surviving output amounts keep their file order");
  expect(graph.inputsOf(0).size() == 1 && graph.r2i.targetsOf(0)[0] == 1 &&
             graph.inputAmountsOf(0)[0] == 2,
         "r0 is untouched");
  expect(graph.inputsOf(1).size() == 1 && graph.r2i.targetsOf(1)[0] == 1 &&
             graph.inputAmountsOf(1)[0] == 1,
         "the folded recipe keeps only the real input");
  const auto foldedWs = graph.workstations.targetsOf(1);
  expect(foldedWs.size() == 2 && foldedWs[0] == 0 && foldedWs[1] == 1,
         "recipes that differed only by a degenerate input fold, workstations united");
  expect(graph.inputsOf(2).empty(), "r3 keeps its output with no input at all");

  // An input-less recipe is not a curiosity to be husked out later: it is the
  // only route that does not need the unstocked leaf, and the plan must use it.
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.satellite.enabled = false;
  aw::options.seedPruning = false;
  const aw::Handle all[] = {1, 2};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  expect(sub.graph.nRecipe == 3, "the input-less recipe reaches the subgraph");
  const aw::PlanResult plan = aw::planCrafting(sub, sub.translate(0), 1, {});
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.substitutionPruning = true;
  aw::options.directPruning = true;
  aw::options.recipePruning = true;
  expect(plan.status == aw::PlanStatus::OK && plan.provenOptimal, "the plan is proven optimal");
  expect(plan.exec.size() == 3 && plan.exec[0] == 0 && plan.exec[1] == 0 && plan.exec[2] == 1,
         "one firing of the input-less recipe covers the request");

  // With seed pruning on, the two routes into the unstocked leaf are dropped
  // and the input-less recipe is the only survivor.
  const aw::Subgraph seeded = aw::reachableSubgraph(1, all);
  expect(seeded.graph.nRecipe == 1 && subgraphHasRecipe(seeded, 2),
         "seed pruning drops the routes into the unstocked leaf");
}

AW_TEST(testDegenerateOnlyInput) {
  std::cout << "[Test] a degenerate input is dropped, not the recipe\n";
  aw::registerCraftingGraph(buildDegenerateOnlyInputSample());
  expect(aw::getCraftingError() == nullptr, "degenerate-only sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  expect(graph.nRecipe == 1, "the only recipe for item 1 survives");
  expect(graph.inputsOf(0).empty(), "its x0 input is gone");
  expect(graph.producersOf(0).size() == 1, "item 1 is still produced");

  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.satellite.enabled = false;
  const aw::Handle all[] = {1, 2};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  const aw::PlanResult plan = aw::planCrafting(sub, sub.translate(0), 1, {});
  aw::options.satellite.enabled = true;
  aw::options.substitutionPruning = true;
  aw::options.directPruning = true;
  aw::options.recipePruning = true;
  expect(plan.status == aw::PlanStatus::OK, "item 1 stays feasible");
  expect(plan.exec.size() == 1 && plan.exec[0] == 1, "the surviving recipe is the route");
}

}  // namespace plan
