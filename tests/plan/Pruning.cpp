// Recipe pruning tests
//
// Recipe-level pruning: recipe rules, direct dominance and substitution.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

// Dominated-input substitution. `X <- T` loses to `X x9 <- w` once every member
// of the tag T is paid for with w: A is made directly from w, and B only through
// C, which is the real-input chain the substitution has to follow upward. T is a
// tag of {A, B}.
//
//   handle 1 X <- r0 (x1, ws [6], T x1)
//              <- r1 (x9, ws [6], w x1)
//   handle 2 A <- r2 (x1, ws [6], w x4)
//   handle 3 B <- r3 (x1, ws [6], C x1)
//   handle 4 C <- r4 (x1, ws [6], w x3)
//   handle 5 w <- r5 (x1, ws [6], u x1)
//   handle 6 WS   (leaf)
//   handle 7 z    (leaf)
//   handle 8 u    (leaf)
//   handle 9 T <- r6 (x1, no ws, A x1)   (synthetic)
//              <- r7 (x1, no ws, B x1)   (synthetic)
//
// Recipe ids in file order: r0=0 .. r7=7. w has a producer on purpose, so the
// composite pass leaves r1 alone and the substitution is the only reason r0 can
// go. When `breakChain` is set, C is made from z instead of w, so B (and hence
// T) does not cost w and r0 must survive.
aw::vector<std::byte> buildSubstitutionSample(bool breakChain = false) {
  AwrWriter w;
  w.header(8, 6);  // real handles 1..8; entries 1, 2, 3, 4, 5, 9
  w.item(1, 2);
  w.recipe(1, {6}, {{9, 1}});
  w.recipe(9, {6}, {{5, 1}});
  w.item(2, 1);
  w.recipe(1, {6}, {{5, 4}});
  w.item(3, 1);
  w.recipe(1, {6}, {{4, 1}});
  w.item(4, 1);
  if (breakChain)
    w.recipe(1, {6}, {{7, 1}});
  else
    w.recipe(1, {6}, {{5, 3}});
  w.item(5, 1);
  w.recipe(1, {6}, {{8, 1}});
  w.item(9, 2);
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{3, 1}});
  return w.out;
}

// The same substitution with a real input: `X <- Y` loses to `X x9 <- w`
// because Y <- w x4.
//
//   handle 1 X <- r0 (x1, ws [4], Y x1)
//              <- r1 (x9, ws [4], w x1)
//   handle 2 Y <- r2 (x1, ws [4], w x4)
//   handle 3 w <- r3 (x1, ws [4], u x1)
//   handle 4 WS   (leaf)
//   handle 5 u    (leaf)
// Recipe ids: r0=0, r1=1, r2=2, r3=3. w has a producer so r1 is not
// composite-dominated and the substitution is the only reason r0 can go.
aw::vector<std::byte> buildSubstitutionRealSample() {
  AwrWriter w;
  w.header(5, 3);  // real handles 1..5; entries 1, 2, 3
  w.item(1, 2);
  w.recipe(1, {4}, {{2, 1}});
  w.recipe(9, {4}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {4}, {{3, 4}});
  w.item(3, 1);
  w.recipe(1, {4}, {{5, 1}});
  return w.out;
}

