// Solver tests
//
// The integer solver in isolation, driven by hand-built matrices.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// Recipes that differ only in their workstation set are the same LP column,
// so registration folds them together.
//
//   item 1 <- rA (x1, workstations [1],    input item 2 x1)  \ same key,
//          <- rB (x1, workstations [2, 3], input item 2 x1)  / merged
//          <- rC (x2, workstations [1],    input item 2 x1)  different amount
//          <- rD (x1, workstations [1],    input item 2 x2)  different input
//
// item 2 is a leaf, item 3 is an unused real resource.
aw::vector<std::byte> buildDuplicateSample() {
  AwrWriter w;
  w.header(3, 1);
  w.item(1, 4);
  w.recipe(1, {1}, {{2, 1}});
  w.recipe(1, {2, 3}, {{2, 1}});
  w.recipe(2, {1}, {{2, 1}});
  w.recipe(1, {1}, {{2, 2}});
  return w.out;
}

aw::solver::Matrix makeMatrix(
    uint32_t rows, uint32_t cols,
    const aw::vector<aw::vector<std::pair<uint32_t, int64_t>>> &columns) {
  aw::solver::Matrix A(rows, cols);
  A.colStart.reserve(cols);
  A.colStart.push_back_unchecked(0);
  for (uint32_t j = 0; j < cols; j++) {
    for (const auto &entry : columns[j]) {
      A.rowIndex.push_back(entry.first);
      A.value.push_back(entry.second);
    }
    A.colStart.push_back_unchecked((uint32_t) A.rowIndex.size());
  }
  return A;
}

}  // namespace

AW_TEST(testSolver) {
  std::cout << "[Test] integer solver\n";

  // min x0 + x1  s.t.  x0 + 2 x1 >= 4, x0 >= 1.
  // The LP relaxation is 2.5 at (1, 1.5); whole executions cost 3.
  {
    const aw::solver::Matrix A = makeMatrix(2, 2, {{{0, 1}, {1, 1}}, {{0, 2}}});
    const aw::vector<int64_t> b = {4, 1};
    const aw::vector<int64_t> c = {1, 1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK, "simple solve succeeds");
    expect(r.provenOptimal, "small solve is proven optimal");
    expect(r.objective == 3, "integer objective rounds the LP optimum up");
    expect(r.x.size() == 2 && r.x[0] + 2 * r.x[1] >= 4 && r.x[0] >= 1,
           "integer solution stays feasible");
  }

  // 2 x0 >= 5 has no integer point at 2.5, so the solver must round up.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 2}}});
    const aw::vector<int64_t> b = {5};
    const aw::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.objective == 3,
           "integrality rounds a fractional bound up");
  }

  // An empty row reads 0 >= b, so a positive requirement is unsatisfiable.
  {
    const aw::solver::Matrix A = makeMatrix(2, 1, {{{0, 1}}});
    const aw::vector<int64_t> b = {1, 1};
    const aw::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::INFEASIBLE, "infeasible model is detected");
  }

  // x0 >= 5 and -x0 >= -1 cannot both hold.
  {
    const aw::solver::Matrix A = makeMatrix(2, 1, {{{0, 1}, {1, -1}}});
    const aw::vector<int64_t> b = {5, -1};
    const aw::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::INFEASIBLE, "contradictory rows are detected");
  }

  // A negative right hand side is free starting stock.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 1}}});
    const aw::vector<int64_t> b = {-5};
    const aw::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.objective == 0 && r.x[0] == 0,
           "negative rhs needs no production");
  }

  // A starting cap below the requirement must be grown, not treated as a
  // hard limit.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 1}}});
    const aw::vector<int64_t> b = {5};
    const aw::vector<int64_t> c = {1};
    aw::solver::Options tight;
    tight.objectiveCap = 1;
    const aw::solver::Result r = aw::solver::solve(A, b, c, tight);
    expect(r.status == aw::PlanStatus::OK && r.objective == 5,
           "grows the objective cap until it stops binding");
  }

  // One worker and a fixed seed must be reproducible.
  {
    const aw::solver::Matrix A = makeMatrix(1, 2, {{{0, 1}}, {{0, 1}}});
    const aw::vector<int64_t> b = {7};
    const aw::vector<int64_t> c = {1, 2};
    aw::solver::Options single;
    single.numWorkers = 1;
    const aw::solver::Result first = aw::solver::solve(A, b, c, single);
    const aw::solver::Result second = aw::solver::solve(A, b, c, single);
    expect(first.status == aw::PlanStatus::OK && first.objective == 7 &&
               second.objective == first.objective && second.x == first.x,
           "single worker search is reproducible");
  }
}

