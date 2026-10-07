// Tag tests
//
// Tag edges: pruning, transitive closure, batching, inlining and tidying.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// T = {glass, stained}. Stained glass is gated by glass (stained x1 <- glass
// x1 + dye x1), so the synthetic edge `T <- stained` is dominated while
// `T <- glass` is not. The recipe yields one stained glass on purpose: a
// producer that emits a batch keeps its tag edge, see
// buildBatchRecycleSample() and the batching guard in Prune.cpp.
//
//   handle 1 glass   <- r0 (x1, ws [3], sand x1)
//   handle 2 stained <- r1 (x1, ws [3], glass x1 + dye x1)
//   handle 5 P       <- r2 (x1, ws [4], T x1)
//   handle 6 T       <- r3 (x1, no ws, stained x1)   (synthetic)
//                    <- r4 (x1, no ws, glass x1)     (synthetic)
aw::vector<std::byte> buildGlassSample() {
  AwrWriter w;
  w.header(5, 4);
  w.item(1, 1);
  w.recipe(1, {3}, {{4, 1}});
  w.item(2, 1);
  w.recipe(1, {3}, {{1, 1}, {3, 1}});
  w.item(5, 1);
  w.recipe(1, {4}, {{6, 1}});
  w.item(6, 2);
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{1, 1}});
  return w.out;
}

// The counterexample from docs/pruning.typ: plain reachability would call m
// dominated by w, but z is a free leaf, so `T <- m` must survive.
//
//   handle 3 w <- a x1 <- b x1 <- base x1
//   handle 6 m <- J x1
//   handle 8 T = {m, w}, handle 9 J = {w, z}, z is a leaf
// A sample where tag conversions change which route is optimal. Handles 1..6
// are real, handle 7 is the pseudo-resource T with members {1, 2}.
//
//   a <- m1, b <- a, c <- b        (route A: four real crafts to P)
//   P <- T x5, T <- m1, T <- m2    (route B: one real craft, five tag edges)
//
// With every edge costing 1, route A wins (4 < 6). With tag edges free, route
// B wins (1 < 4).
aw::vector<std::byte> buildFreeTagSample() {
  AwrWriter w;
  w.header(6, 5);
  w.item(3, 1);
  w.recipe(1, {1}, {{1, 1}});
  w.item(4, 1);
  w.recipe(1, {1}, {{3, 1}});
  w.item(5, 1);
  w.recipe(1, {1}, {{4, 1}});
  w.item(6, 2);
  w.recipe(1, {1}, {{7, 5}});
  w.recipe(1, {1}, {{5, 1}});
  w.item(7, 2);
  w.recipe(1, {}, {{1, 1}});
  w.recipe(1, {}, {{2, 1}});
  return w.out;
}

// A tag with two usable members, for the post-solve tidying pass. Handles 1..4
// are real, handle 5 is the pseudo-resource T = {m_stock, m_made}.
//
//   m_made <- m_base x1             (real)
//   P      <- T x5                  (real)
//   T      <- m_stock               (synthetic)
//   T      <- m_made                (synthetic)
//
// A plan can fire both member edges: m_stock is held, and m_made is crafted
// from the stocked m_base. They are therefore both legal answers to "which
// member fills T", and the tidying pass has to pick one; it picks m_stock,
// because a member already in stock is available before the plan runs.
aw::vector<std::byte> buildTidySample() {
  AwrWriter w;
  w.header(4, 3);
  w.item(3, 1);
  w.recipe(1, {1}, {{1, 1}});
  w.item(4, 1);
  w.recipe(1, {1}, {{5, 5}});
  w.item(5, 2);
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{3, 1}});
  return w.out;
}