// The right-only-negative regression from plan section 6. S consumes
// coal_block, which the composite `r + R` never mentions. A one-sided
// comparison would wrongly call `coal <- torch` dominated.
//
//   handle 1 coal       <- r0 (x1, ws [1], torch x4)
//                       <- r1 (x9, ws [1], coal_block x1)
//   handle 2 torch      <- r2 (x4, ws [1], stick x1 + J x1)
//   handle 3 coal_block <- r3 (x1, ws [1], coal x9)
//   handle 6 J = {stick, leaf}, handles 4 and 5 are leaves.
aw::vector<std::byte> buildCoalRegressionSample() {
  AwrWriter w;
  w.header(5, 4);
  w.item(1, 2);
  w.recipe(1, {1}, {{2, 4}});
  w.recipe(9, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(4, {1}, {{4, 1}, {6, 1}});
  w.item(3, 1);
  w.recipe(1, {1}, {{1, 9}});
  w.item(6, 2);
  w.recipe(1, {}, {{4, 1}});
  w.recipe(1, {}, {{5, 1}});
  return w.out;
}

// R is more efficient in Z than S: 5 X from one Z versus 4 X from one Z.
// R must survive even though S has a larger output.
//
//   handle 1 X <- R (x5, ws [1], Y x1)
//              <- S (x4, ws [1], Z x1)
//   handle 2 Y <- r (x1, ws [1], Z x1)
//   handle 3 Z is a leaf.
aw::vector<std::byte> buildAmountRatioSample() {
  AwrWriter w;
  w.header(3, 2);
  w.item(1, 2);
  w.recipe(5, {1}, {{2, 1}});
  w.recipe(4, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {1}, {{3, 1}});
  return w.out;
}

// Y has an independent route to a leaf, so `X <- Y` is not dominated by
// `X <- Z`.
//
//   handle 1 X <- R (x1, ws [1], Y x1)
//              <- S (x1, ws [1], Z x1)
//   handle 2 Y <- r (x1, ws [1], K x1)
//   handles 3 Z and 4 K are leaves.
aw::vector<std::byte> buildIndependentRouteSample() {
  AwrWriter w;
  w.header(4, 2);
  w.item(1, 2);
  w.recipe(1, {1}, {{2, 1}});
  w.recipe(1, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {1}, {{4, 1}});
  return w.out;
}

// R needs 3 Y, but the only Y recipe makes 2. The ceiling forces alpha = 2, so
// `2 Y + R` leaves an extra Y and cannot be bounded by S; a rational alpha of
// 1.5 would have wrongly dominated R.
//
//   handle 1 X <- R (x1, ws [1], Y x3)
//              <- S (x1, ws [1], Z x1)
//   handle 2 Y <- r (x2, ws [1], Z x1)
//   handle 3 Z is a leaf.
aw::vector<std::byte> buildIntegerScalingSample() {
  AwrWriter w;
  w.header(3, 2);
  w.item(1, 2);
  w.recipe(1, {1}, {{2, 3}});
  w.recipe(1, {1}, {{3, 1}});
  w.item(2, 1);
  w.recipe(2, {1}, {{3, 1}});
  return w.out;
}

// The workstation counterexample for composite dominance. R and S both produce
// X, but S needs a workstation R does not; after inlining Y, S still
// cost-dominates R. Dropping R is only sound when S can run on every
// workstation R can, otherwise a player holding only R's station loses the
// only route to X.
//
//   handle 1 X <- R (x1, ws [4=A], Y x1)
//              <- S (x1, ws [5=B] when !sSuperset, [4, 5] when sSuperset, Z x1)
//   handle 2 Y <- r (x1, ws [4=A], Z x1)
//   handle 3 Z <- (x1, ws [4=A], BASE x1)
//   handle 6 BASE is a leaf.
// Recipe ids in file order: R=0, S=1, r=2, Z's recipe=3.
aw::vector<std::byte> buildWorkstationGuardSample(bool sSuperset) {
  AwrWriter w;
  w.header(6, 3);  // entries: handles 1, 2, 3
  w.item(1, 2);    // X: R and S
  w.recipe(1, {4}, {{2, 1}});  // R, WS_A
  // S: WS_B only, or both stations when sSuperset.
  w.recipe(1, sSuperset ? std::initializer_list<std::uint32_t>{4, 5}
                        : std::initializer_list<std::uint32_t>{5},
           {{3, 1}});
  w.item(2, 1);            // Y
  w.recipe(1, {4}, {{3, 1}});
  w.item(3, 1);            // Z
  w.recipe(1, {4}, {{6, 1}});
  return w.out;
}

// Two incomparable dominators for the same recipe. R's route to X goes through
// Y1, and after inlining Y1's producer both S1 (X <- P, WS_B) and S2
// (X <- Y2, WS_C) cost-dominate R, while neither dominates the other. They are
// therefore two sink SCCs, i.e. two kept representatives, and the union of
// their workstations decides whether R can be dropped.
//
//   handle 1 X  <- R  (x1, ws [7=A], Y1 x1 + Y2 x1)
//               <- S1 (x1, ws [8=B], P x1)
//               <- S2 (x1, ws [9=C], Y2 x1)
//   handle 2 Y1 <- r1 (x1, ws [7], P x1)
//   handle 3 Y2 <- r2 (x1, ws [7], Q x1)
//   handle 4 P  <- p  (x1, ws [7], ORE x1)
//   handles 5..9 ORE, Q, WS_A, WS_B, WS_C are leaves.
// Recipe ids in file order: R=0, S1=1, S2=2, r1=3, r2=4, p=5.
aw::vector<std::byte> buildMultiDominatorSample() {
  AwrWriter w;
  w.header(9, 4);
  w.item(1, 3);
  w.recipe(1, {7}, {{2, 1}, {3, 1}});
  w.recipe(1, {8}, {{4, 1}});
  w.recipe(1, {9}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {7}, {{4, 1}});
  w.item(3, 1);
  w.recipe(1, {7}, {{6, 1}});
  w.item(4, 1);
  w.recipe(1, {7}, {{5, 1}});
  return w.out;
}

// Direct (column) dominance: r1 yields the same X as r0 with less of the same
// input, and the composite pass cannot see it because A's only producer makes
// the inlined column of r0 no worse than r1.
//
//   handle 1 X <- r0 (x2, ws [4], A x2)
//               <- r1 (x2, ws [5], A x1)   ; v_r1 = 2X - A >= 2X - 2A = v_r0
//   handle 2 A <- r2 (x1, ws [4], B x1)
//   handle 3 B    (leaf)
//   handles 4, 5 are WS_A and WS_B.
aw::vector<std::byte> buildDirectDominanceSample() {
  AwrWriter w;
  w.header(5, 2);
  w.item(1, 2);
  w.recipe(2, {4}, {{2, 2}});
  w.recipe(2, {5}, {{2, 1}});
  w.item(2, 1);
  w.recipe(1, {4}, {{3, 1}});
  return w.out;
}

}  // namespace

