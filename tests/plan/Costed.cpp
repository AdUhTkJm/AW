// Recipe cost tests
//
// The cost-aware objective, where a recipe executes a batch per column.

#include "plan/Harness.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// A recipe's cost is how many times the underlying Minecraft recipe runs per
// execution of the column: 1 for an ordinary recipe, the batch size for a
// level of a chanced recipe, 0 for a tag edge. Everything below checks that the
// planner charges it, and that the pruning conditions that drop a recipe only
// do so when the replacement is not more expensive per execution.

// The planner's objective, recomputed from the firing vector: what the plan
// costs, not how many recipes it fires. The two differ by the batch factor.
int64_t planObjective(const aw::Subgraph &sub, const aw::PlanResult &plan) {
  int64_t cost = 0;
  for (uint32_t r = 0; r < sub.graph.nRecipe; r++)
    if (sub.graph.output[r] < sub.graph.nReal)
      cost += sub.graph.cost[r] * plan.exec[r];
  return cost;
}

// Two routes to X: a single unit per execution, and a batch of 8 that is worth
// `batchCost` real executions. Minimising executions takes the batch; the
// cost-aware planner only takes it when it is also cheaper in real executions.
aw::vector<std::byte> buildCostedObjectiveSample(int64_t batchCost) {
  AwrWriter w;
  w.header(1, 1);
  w.item(1, 2);
  w.recipe(1, {1}, {});
  w.cost(batchCost);
  w.recipe(8, {1}, {});
  return w.out;
}

}  // namespace

AW_TEST(testCostedObjective) {
  std::cout << "[Test] the objective charges the recipe cost\n";
  aw::registerCraftingGraph(buildCostedObjectiveSample(100));
  expect(aw::getCraftingError() == nullptr, "the costed objective sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  expect(graph.nRecipe == 2 && graph.cost[0] == 1 && graph.cost[1] == 100,
         "both routes keep their costs");

  const aw::Handle all[] = {1};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  const aw::ItemId target = sub.translate(0);

  // 16 X: two executions of the batch (200 real executions) against sixteen
  // ordinary ones (16). Counting executions picks the batch, so a plan that
  // pays 16 is only reachable by charging the batch its size.
  const aw::PlanResult plan = aw::planCrafting(sub, target, 16, {});
  expect(plan.status == aw::PlanStatus::OK, "the costed objective sample plans");
  expect(plan.provenOptimal, "the costed objective optimum is proven");
  expect(planObjective(sub, plan) == 16, "the plan pays 16, not the batch's 200");
  expect(plan.exec[0] == 16 && plan.exec[1] == 0, "the cheap route is the one executed");

  // A batch that is cheaper per unit: with both columns at cost 1, 100 X is
  // thirteen batches of eight (thirteen executions) against a hundred singles.
  aw::registerCraftingGraph(buildCostedObjectiveSample(1));
  expect(aw::getCraftingError() == nullptr, "the equal-cost sample parses");
  const aw::Subgraph equalSub = aw::reachableSubgraph(1, all);
  const aw::PlanResult batched = aw::planCrafting(equalSub, equalSub.translate(0), 100, {});
  expect(batched.status == aw::PlanStatus::OK && batched.provenOptimal,
         "the equal-cost sample is solved to optimality");
  expect(planObjective(equalSub, batched) == 13,
         "thirteen batches of eight beat a hundred singles");
}

AW_TEST(testGreedyCost) {
  std::cout << "[Test] the greedy pre-pass ranks routes by cost\n";
  aw::registerCraftingGraph(buildCostedObjectiveSample(100));
  expect(aw::getCraftingError() == nullptr, "the greedy cost sample parses");
  const aw::Handle all[] = {1};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);

  const aw::vector<int64_t> greedy = aw::greedyDagPlan(sub, sub.translate(0), 16, {});
  expect(!greedy.empty(), "the greedy finds a plan for the cost sample");
  int64_t cost = 0;
  for (uint32_t r = 0; r < sub.graph.nRecipe; r++)
    if (sub.graph.output[r] < sub.graph.nReal)
      cost += sub.graph.cost[r] * greedy[r];
  expect(cost == 16, "the greedy takes the cheaper route, not the shorter one");
  expect(greedy[0] == 16 && greedy[1] == 0, "sixteen singles beat one batch of eight");
}