AW_TEST(testReducedCostFixing) {
  std::cout << "[Test] reduced-cost fixing\n";

  // min x0 + x1  s.t.  2 x0 >= 7.  The LP is 3.5 while the integer optimum is
  // 4, and x1 is pure overhead, so x1's reduced cost (1) exceeds the 0.5 gap.
  const aw::solver::Matrix A = makeMatrix(1, 2, {{{0, 2}}, {}});
  const aw::vector<int64_t> b = {7};
  const aw::vector<int64_t> c = {1, 1};

  {
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    const aw::solver::Result r = aw::solver::solve(A, b, c, options);
    expect(r.status == aw::PlanStatus::OK && r.provenOptimal,
           "reduced-cost fixing still proves optimality");
    expect(r.objective == 4, "reduced-cost fixing keeps the optimum");
    expect(r.fixedColumns == 1, "the dominated column is fixed");
    expect(r.x.size() == 2 && r.x[0] == 4 && r.x[1] == 0,
           "a fixed column is reported as zero in the full solution");
  }
  {
    aw::solver::Options options;
    options.reducedCostGap = 0.0;
    const aw::solver::Result r = aw::solver::solve(A, b, c, options);
    expect(r.status == aw::PlanStatus::OK && r.objective == 4 && r.fixedColumns == 0,
           "a zero gap disables the fixing");
  }

  // Generalization: a column that can also serve the row is not fixed, but its
  // reduced cost still caps it at floor((incumbent - LP) / d1) = 1 instead of
  // dropping it. Exercise that path and check it keeps the optimum.
  {
    const aw::solver::Matrix A2 = makeMatrix(1, 2, {{{0, 2}}, {{0, 1}}});
    const aw::vector<int64_t> b2 = {7};
    const aw::vector<int64_t> c2 = {1, 1};
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    const aw::solver::Result r = aw::solver::solve(A2, b2, c2, options);
    expect(r.status == aw::PlanStatus::OK && r.provenOptimal && r.objective == 4,
           "a capped column keeps the optimum");
    expect(r.fixedColumns == 0, "a positively bounded column is not reported fixed");
  }
}

// Flash mode stops at the first feasible plan. The plan is a real one, but no
// optimality is claimed, and the reduced-cost fixing probe (an optimality
// accelerator) is skipped.
AW_TEST(testFlash) {
  std::cout << "[Test] flash mode\n";

  // The solver reads its own `flash` switch; `planCrafting` copies the
  // process-wide `aw::options.flash` into it and clears it again once a plan
  // has been rejected, so these direct `solve` calls set it on the options.

  // min x0 + x1  s.t.  x0 + 2 x1 >= 4, x0 >= 1.  The optimum is 3. Flash may
  // return any feasible point, so assert feasibility and a sound objective
  // floor rather than the exact cost.
  {
    const aw::solver::Matrix A = makeMatrix(2, 2, {{{0, 1}, {1, 1}}, {{0, 2}}});
    const aw::vector<int64_t> b = {4, 1};
    const aw::vector<int64_t> c = {1, 1};
    aw::solver::Options options;
    options.flash = true;
    const aw::solver::Result r = aw::solver::solve(A, b, c, options);
    expect(r.status == aw::PlanStatus::OK, "flash returns a plan");
    expect(r.x.size() == 2 && r.x[0] + 2 * r.x[1] >= 4 && r.x[0] >= 1,
           "the flash plan is feasible");
    expect(r.objective == r.x[0] + r.x[1] && r.objective >= 3,
           "the flash objective matches the plan and is at least the optimum");
  }

  // A cap below the requirement still has to be grown: the cap bounds the
  // search, it is not a hard limit. With x0 >= 5 the first cap of 1 is
  // infeasible and is abandoned for a cap that admits a plan.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 1}}});
    const aw::vector<int64_t> b = {5};
    const aw::vector<int64_t> c = {1};
    aw::solver::Options options;
    options.flash = true;
    options.objectiveCap = 1;
    const aw::solver::Result r = aw::solver::solve(A, b, c, options);
    expect(r.status == aw::PlanStatus::OK && r.objective >= 5,
           "flash grows a binding cap until the plan fits");
  }

  // Reduced-cost fixing is an optimality proof accelerator, so flash skips it:
  // the model that fixes a column with a 0.5 gap reports none under flash.
  {
    const aw::solver::Matrix A = makeMatrix(1, 2, {{{0, 2}}, {}});
    const aw::vector<int64_t> b = {7};
    const aw::vector<int64_t> c = {1, 1};
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    options.flash = true;
    const aw::solver::Result r = aw::solver::solve(A, b, c, options);
    expect(r.status == aw::PlanStatus::OK && r.fixedColumns == 0,
           "flash skips reduced-cost fixing");
  }

  // Clearing the switch is enough to optimize again on the same model: this is
  // what `planCrafting` does for the retries after a rejected plan.
  {
    const aw::solver::Matrix A = makeMatrix(1, 2, {{{0, 2}}, {}});
    const aw::vector<int64_t> b = {7};
    const aw::vector<int64_t> c = {1, 1};
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    options.flash = false;
    const aw::solver::Result r = aw::solver::solve(A, b, c, options);
    expect(r.status == aw::PlanStatus::OK && r.fixedColumns > 0,
           "with flash off the reduced-cost probe runs again");
  }
}