AW_TEST(testRecipePruning) {
  std::cout << "[Test] composite recipe pruning\n";

  // The motivating example: black x224 <- black_candle is a net loss once the
  // black candle's producers are inlined, while black x256 <- black_dye is the
  // route the 224 column loses to.
  aw::registerCraftingGraph(buildBlackCandleSample());
  expect(aw::getCraftingError() == nullptr, "black candle sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 6 && graph.nReal == 6, "black candle sample shape");
    expect(graph.recipeDominated.size() == graph.nRecipe &&
               graph.recipeGuardInput.size() == graph.nRecipe,
           "one recipe pruning flag per recipe");
    expect(graph.recipeDominated[5] == 1,
           "black x224 <- black_candle is composite-dominated");
    expect(graph.recipeGuardInput[5] == 0,
           "the guard of black x224 <- black_candle is black_candle");
    expect(graph.recipeDominated[4] == 0, "black x256 <- black_dye survives");
  }
  {
    // Stocking the guard input keeps the dropped recipe reachable.
    const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[0] = 10;  // black_candle
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    bool kept = false;
    for (aw::ItemId r : sub.recipeOrigin)
      if (r == 5)
        kept = true;
    expect(kept, "stocking the guard input keeps the dominated recipe");
    const aw::ItemId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, 224, inventory);
    expect(r.status == aw::PlanStatus::OK, "a stocked guard still plans");
  }
  {
    // The A/B switch turns the drop off entirely. Satellite elimination and
    // seed pruning are disabled too: they would drop the same dead island on
    // their own.
    const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
    aw::options.recipePruning = false;
    aw::options.satellite.enabled = false;
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all);
    aw::options.seedPruning = true;
    aw::options.satellite.enabled = true;
    aw::options.recipePruning = true;
    bool kept = false;
    for (aw::ItemId r : sub.recipeOrigin)
      if (r == 5)
        kept = true;
    expect(kept, "disabling recipe pruning keeps every recipe");
  }

  // The right-only negative entry regression from plan section 6: S consumes
  // coal_block, which the composite never mentions, so `coal <- torch` must
  // not be dominated.
  aw::registerCraftingGraph(buildCoalRegressionSample());
  expect(aw::getCraftingError() == nullptr, "coal regression sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.recipeDominated[0] == 0,
           "a right-only negative entry refutes domination");
    expect(graph.recipeDominated[1] == 1,
           "the self-cancelling cycle is dominated");
  }

  // R is more efficient in Z than S, so it must survive.
  aw::registerCraftingGraph(buildAmountRatioSample());
  expect(aw::getCraftingError() == nullptr, "amount ratio sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "the more Z-efficient recipe is not dominated");

  // Y has a leaf-rooted route of its own, so `X <- Y` survives.
  aw::registerCraftingGraph(buildIndependentRouteSample());
  expect(aw::getCraftingError() == nullptr, "independent route sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "an independent route to Y blocks domination");

  // The producer's output amount does not divide q, so the ceiling decides.
  aw::registerCraftingGraph(buildIntegerScalingSample());
  expect(aw::getCraftingError() == nullptr, "integer scaling sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "the ceiling, not a rational alpha, decides domination");

  // S dominates R after inlining Y, but S runs only on WS_B while R runs on
  // WS_A. The cost relation is still recorded; availability is deferred to the
  // query, where a player holding only WS_A must keep R.
  aw::registerCraftingGraph(buildWorkstationGuardSample(false));
  expect(aw::getCraftingError() == nullptr, "workstation guard sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 1,
         "a disjoint workstation set no longer blocks composite domination");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    bool hasB = false;
    for (aw::ItemId station : graph.recipeDominatorWorkstations[0])
      if (station == 4)  // handle 5 = WS_B, the only station of S
        hasB = true;
    expect(hasB, "the dominator workstations record S's station");
  }

  // When S can run on everything R can, the drop is still sound.
  aw::registerCraftingGraph(buildWorkstationGuardSample(true));
  expect(aw::getCraftingError() == nullptr, "workstation superset sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 1,
         "a superset workstation set keeps composite domination");
}

AW_TEST(testRecipePruningWorkstations) {
  std::cout << "[Test] composite pruning respects workstations\n";
  aw::registerCraftingGraph(buildWorkstationGuardSample(false));
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle onlyA[] = {4};
  const aw::Handle both[] = {4, 5};

  // BASE (handle 6 -> item node 5) is the only leaf; the player holds it.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[5] = 100;

  // R (recipe 0) is dominated by S, which runs on WS_B only. A player who does
  // not own WS_B keeps R; one who does can drop it, which the old
  // workstation-subset gate never allowed.
  auto keepsR = [&](std::span<const aw::Handle> ws) {
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    for (aw::ItemId r : sub.recipeOrigin)
      if (r == 0)
        return true;
    return false;
  };
  expect(keepsR(onlyA), "a player without the dominator's station keeps R");
  expect(!keepsR(both), "a player with the dominator's station drops R");

  auto plan = [&](bool prune, std::span<const aw::Handle> ws) {
    aw::options.recipePruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const aw::vector<aw::Handle> cases[] = {{4}, {4, 5}};
  for (const aw::vector<aw::Handle> &ws : cases) {
    const auto full = plan(false, ws);
    const auto pruned = plan(true, ws);
    expect(full.first == aw::PlanStatus::OK, "the unpruned route plans");
    expect(pruned.first == full.first, "workstation guard preserves the plan status");
    expect(pruned.second == full.second, "workstation guard preserves the optimum");
  }
  aw::options.recipePruning = true;
}

AW_TEST(testRecipeDominatorWorkstations) {
  std::cout << "[Test] composite pruning unions dominator workstations\n";
  aw::registerCraftingGraph(buildMultiDominatorSample());
  expect(aw::getCraftingError() == nullptr, "multi-dominator sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  expect(graph.recipeDominated.size() == graph.nRecipe,
         "the dominated flags are sized");
  expect(graph.recipeDominated[0] == 1, "R is dominated by S1 and S2");
  {
    // WS_B is handle 8 -> node 7 and WS_C is handle 9 -> node 8. Both sink
    // representatives contribute, so either station alone can drop R.
    const aw::vector<aw::ItemId> want = {7, 8};
    expect(graph.recipeDominatorWorkstations[0] == want,
           "both dominators contribute their stations");
  }

  // Leaves are free only through inventory.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::ItemId m = 0; m < graph.nReal; m++)
    if (graph.producersOf(m).empty())
      inventory[m] = 1000000000LL;

  const aw::Handle a[] = {7};
  const aw::Handle ab[] = {7, 8};
  const aw::Handle ac[] = {7, 9};

  auto keepsR = [&](std::span<const aw::Handle> ws) {
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    for (aw::ItemId r : sub.recipeOrigin)
      if (r == 0)
        return true;
    return false;
  };
  expect(keepsR(a), "without a dominator station R survives");
  expect(!keepsR(ab), "WS_B alone lets S1 replace R");
  expect(!keepsR(ac), "WS_C alone lets S2 replace R");

  auto plan = [&](bool prune, std::span<const aw::Handle> ws) {
    aw::options.recipePruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const aw::vector<aw::Handle> cases[] = {{7}, {7, 8}, {7, 9}, {7, 8, 9}};
  for (const aw::vector<aw::Handle> &ws : cases) {
    const auto full = plan(false, ws);
    const auto pruned = plan(true, ws);
    expect(full.first == aw::PlanStatus::OK, "the unpruned route plans");
    expect(pruned.first == full.first, "dominator stations preserve the plan status");
    expect(pruned.second == full.second, "dominator stations preserve the optimum");
  }
  aw::options.recipePruning = true;
}

AW_TEST(testRecipePruningParity) {
  std::cout << "[Test] composite pruning preserves the optimum\n";
  aw::registerCraftingGraph(buildBlackCandleSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};

  // Leaves (handles 5 and 6) are free only through inventory.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::ItemId m = 0; m < graph.nReal; m++)
    if (graph.producersOf(m).empty())
      inventory[m] = 1000000000LL;

  auto plan = [&](bool prune, aw::Amount amount) {
    aw::options.recipePruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::ItemId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const aw::Amount amounts[] = {1, 224, 256, 300};
  for (aw::Amount amount : amounts) {
    const auto full = plan(false, amount);
    const auto pruned = plan(true, amount);
    expect(full.first == pruned.first,
           "composite pruning preserves the plan status");
    expect(full.second == pruned.second,
           "composite pruning preserves the optimum");
  }
  aw::options.recipePruning = true;
}

AW_TEST(testDirectDominancePruning) {
  std::cout << "[Test] direct column dominance pruning\n";
  aw::registerCraftingGraph(buildDirectDominanceSample());
  expect(aw::getCraftingError() == nullptr, "direct dominance sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 3 && graph.nReal == 5, "direct dominance sample shape");
    expect(graph.recipeDirectDominated.size() == graph.nRecipe &&
               graph.recipeDirectDominatorWorkstations.size() == graph.nRecipe,
           "one direct pruning flag per recipe");
    expect(graph.recipeDirectDominated[0] == 1,
           "the column with more input is directly dominated");
    expect(graph.recipeDirectDominated[1] == 0, "the maximal column survives");
    expect(graph.recipeDominated[0] == 0 && graph.recipeDominated[1] == 0,
           "the composite pass cannot see this pair");
    const aw::vector<aw::ItemId> want = {4};  // node 4 = handle 5 = WS_B
    expect(graph.recipeDirectDominatorWorkstations[0] == want,
           "the dominator's workstation is recorded");
  }

  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  // B (handle 3 -> node 2) is a leaf, so the inventory must hold it.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[2] = 100;

  const aw::Handle onlyA[] = {4};    // WS_A
  const aw::Handle both[] = {4, 5};  // WS_A and WS_B

  expect(!subgraphHasRecipe(aw::reachableSubgraph(1, both, inventory), 0),
         "with the dominator's station the dominated recipe is dropped");
  expect(subgraphHasRecipe(aw::reachableSubgraph(1, onlyA, inventory), 0),
         "without the dominator's station the dominated recipe survives");
  {
    // Nothing is inlined, so holding the shared input never revives it.
    aw::vector<aw::Amount> stocked = inventory;
    stocked[1] = 100;  // A
    expect(!subgraphHasRecipe(aw::reachableSubgraph(1, both, stocked), 0),
           "stock of the shared input does not keep the dominated recipe");
  }

  auto plan = [&](bool prune, std::span<const aw::Handle> ws, aw::Amount amount) {
    aw::options.directPruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const aw::vector<aw::Handle> cases[] = {{4}, {4, 5}};
  for (const aw::vector<aw::Handle> &ws : cases) {
    for (aw::Amount amount : {2, 3, 8}) {
      const auto full = plan(false, ws, amount);
      const auto pruned = plan(true, ws, amount);
      expect(full.first == pruned.first, "direct dominance preserves the status");
      expect(full.second == pruned.second, "direct dominance preserves the optimum");
    }
  }
  aw::options.directPruning = true;
}

AW_TEST(testSubstitutionPruning) {
  std::cout << "[Test] dominated-input substitution pruning\n";

  aw::registerCraftingGraph(buildSubstitutionSample());
  expect(aw::getCraftingError() == nullptr, "substitution sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 8 && graph.nReal == 8, "substitution sample shape");
    expect(graph.recipeSubstituted.size() == graph.nRecipe &&
               graph.recipeSubstitutedGuards.size() == graph.nRecipe &&
               graph.recipeSubstitutedDominatorWorkstations.size() == graph.nRecipe,
           "one substitution flag per recipe");
    expect(graph.recipeSubstituted[0] == 1, "X <- T is substituted");
    expect(graph.recipeSubstituted[1] == 0, "the dominator survives");
    const aw::vector<aw::ItemId> guards = {1, 2};  // A, B
    expect(graph.recipeSubstitutedGuards[0] == guards,
           "the tag expands into its members for the stock guard");
    const aw::vector<aw::ItemId> ws = {5};  // WS
    expect(graph.recipeSubstitutedDominatorWorkstations[0] == ws,
           "the dominator's workstation is recorded");
  }

  // Stocking a tag member keeps the recipe: the free member can still be spent
  // on the tag instead of paying for it with w.
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[4] = 1000000000LL;  // w, so the sample is feasible
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6, 7, 8};
  expect(!subgraphHasRecipe(aw::reachableSubgraph(1, all, inventory), 0),
         "with no member stock the substituted recipe is dropped");
  {
    aw::vector<aw::Amount> stocked = inventory;
    stocked[1] = 1;  // A (handle 2)
    expect(subgraphHasRecipe(aw::reachableSubgraph(1, all, stocked), 0),
           "stocking a tag member keeps the substituted recipe");
  }

  // A chain that does not cost w leaves the recipe alone.
  aw::registerCraftingGraph(buildSubstitutionSample(true));
  expect(aw::getCraftingError() == nullptr, "broken chain sample parses");
  expect(aw::getCraftingGraph().recipeSubstituted[0] == 0,
         "a chain that bypasses w blocks the substitution");

  // A real (non-tag) input collapses the same way.
  aw::registerCraftingGraph(buildSubstitutionRealSample());
  expect(aw::getCraftingError() == nullptr, "real substitution sample parses");
  expect(aw::getCraftingGraph().recipeSubstituted[0] == 1,
         "a real input that costs w is substituted");
  expect(aw::getCraftingGraph().recipeSubstitutedGuards[0] ==
             aw::vector<aw::ItemId>{1},
         "the real input is its own guard");

  // Substitution preserves the optimum: leaves are free only through stock.
  aw::registerCraftingGraph(buildSubstitutionSample());
  const aw::CraftingGraph &g = aw::getCraftingGraph();
  aw::vector<aw::Amount> inv(g.nItem, 0);
  for (aw::ItemId m = 0; m < g.nReal; m++)
    if (g.producersOf(m).empty())
      inv[m] = 1000000000LL;
  auto plan = [&](bool prune, aw::Amount amount) {
    aw::options.substitutionPruning = prune;
    const aw::Handle all[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inv);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult r = aw::planCrafting(sub, target, amount, inv);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };
  for (aw::Amount amount : {1, 3, 9}) {
    const auto full = plan(false, amount);
    const auto pruned = plan(true, amount);
    expect(full.first == pruned.first, "substitution preserves the status");
    expect(full.second == pruned.second, "substitution preserves the optimum");
  }
  aw::options.substitutionPruning = true;
}

AW_TEST(testReducedCostParity) {
  std::cout << "[Test] reduced-cost fixing preserves the plan optimum\n";
  aw::registerCraftingGraph(buildBlackCandleSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};

  // Leaves are free only through inventory.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::ItemId m = 0; m < graph.nReal; m++)
    if (graph.producersOf(m).empty())
      inventory[m] = 1000000000LL;

  auto plan = [&](double gap, aw::Amount amount) {
    aw::solver::Options options;
    options.reducedCostGap = gap;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::ItemId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory, options);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const aw::Amount amounts[] = {1, 224, 256, 300};
  for (aw::Amount amount : amounts) {
    const auto off = plan(0.0, amount);
    const auto on = plan(10.0, amount);
    expect(off.first == on.first,
           "reduced-cost fixing preserves the plan status");
    expect(off.second == on.second,
           "reduced-cost fixing preserves the plan optimum");
  }
}

}  // namespace plan