aw::vector<std::byte> buildCounterSample() {  AwrWriter w;
  w.header(7, 7);
  w.item(3, 1);
  w.recipe(1, {1}, {{4, 1}});
  w.item(4, 1);
  w.recipe(1, {1}, {{5, 1}});
  w.item(5, 1);
  w.recipe(1, {1}, {{1, 1}});
  w.item(6, 1);
  w.recipe(1, {1}, {{9, 1}});
  w.item(7, 1);
  w.recipe(1, {1}, {{8, 1}});
  w.item(8, 2);
  w.recipe(1, {}, {{6, 1}});
  w.recipe(1, {}, {{3, 1}});
  w.item(9, 2);
  w.recipe(1, {}, {{3, 1}});
  w.recipe(1, {}, {{2, 1}});
  return w.out;
}

// T = {m, w} with `m x8 <- w x1`. The amount condition forbids pruning.
//
//   handle 1 m <- r0 (x8, ws [2], w x1)
//   handle 3 T <- r1 (x1, no ws, m x1)
//              <- r2 (x1, no ws, w x1)
aw::vector<std::byte> buildBulkSample() {
  AwrWriter w;
  w.header(2, 2);
  w.item(1, 1);
  w.recipe(8, {2}, {{2, 1}});
  w.item(3, 2);
  w.recipe(1, {}, {{1, 1}});
  w.recipe(1, {}, {{2, 1}});
  return w.out;
}

// The EnderIO fused-quartz shape: two variants that require each other and a
// tag that does not itself contain the dominator. Only the nonoptimal general
// tag gate can close this.
//
//   handle 1 W     <- r0 (x1, ws [4], base x1)
//   handle 2 M     <- r1 (x1, ws [4], N x1)
//                  <- r2 (x1, ws [4], W x1)
//                  <- r3 (x1, ws [4], T_A x1)
//   handle 3 N     <- r4 (x1, ws [4], M x1)
//                  <- r5 (x1, ws [4], T_N x1)
//   handle 5 T_A   <- r6 (x1, no ws, M x1)      (synthetic)
//   handle 6 T_N   <- r7 (x1, no ws, N x1)      (synthetic)
//   handle 7 T_ALL <- r8 (x1, no ws, W x1)      (synthetic)
//                  <- r9 (x1, no ws, M x1)
//                  <- r10 (x1, no ws, N x1)
aw::vector<std::byte> buildFusedQuartzSample() {
  AwrWriter w;
  w.header(4, 6);  // real handles 1..4; entries 1, 2, 3, 5, 6, 7
  w.item(1, 1);
  w.recipe(1, {4}, {{4, 1}});
  w.item(2, 3);
  w.recipe(1, {4}, {{3, 1}});
  w.recipe(1, {4}, {{1, 1}});
  w.recipe(1, {4}, {{5, 1}});
  w.item(3, 2);
  w.recipe(1, {4}, {{2, 1}});
  w.recipe(1, {4}, {{6, 1}});
  w.item(5, 1);
  w.recipe(1, {}, {{2, 1}});
  w.item(6, 1);
  w.recipe(1, {}, {{3, 1}});
  w.item(7, 3);
  w.recipe(1, {}, {{1, 1}});
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{3, 1}});
  return w.out;
}

// A chain whose middle pair is neither a witness pair nor a common-tag pair, so
// it only enters the relation once the closure materializes it. `M` consumes
// `J`, and `J` only requires `W` through the single-member tag `K`.
//
//   handle 1 W <- r0 (x1, ws [4], base x1)
//   handle 2 M <- r1 (x1, ws [4], J x1)
//   handle 3 J <- r2 (x1, ws [4], K x1)
//   handle 5 Z <- r3 (x1, ws [4], W x1)      (makes (Z, W) a witness pair)
//   handle 6 T <- r4 (x1, no ws, W x1)       (synthetic; T = {W, M})
//              <- r5 (x1, no ws, M x1)
//   handle 7 K <- r6 (x1, no ws, Z x1)       (synthetic)
aw::vector<std::byte> buildTransitiveClosureSample() {
  AwrWriter w;
  w.header(5, 6);  // real handles 1..5; entries 1, 2, 3, 5, 6, 7
  w.item(1, 1);
  w.recipe(1, {4}, {{4, 1}});
  w.item(2, 1);
  w.recipe(1, {4}, {{3, 1}});
  w.item(3, 1);
  w.recipe(1, {4}, {{7, 1}});
  w.item(5, 1);
  w.recipe(1, {4}, {{1, 1}});
  w.item(6, 2);
  w.recipe(1, {}, {{1, 1}});
  w.recipe(1, {}, {{2, 1}});
  w.item(7, 1);
  w.recipe(1, {}, {{5, 1}});
  return w.out;
}