AW_TEST(testZeroCostColumns) {
  std::cout << "[Test] zero-cost columns and the natural cap\n";

  // A free column with nothing to consume it has a natural cap of zero, so the
  // solver cannot pump it even though the row is only `>=`.
  {
    const aw::solver::Matrix A = makeMatrix(2, 2, {{{0, 1}}, {{1, 1}}});
    const aw::vector<int64_t> b = {0, 1};
    const aw::vector<int64_t> c = {0, 1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.provenOptimal && r.objective == 1,
           "an unconsumed free column costs nothing and is not pumped");
    expect(r.x.size() == 2 && r.x[0] == 0 && r.x[1] == 1,
           "the natural cap pins the free column to zero");
  }

  // A free column can need more executions than the objective value: one
  // costed craft consumes eight tag units. Its domain must come from the
  // natural cap, not from `cap` itself.
  {
    const aw::solver::Matrix A = makeMatrix(2, 2, {{{0, 1}}, {{0, -8}, {1, 1}}});
    const aw::vector<int64_t> b = {0, 1};
    const aw::vector<int64_t> c = {0, 1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.provenOptimal && r.objective == 1,
           "a free column may exceed the objective cap");
    expect(r.x.size() == 2 && r.x[1] == 1 && r.x[0] >= 8,
           "the natural cap admits the eight tag units the craft needs");
  }

  // With no costed column at all there is nothing to minimize, but the model
  // is still a feasibility problem, not `x = 0`.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 1}}});
    const aw::vector<int64_t> b = {3};
    const aw::vector<int64_t> c = {0};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.objective == 0 && r.x.size() == 1 &&
               r.x[0] >= 3,
           "an all-free model is solved for feasibility");
  }
}