AW_TEST(testCostedCanonicalization) {
  std::cout << "[Test] canonicalization keeps two costs apart\n";
  // The same column twice plus the same column at another cost. The two equal
  // ones merge (their workstations union), the expensive one must not: it is a
  // different column, and merging it would either drop a route or charge the
  // wrong number of executions.
  AwrWriter w;
  w.header(2, 2);
  w.item(1, 3);
  w.recipe(1, {1}, {});
  w.recipe(1, {1, 2}, {});
  w.cost(5);
  w.recipe(1, {2}, {});
  w.item(2, 0);
  aw::registerCraftingGraph(w.out);
  expect(aw::getCraftingError() == nullptr, "the costed canonicalization sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  expect(graph.nRecipe == 2, "only the equally costed twins merge");
  expect(graph.cost.size() == graph.nRecipe, "the cost array matches the recipe count");
  expect(graph.cost[0] == 1 && graph.cost[1] == 5,
         "the merged recipe keeps cost 1 and the other keeps 5");
  const auto mergedStations = graph.workstations.targetsOf(0);
  expect(mergedStations.size() == 2 && mergedStations[0] == 0 && mergedStations[1] == 1,
         "merging unions the workstations");
}

AW_TEST(testCostedDirectDominance) {
  std::cout << "[Test] direct dominance needs the dominator not to cost more\n";
  // v_single <= v_batch, so one execution of the batch replaces one of the
  // single. That is only a replacement when it is also no more expensive; a
  // batch worth 100 executions is not.
  aw::registerCraftingGraph(buildCostedObjectiveSample(100));
  expect(aw::getCraftingError() == nullptr, "the costed direct sample parses");
  expect(aw::getCraftingGraph().recipeDirectDominated[0] == 0,
         "a cheap single is not dominated by an expensive batch");

  aw::registerCraftingGraph(buildCostedObjectiveSample(1));
  expect(aw::getCraftingError() == nullptr, "the equal-cost direct sample parses");
  expect(aw::getCraftingGraph().recipeDirectDominated[0] == 1,
         "with equal costs the batch dominates the single");
}

AW_TEST(testCostedCompositePruning) {
  std::cout << "[Test] composite dominance charges the inlined witness\n";
  // X <- Y loses to X <- (nothing) once one execution of Y's producer is
  // added, because that pair makes exactly one X. The pair costs two real
  // executions, so it may only be replaced by a recipe that costs at most two.
  auto build = [](int64_t replacementCost) {
    AwrWriter w;
    w.header(2, 2);
    w.item(1, 2);
    w.recipe(1, {1}, {{2, 1}});
    w.cost(replacementCost);
    w.recipe(1, {1}, {});
    w.item(2, 1);
    w.recipe(1, {1}, {});
    return w.out;
  };

  aw::registerCraftingGraph(build(2));
  expect(aw::getCraftingError() == nullptr, "the composite cost sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.recipeDominated[0] == 1,
           "a replacement that costs no more than the pair dominates it");
    expect(graph.recipeGuardInput[0] == 1, "the inlined witness is the guard");
  }

  aw::registerCraftingGraph(build(5));
  expect(aw::getCraftingError() == nullptr, "the expensive composite sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "a replacement that costs more than the pair does not dominate it");
}

AW_TEST(testCostedSubstitutionPruning) {
  std::cout << "[Test] substitution dominance needs the replacement to cost no more\n";
  // X <- Y and X <- w, with Y <- w: one execution of the w route stands in for
  // one of the Y route, so it may only replace it when it is not dearer. The
  // third recipe of X is untouched by every relation, so the dead-node pass
  // keeps the first sibling instead of clearing the certificate under test.
  auto build = [](int64_t wCost) {
    AwrWriter w;
    w.header(3, 3);
    w.item(1, 3);
    w.recipe(1, {1}, {{2, 1}});
    w.cost(wCost);
    w.recipe(1, {1}, {{3, 1}});
    w.recipe(1, {1}, {});
    w.item(2, 1);
    w.recipe(1, {1}, {{3, 1}});
    w.item(3, 0);
    return w.out;
  };

  aw::registerCraftingGraph(build(1));
  expect(aw::getCraftingError() == nullptr, "the substitution cost sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 4 && graph.nReal == 3, "the substitution cost sample shape");
    expect(graph.cost[0] == 1 && graph.cost[1] == 1 && graph.cost[2] == 1,
           "the sample costs are the ordinary one");
    expect(graph.recipeSubstituted[0] == 1,
           "a no-dearer replacement substitutes the witness route");
  }

  aw::registerCraftingGraph(build(5));
  expect(aw::getCraftingError() == nullptr, "the expensive substitution sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.cost[1] == 5, "the w route is the dear one");
    expect(graph.recipeSubstituted[0] == 0,
           "a dearer replacement does not substitute the witness route");
  }
}

AW_TEST(testCostedTagCover) {
  std::cout << "[Test] the tag column-cover rule charges the replacement\n";
  // T has members m and w. m is only made by m <- q, where q is gathered and
  // not w, so m is dominated by w only through the column-cover rule: a
  // producer of w must yield at least as much and consume no more. That
  // replacement is one execution for one, so it must not cost more.
  auto build = [](int64_t wCost) {
    AwrWriter w;
    w.header(3, 4);
    w.item(1, 1);
    w.recipe(1, {1}, {{3, 1}});
    w.cost(wCost);
    w.item(2, 1);
    w.recipe(1, {1}, {});
    w.item(3, 0);
    w.item(4, 2);
    w.recipe(1, {}, {{1, 1}});
    w.recipe(1, {}, {{2, 1}});
    return w.out;
  };

  aw::registerCraftingGraph(build(1));
  expect(aw::getCraftingError() == nullptr, "the tag cover sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 4 && graph.nReal == 3, "the tag cover sample shape");
    // The tag edges are the two last recipes, in member order: T <- m first.
    expect(graph.tagEdgeDominated[2] == 1,
           "a no-dearer w covers m, so the T <- m edge is dropped");
    expect(graph.tagEdgeDominated[3] == 0, "the T <- w edge stays");
  }

  aw::registerCraftingGraph(build(5));
  expect(aw::getCraftingError() == nullptr, "the expensive tag cover sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.tagEdgeDominated[2] == 0,
           "a dearer w does not cover m, so the edge survives");
  }
}

AW_TEST(testCostedObjectiveCap) {
  std::cout << "[Test] a capped objective cannot use a column that alone costs more\n";
  // The domain of a costed column is `cap / cost`, not `cap`: with a cap of 1
  // the batch of 8 at cost 100 is fixed to zero, and only the single survives.
  aw::registerCraftingGraph(buildCostedObjectiveSample(100));
  expect(aw::getCraftingError() == nullptr, "the objective cap sample parses");
  const aw::Handle all[] = {1};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  aw::solver::Options options;
  options.objectiveCap = 1;
  const aw::PlanResult plan = aw::planCrafting(sub, sub.translate(0), 1, {}, options);
  expect(plan.status == aw::PlanStatus::OK, "one unit is reachable within a cap of 1");
  expect(plan.exec[0] == 1 && plan.exec[1] == 0,
         "the batch column cannot fit a cap below its cost");
}

}  // namespace plan