// A pack with one "catch-all" tag: T has many members, every member is made
// from the witness w, so the tag rule would dominate every T <- m edge. The
// member count is above the tag pass's budget, whose whole point is to keep a
// tag like this (ATM10 ships two with tens of thousands of members) from
// expanding into a quadratic universe of pairs. The tag must be skipped, not
// exploded.
//
//   handle 1        w      (leaf, the witness)
//   handles 2..n+1  m_i    <- (x1, ws [w], w x1)
//   handle n+2      T      <- (x1, no ws, w x1) and <- (x1, no ws, m_i x1)
constexpr uint32_t k_wideTagMembers = 6000;

aw::vector<std::byte> buildWideTagSample() {
  const uint32_t n = k_wideTagMembers;
  const uint32_t witness = 1;

  AwrWriter w;
  w.header(n + 1, n + 1);  // real: w + m_i; entries: handles 2..n+1 and the tag

  // m_i <- w x1, on workstation w.
  for (uint32_t i = 0; i < n; i++) {
    w.item(2 + i, 1);
    w.recipe(1, {witness}, {{witness, 1}});
  }

  // T <- w, then T <- m_i for every member.
  w.item(n + 2, n + 1);
  for (uint32_t i = 0; i <= n; i++)
    w.recipe(1, {}, {{i == 0 ? witness : 1 + i, 1}});
  return w.out;
}

}  // namespace

AW_TEST(testTagPruning) {
  std::cout << "[Test] tag-edge dominance pruning\n";

  aw::registerCraftingGraph(buildGlassSample());
  expect(aw::getCraftingError() == nullptr, "glass sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nReal == 5 && graph.nItem == 6 && graph.nRecipe == 5,
           "glass sample shape");
    expect(graph.tagEdgeDominated.size() == graph.nRecipe,
           "one pruning flag per recipe");
    expect(graph.tagEdgeDominated[3] == 1, "the stained tag edge is dominated");
    expect(graph.tagEdgeDominated[4] == 0, "the glass tag edge survives");
    expect(graph.tagEdgeDominated[0] == 0 && graph.tagEdgeDominated[1] == 0 &&
               graph.tagEdgeDominated[2] == 0,
           "real recipes are never flagged");
  }

  const aw::Handle all[] = {1, 2, 3, 4, 5};
  const uint32_t nItem = aw::getCraftingGraph().nItem;
  auto keepsStainedEdge = [](const aw::Subgraph &sub) {
    for (aw::ItemId recipe : sub.recipeOrigin)
      if (recipe == 3)
        return true;
    return false;
  };

  {
    const aw::Subgraph sub = aw::reachableSubgraph(5, all);
    expect(!keepsStainedEdge(sub),
           "the dominated tag edge is dropped with no inventory");
  }
  {
    // Stocking the dominated member keeps its tag edge usable.
    aw::vector<aw::Amount> inventory(nItem, 0);
    inventory[1] = 10;  // handle 2 (stained glass)
    const aw::Subgraph sub = aw::reachableSubgraph(5, all, inventory);
    expect(keepsStainedEdge(sub), "inventory keeps the dominated tag edge");
  }
  {
    // The A/B switch turns the drop off entirely. Satellite elimination and
    // seed pruning are disabled too: they drop the same dead tag island
    // independently of the tag pass.
    aw::options.tagPruning = false;
    aw::options.satellite.enabled = false;
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(5, all);
    aw::options.seedPruning = true;
    aw::options.satellite.enabled = true;
    aw::options.tagPruning = true;
    expect(keepsStainedEdge(sub), "disabling pruning keeps every tag edge");
  }

  // The counterexample from docs/pruning.typ: a free co-member of an input tag
  // blocks the tag rule, so neither edge of T may be dropped.
  aw::registerCraftingGraph(buildCounterSample());
  expect(aw::getCraftingError() == nullptr, "counter sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 9, "counter sample shape");
    expect(graph.tagEdgeDominated[5] == 0 && graph.tagEdgeDominated[6] == 0,
           "a free co-member blocks the tag rule");
  }

  // Bulk amplification: m x8 <- w x1 must survive the amount condition.
  aw::registerCraftingGraph(buildBulkSample());
  expect(aw::getCraftingError() == nullptr, "bulk sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 3, "bulk sample shape");
    expect(graph.tagEdgeDominated[1] == 0, "bulk amplification is not dominated");
  }
}