AW_TEST(testPlan) {
  std::cout << "[Test] crafting plan\n";
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "plan sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  const aw::Handle all[] = {1, 2};

  // The two-item loop is net-positive, so the net balance accepts r0:8 / r1:4
  // for four item 1, but nothing can fire from empty stock. The eager 2-cycle
  // cut tells the solver that before the first solve, so the instance comes
  // back INFEASIBLE; with the cuts off the balance still has a feasible plan
  // and only the post-solve check refuses it, which is the weaker answer.
  {
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    aw::options.seedPruning = true;
    expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 2, "unseeded subgraph shape");

    aw::solver::Options noCuts;
    noCuts.maxStartupGroups = 0;
    const aw::PlanResult rejected = aw::planCrafting(sub, sub.translate(0), 4, {}, noCuts);
    expect(rejected.status == aw::PlanStatus::CYCLE_UNFULFILLED,
           "an unseeded cycle is rejected");
    expect(!rejected.provenOptimal, "a rejected cycle claims nothing");

    const aw::PlanResult cut = aw::planCrafting(sub, sub.translate(0), 4, {});
    expect(cut.status == aw::PlanStatus::INFEASIBLE,
           "the 2-cycle cut proves the unseeded cycle infeasible");
  }

  // With the filter on, the unreachable loop never reaches the solver.
  {
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    expect(sub.graph.nItem == 1 && sub.graph.nRecipe == 0,
           "the unseeded loop is pruned");
    const aw::PlanResult none = aw::planCrafting(sub, sub.translate(0), 4, {});
    expect(none.status == aw::PlanStatus::INFEASIBLE, "plan without a seed is infeasible");
  }

  // One unit of the target seeds the loop, so the same recycling plan is both
  // optimal and executable.
  {
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    inventory[0] = 1;  // item 1
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    expect(target == 0, "target is found in the subgraph");
    expect(sub.translate(99) == UINT32_MAX, "missing item reports no index");
    const aw::PlanResult seeded = aw::planCrafting(sub, target, 4, inventory);
    expect(seeded.status == aw::PlanStatus::OK, "a seeded cycle is feasible");
    expect(seeded.exec.size() == 2, "plan has one count per recipe");
    expect(seeded.exec[0] == 8, "r0 count");
    expect(seeded.exec[1] == 4, "r1 count");
  }

  // 100 spare item 2 units cover the cycle losses, so r0 alone suffices.
  {
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    inventory[1] = 100;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::PlanResult stocked = aw::planCrafting(sub, sub.translate(0), 4, inventory);
    expect(stocked.status == aw::PlanStatus::OK, "plan with inventory is optimal");
    expect(stocked.exec[0] == 4, "stocked r0 count");
    expect(stocked.exec[1] == 0, "stocked r1 count");
  }
}

AW_TEST(testPlanInfeasible) {
  std::cout << "[Test] infeasible plan\n";
  aw::registerCraftingGraph(buildPlanLeafSample());
  expect(aw::getCraftingError() == nullptr, "leaf sample parses");

  const aw::Handle all[] = {1};
  // Satellite elimination and seed pruning would both drop the dead
  // raw-material route, and hide the missing leaf this test is about; keep
  // them off so the reachability shape stays visible and the solver sees it.
  aw::options.satellite.enabled = false;
  aw::options.seedPruning = false;
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;
  expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 1, "leaf subgraph shape");

  const aw::ItemId target = sub.translate(0);
  const aw::PlanResult r = aw::planCrafting(sub, target, 4, {});
  expect(r.status == aw::PlanStatus::INFEASIBLE, "missing leaf makes the plan infeasible");
}

AW_TEST(testDuplicateRecipes) {
  std::cout << "[Test] duplicate recipes\n";
  aw::registerCraftingGraph(buildDuplicateSample());
  expect(aw::getCraftingError() == nullptr, "duplicate sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // rA and rB collapse; rC and rD stay because their amounts differ.
  expect(graph.nRecipe == 3, "recipes that differ only by workstation fold together");
  expect(graph.nItem == 3 && graph.nReal == 3, "duplicate sample shape");

  // The survivor keeps rA's id (0) and order, and gains rB's workstations.
  const auto recipes = graph.producersOf(0);
  expect(recipes.size() == 3 && recipes[0] == 0 && recipes[1] == 1 &&
         recipes[2] == 2,
         "surviving recipes keep their file order");
  const auto ws = graph.workstations.targetsOf(0);
  expect(ws.size() == 3 && ws[0] == 0 && ws[1] == 1 && ws[2] == 2,
         "workstation sets are merged and deduplicated");
  expect(graph.outputAmt[0] == 1 && graph.outputAmt[1] == 2 &&
         graph.outputAmt[2] == 1,
         "output amounts survive the fold");
  expect(graph.inputAmountsOf(2).size() == 1 && graph.r2i.weightsOf(2)[0] == 2,
         "a different input amount is not folded away");
  expect(graph.i2r.numEdges() == graph.nRecipe,
         "one item -> recipe edge per surviving recipe");

  // The reachable subgraph inherits the canonicalized graph, so it cannot
  // contain two identical columns either. Recipe pruning is disabled here:
  // item 2 is an unstocked leaf, so it would collapse the three recipes on its
  // own. Satellite elimination is off for the same reason: the same leaf would
  // make the whole component dead.
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.satellite.enabled = false;
  aw::options.seedPruning = false;
  const aw::Handle all[] = {1, 2, 3};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.directPruning = true;
  aw::options.recipePruning = true;
  expect(sub.graph.nItem == 2, "only the output and its leaf are reachable");
  expect(sub.graph.nRecipe == 3, "the subgraph keeps every surviving recipe");
}

}  // namespace plan