AW_TEST(testWideTagPruning) {
  std::cout << "[Test] oversized tags are skipped\n";

  // The pack pass would spend its own budget scanning this many recipes; the
  // tag budget is what this test is about.
  aw::options.pack.enabled = false;
  aw::registerCraftingGraph(buildWideTagSample());
  aw::options.pack.enabled = true;

  expect(aw::getCraftingError() == nullptr, "wide tag sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const uint32_t n = k_wideTagMembers;
  expect(graph.nRecipe == 2 * n + 1, "wide tag sample shape");
  expect(graph.tagEdgeDominated.size() == graph.nRecipe,
         "wide tag has one pruning flag per recipe");

  // Every T <- m_i edge would be dominated if the tag were part of the
  // relation; the member edges are the last n + 1 recipes in file order.
  size_t dominated = 0;
  for (uint32_t r = n; r < graph.nRecipe; r++)
    dominated += graph.tagEdgeDominated[r];
  expect(dominated == 0, "an oversized tag is skipped instead of expanded");
}

// The general tag gate and the transitive closure. Both samples leave the
// domination relation one step short unless nonoptimal mode closes it, so the
// exact run has to keep the tag edges.
AW_TEST(testTagTransitiveClosure) {
  std::cout << "[Test] tag transitive closure\n";

  // Sample A: M and N require each other, and N only requires W through T_N,
  // which does not contain W.
  aw::options.nonoptimal = true;
  aw::registerCraftingGraph(buildFusedQuartzSample());
  expect(aw::getCraftingError() == nullptr, "fused quartz sample parses");
  {
    const aw::CraftingGraph &g = aw::getCraftingGraph();
    expect(g.nRecipe == 11, "fused quartz sample shape");
    expect(g.tagEdgeDominated[8] == 0, "the dominator keeps its tag edge");
    expect(g.tagEdgeDominated[9] == 1, "the mutual member is dropped");
    expect(g.tagEdgeDominated[10] == 1, "the other mutual member is dropped");
  }
  aw::options.nonoptimal = false;
  aw::registerCraftingGraph(buildFusedQuartzSample());
  {
    const aw::CraftingGraph &g = aw::getCraftingGraph();
    expect(g.tagEdgeDominated[9] == 0 && g.tagEdgeDominated[10] == 0,
           "the one-step relation keeps both mutual members");
  }

  // Sample B: (J, W) is neither a witness pair nor a common-tag pair, so only
  // the closure can put it into the relation.
  aw::options.nonoptimal = true;
  aw::registerCraftingGraph(buildTransitiveClosureSample());
  expect(aw::getCraftingError() == nullptr, "closure sample parses");
  {
    const aw::CraftingGraph &g = aw::getCraftingGraph();
    expect(g.nRecipe == 7, "closure sample shape");
    expect(g.tagEdgeDominated[4] == 0, "the dominator keeps its tag edge");
    expect(g.tagEdgeDominated[5] == 1, "the chained member is dropped");
  }
  aw::options.nonoptimal = false;
  aw::registerCraftingGraph(buildTransitiveClosureSample());
  expect(aw::getCraftingGraph().tagEdgeDominated[5] == 0,
         "without the closure the chained member survives");
  aw::options.nonoptimal = false;
}

AW_TEST(testTagPruningParity) {
  std::cout << "[Test] tag pruning preserves the optimum\n";
  aw::registerCraftingGraph(buildGlassSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  // Leaves are free only via inventory, so stock every item with no recipe.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::ItemId m = 0; m < graph.nReal; m++)
    if (graph.producersOf(m).empty())
      inventory[m] = 1000000000ULL;

  auto plan = [&](bool prune) {
    aw::options.tagPruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(5, all, inventory);
    const aw::ItemId target = sub.translate(4);
    const aw::PlanResult r = aw::planCrafting(sub, target, 16, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const auto full = plan(false);
  const auto pruned = plan(true);
  aw::options.tagPruning = true;
  expect(full.first == aw::PlanStatus::OK && pruned.first == aw::PlanStatus::OK,
         "both plan variants are feasible");
  expect(full.second == pruned.second,
         "pruning does not change the optimum");
}

// The batching guard's witness: a tag edge whose member is produced in batches
// is a free sink for the surplus, so dropping it raises the optimum. Before the
// guard, this sample planned in 6 real steps with tag pruning and 4 without.
AW_TEST(testTagBatchingGuard) {
  std::cout << "[Test] a batched member keeps its tag edge\n";
  aw::registerCraftingGraph(buildBatchRecycleSample());
  expect(aw::getCraftingError() == nullptr, "batch recycle sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // `T <- m`: T is pseudo handle 5, m is handle 3. It has to stay flagged as
  // undominated, because m's only recipe emits four units at a time.
  bool batchedEdgeKept = false;
  for (aw::RecipeId r : graph.producersOf(4)) {
    const auto inputs = graph.inputsOf(r);
    if (inputs.size() == 1 && inputs[0] == 2)
      batchedEdgeKept = graph.tagEdgeDominated[r] == 0;
  }
  expect(batchedEdgeKept, "the batched member T <- m survives the guard");

  const aw::Handle all[] = {1, 2, 3, 4};
  aw::vector<aw::Amount> inventory(graph.nItem, 0);

  // Count only real recipes: the synthetic tag edges are free. The sample is an
  // unseeded cycle, so keep the seed filter off to hand the solver the loop the
  // tag guard is about; the net balance still cannot be turned into a firing
  // sequence, which is what the check reports.
  auto realSteps = [&](bool prune) {
    aw::options.tagPruning = prune;
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    aw::options.seedPruning = true;
    const aw::ItemId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);
    int64_t real = 0;
    if (r.status == aw::PlanStatus::OK)
      for (uint32_t i = 0; i < sub.graph.nRecipe; i++)
        if (sub.graph.output[i] < sub.graph.nReal)
          real += r.exec[i];
    return std::pair<aw::PlanStatus, int64_t>(r.status, real);
  };

  const auto full = realSteps(false);
  const auto pruned = realSteps(true);
  aw::options.tagPruning = true;
  // The batch surplus is a free sink on paper, but the loop has no seed, so the
  // guard's effect on the optimum is asserted on the preprocessing flag above.
  expect(full.first == aw::PlanStatus::CYCLE_UNFULFILLED &&
             pruned.first == aw::PlanStatus::CYCLE_UNFULFILLED,
         "the unseeded batch recycle cycle is rejected");
}

AW_TEST(testTagInlining) {
  std::cout << "[Test] single-use tag inlining\n";
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  struct Result {
    size_t globalRecipes;
    size_t subRecipes;
    int64_t real;
  };

  // Tag edges are conceptually free, so the real part of the plan is the
  // objective that must not move.
  auto run = [&](aw::TagInlineMode mode, aw::Amount stainedStock) {
    aw::options.tagInlining = mode;
    aw::registerCraftingGraph(buildGlassSample());
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    for (aw::ItemId m = 0; m < graph.nReal; m++)
      if (graph.producersOf(m).empty())
        inventory[m] = 1000000000ULL;
    inventory[1] = stainedStock;  // handle 2: stained glass, the dominated member
    const aw::Subgraph sub = aw::reachableSubgraph(5, all, inventory);
    const aw::ItemId target = sub.translate(4);
    const aw::PlanResult r = aw::planCrafting(sub, target, 16, inventory);
    int64_t real = 0;
    if (r.status == aw::PlanStatus::OK)
      for (uint32_t i = 0; i < sub.graph.nRecipe; i++)
        if (sub.graph.output[i] < sub.graph.nReal)
          real += r.exec[i];
    return Result{graph.nRecipe, sub.graph.nRecipe, real};
  };

  // T = {stained, glass} is consumed once by P, at amount 1. With no stock, tag
  // pruning drops the stained member edge, so only glass is left to fold in.
  const auto off = run(aw::TagInlineMode::OFF, 0);
  const auto pre = run(aw::TagInlineMode::PRE_PRUNE, 0);
  const auto query = run(aw::TagInlineMode::QUERY_TIME, 0);
  expect(off.globalRecipes == 5, "off keeps the tag node and its two edges");
  expect(pre.globalRecipes == 4, "PRE removes the tag edges from the graph");
  expect(query.subRecipes < off.subRecipes,
         "QUERY_TIME removes the tag edge from the subgraph");
  expect(off.real == pre.real && off.real == query.real,
         "inlining preserves the real optimum");

  // Stocking the dominated member must keep it usable. QUERY_TIME sees the
  // stocked member edge in the walk and folds it in; filtering members at
  // registration time would have dropped it and forced a plain glass craft.
  const auto offStock = run(aw::TagInlineMode::OFF, 1000);
  const auto preStock = run(aw::TagInlineMode::PRE_PRUNE, 1000);
  const auto queryStock = run(aw::TagInlineMode::QUERY_TIME, 1000);
  expect(offStock.real == 16, "stocked stained glass is spent when off");
  expect(preStock.real == 16, "PRE still spends the stocked member");
  expect(queryStock.real == 16, "QUERY_TIME still spends the stocked member");

  aw::options.tagInlining = aw::TagInlineMode::OFF;
  aw::registerCraftingGraph(buildGlassSample());
}

// `output` must stay non-decreasing over the recipe ids: the real recipes are a
// prefix and the synthetic tag edges a suffix, which is what lets every
// real-recipe loop stop at the first tag edge. The inliner used to break that
// by appending its expansion copies at the end of the list, which put recipes
// with a small output last.
AW_TEST(testOutputOrdering) {
  std::cout << "[Test] recipe output ordering\n";

  // Item 1 <- rA (x1, workstation [1], input pseudo-3 x1). The pseudo's members
  // are item 1 and item 2, and rB makes item 2 out of nothing, so the walk
  // reaches everything. Expanding rA duplicates output item 1, which has to
  // stay ahead of rB's output item 2.
  AwrWriter w;
  w.header(2, 3);
  w.item(1, 1);
  w.recipe(1, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {1}, {});
  w.item(3, 2);
  w.recipe(1, {}, {{1, 1}});
  w.recipe(1, {}, {{2, 1}});
  const aw::vector<std::byte> bytes = w.out;

  const auto ascending = [](const aw::BaseCraftingGraph &g) {
    for (uint32_t r = 1; r < g.nRecipe; r++)
      if (g.output[r] < g.output[r - 1])
        return false;
    return true;
  };

  // Keep every pruning pass out of the way: the point is the inliner's own
  // recipe list, not the pruning that may hide an expansion.
  const bool savedTag = aw::options.tagPruning;
  const bool savedRecipe = aw::options.recipePruning;
  const bool savedDirect = aw::options.directPruning;
  const bool savedSubstitution = aw::options.substitutionPruning;
  const bool savedPack = aw::options.pack.enabled;
  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.pack.enabled = false;

  const aw::Handle all[] = {1, 2, 3};
  for (aw::TagInlineMode mode : {aw::TagInlineMode::OFF, aw::TagInlineMode::PRE_PRUNE,
                                 aw::TagInlineMode::QUERY_TIME, aw::TagInlineMode::BOTH}) {
    aw::options.tagInlining = mode;
    aw::registerCraftingGraph(bytes);
    expect(aw::getCraftingError() == nullptr, "ordering sample parses");
    expect(ascending(aw::getCraftingGraph()), "the registered graph keeps outputs ascending");
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    expect(sub.graph.nRecipe != 0, "the ordering sample stays reachable");
    expect(ascending(sub.graph), "the subgraph keeps outputs ascending");
  }

  aw::options.tagPruning = savedTag;
  aw::options.recipePruning = savedRecipe;
  aw::options.directPruning = savedDirect;
  aw::options.substitutionPruning = savedSubstitution;
  aw::options.pack.enabled = savedPack;
  aw::options.tagInlining = aw::TagInlineMode::OFF;
}

AW_TEST(testFreeTagObjective) {
  std::cout << "[Test] tag conversions are free\n";

  aw::registerCraftingGraph(buildFreeTagSample());
  expect(aw::getCraftingError() == nullptr, "free tag sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[0] = 1000000000ULL;  // m1
  inventory[1] = 1000000000ULL;  // m2

  const aw::Subgraph sub = aw::reachableSubgraph(6, all, inventory);
  const aw::ItemId target = sub.translate(5);
  const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);

  expect(r.status == aw::PlanStatus::OK, "free tag plan is feasible");

  int64_t real = 0;
  int64_t tags = 0;
  for (uint32_t i = 0; i < sub.graph.nRecipe; i++) {
    if (sub.graph.output[i] < sub.graph.nReal)
      real += r.exec[i];
    else
      tags += r.exec[i];
  }
  expect(real == 1, "the cheap route uses one real craft");
  // The tag's row is `>=`, so the solver may fire the member edges well past
  // what P consumes; the tidying pass cuts them back to exactly the five units
  // P asks for, so the response has no conversion the plan does not use.
  expect(tags == 5, "the tidied tag covers exactly the consumption");

  // The fixing pass decides a zero-cost column's neutral domain differently
  // from a costed one. Check that it does not over-tighten the tag edges.
  {
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    const aw::PlanResult fixed = aw::planCrafting(sub, target, 1, inventory, options);
    expect(fixed.status == aw::PlanStatus::OK, "reduced-cost fixing keeps the plan");
    int64_t fixedReal = 0;
    int64_t fixedTags = 0;
    for (uint32_t i = 0; i < sub.graph.nRecipe; i++) {
      if (sub.graph.output[i] < sub.graph.nReal)
        fixedReal += fixed.exec[i];
      else
        fixedTags += fixed.exec[i];
    }
    expect(fixedReal == 1, "reduced-cost fixing keeps the real optimum");
    expect(fixedTags == 5, "reduced-cost fixing is tidied too");
  }
}

// The post-solve tidying pass. The sample is small enough that both member
// edges of the tag are legal, so the test can pin down which one survives and
// how much of the demand spills onto a second edge when the first is capped.
AW_TEST(testTagTidying) {
  std::cout << "[Test] tag conversion tidying\n";

  aw::registerCraftingGraph(buildTidySample());
  expect(aw::getCraftingError() == nullptr, "tidy sample parses");

  const aw::Handle all[] = {1, 2, 3, 4, 5};
  aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
  inventory[0] = 100;  // handle 1 m_base
  inventory[1] = 100;  // handle 2 m_stock

  const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
  const aw::ItemId target = sub.translate(3);
  const aw::BaseCraftingGraph &g = sub.graph;

  // Locate the tag, its two member edges, the recipe that crafts m_made and the
  // one recipe that consumes the tag. Nothing is hardcoded: the subgraph
  // renumbers both spaces.
  aw::ItemId tag = UINT32_MAX;
  for (aw::ItemId item = g.nReal; item < g.nItem; item++)
    if (!g.producersOf(item).empty())
      tag = item;
  expect(tag != UINT32_MAX, "the tidy sample keeps its tag node");

  const aw::ItemId stockItem = sub.translate(1);  // handle 2
  const aw::ItemId madeItem = sub.translate(2);   // handle 3
  aw::RecipeId stockEdge = UINT32_MAX;
  aw::RecipeId madeEdge = UINT32_MAX;
  for (aw::RecipeId r : g.producersOf(tag)) {
    if (g.inputsOf(r)[0] == stockItem)
      stockEdge = r;
    else
      madeEdge = r;
  }
  expect(stockEdge != UINT32_MAX && madeEdge != UINT32_MAX,
         "the tag has one edge per member");

  aw::RecipeId maker = UINT32_MAX;
  aw::RecipeId consumer = UINT32_MAX;
  for (aw::RecipeId r = 0; r < g.nRecipe; r++) {
    if (g.output[r] >= g.nReal)
      continue;
    if (g.output[r] == madeItem)
      maker = r;
    for (aw::ItemId input : g.inputsOf(r))
      if (input == tag)
        consumer = r;
  }
  expect(maker != UINT32_MAX && consumer != UINT32_MAX,
         "the sample keeps its real recipes");

  // The balance vector planCrafting hands the solver: the request at the
  // target, negative stock elsewhere.
  aw::vector<int64_t> b(g.nItem, 0);
  for (aw::ItemId item = 0; item < g.nItem; item++) {
    const aw::ItemId source = sub.itemOrigin[item];
    b[item] = source < inventory.size() ? -inventory[source] : 0;
  }
  b[target] = 1;

  // A fireable plan that overproduces the tag: P consumes five units, and both
  // member edges convert ten.
  aw::vector<int64_t> exec = aw::vector<int64_t>::zeroes(g.nRecipe);
  exec[maker] = 10;
  exec[stockEdge] = 10;
  exec[madeEdge] = 10;
  exec[consumer] = 1;
  expect(aw::planIsFireable(sub, inventory,
                            std::span<const int64_t>(exec.data(), exec.size())),
         "the surplus plan fires");

  const aw::vector<int64_t> tidied = aw::tidyTagConversions(sub, inventory, b, exec);
  expect(!tidied.empty(), "the tidying pass cuts the surplus");
  expect(tidied[stockEdge] == 5 && tidied[madeEdge] == 0,
         "the demand lands on the stocked member");
  expect(tidied[consumer] == 1 && tidied[maker] == 10,
         "the tidying pass leaves real recipes alone");
  expect(aw::planIsFireable(sub, inventory,
                            std::span<const int64_t>(tidied.data(), tidied.size())),
         "the tidied plan still fires");

  {
    // The cap is the solver's own count, so a member edge that fired fewer
    // times than the demand keeps all of them and the rest spills over.
    aw::vector<int64_t> capped = exec;
    capped[stockEdge] = 2;
    const aw::vector<int64_t> spilled = aw::tidyTagConversions(sub, inventory, b, capped);
    expect(!spilled.empty() && spilled[stockEdge] == 2 && spilled[madeEdge] == 3,
           "the demand spills over to a second edge");
  }

  {
    // Nothing to drop: the tag is already consumed exactly, so the pass hands
    // back nothing and the solver's vector stands.
    aw::vector<int64_t> exact = exec;
    exact[stockEdge] = 5;
    exact[madeEdge] = 0;
    expect(aw::tidyTagConversions(sub, inventory, b, exact).empty(),
           "an exactly consumed tag is left alone");
  }

  {
    // A tag nothing consumes loses every conversion, which is what turns the
    // ATM10 plan for one item from about 89700 steps into 3.
    aw::vector<int64_t> unused = exec;
    unused[consumer] = 0;
    const aw::vector<int64_t> dropped = aw::tidyTagConversions(sub, inventory, b, unused);
    expect(!dropped.empty() && dropped[stockEdge] == 0 && dropped[madeEdge] == 0,
           "an unconsumed tag loses its conversions");
  }

  {
    aw::options.tagTidy = false;
    expect(aw::tidyTagConversions(sub, inventory, b, exec).empty(),
           "the switch turns the pass off");
    aw::options.tagTidy = true;
  }
}

}  // namespace plan
