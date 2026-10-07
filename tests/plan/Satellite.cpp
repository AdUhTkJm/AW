// Whole-recipe pruning tests
//
// Whole-recipe pruning: wasteful packs, satellites, closed islands and variants.

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

// The copper-pickaxe example from docs/algorithm.typ. The pack
//
//   r0: ingot  x1 <- nugget  x9
//   r1: pickaxe x1 <- ingot x3 + stick x2
//   r2: nugget x1 <- pickaxe x1
//
// produces and consumes nothing net but costs 19 steps, so `r0` is never used
// by an optimal plan once nugget and pickaxe are out of stock.
//
//   handle 1 ingot   <- r0 (x1, ws [1], nugget x9)
//   handle 2 stick     (leaf)
//   handle 3 pickaxe <- r1 (x1, ws [1], ingot x3 + stick x2)
//   handle 4 nugget  <- r2 (x1, ws [1], pickaxe x1)
aw::vector<std::byte> buildPackSample() {
  AwrWriter w;
  w.header(4, 3);
  w.item(1, 1);
  w.recipe(1, {1}, {{4, 9}});
  w.item(3, 1);
  w.recipe(1, {1}, {{1, 3}, {2, 2}});
  w.item(4, 1);
  w.recipe(1, {1}, {{3, 1}});
  return w.out;
}

// A split-production graph that exercises R3. A has two producers that each
// make 2 units from a different leaf, and B loops back to the target X, so an
// unsound full-production branch could keep only one of them.
//
//   handle 1 X <- r0 (x1, ws [1], A x3)
//   handle 2 A <- r1 (x2, ws [1], B x1)
//              <- r2 (x2, ws [1], C x1)
//   handle 3 B <- r3 (x1, ws [1], X x1)
//   handle 4 C   (leaf)

aw::vector<std::byte> buildPackBranchSample() {
  AwrWriter w;
  w.header(4, 3);
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 3}});
  w.item(2, 2);
  w.recipe(2, {1}, {{3, 1}});
  w.recipe(2, {1}, {{4, 1}});
  w.item(3, 1);
  w.recipe(1, {1}, {{1, 1}});
  return w.out;
}

// A hub with a variant branch hanging off it, which is the shape satellite
// elimination is aimed at. Handle 5 is a bare workbench.
//
//   handle 1 gear    <- r0 (x1, ws [5], plate x1)      (the target)
//   handle 2 plate   <- r1 (x1, ws [5], ore x1)        the cut vertex A
//                    <- r3 (x1, ws [5], variant x1)    the recycling edge
//   handle 3 variant <- r2 (x{variantOut}, ws [5], plate x1)
//   handle 4 ore        (leaf)
//
// With variantOut == 1 the branch is break-even (1 plate -> 1 variant ->
// 1 plate) and can never repay the plate, so it is dead. With variantOut == 2
// it is gainful and must be kept.

aw::vector<std::byte> buildSatelliteSample(uint64_t variantOut) {
  AwrWriter w;
  w.header(5, 3);
  w.item(1, 1);
  w.recipe(1, {5}, {{2, 1}});
  w.item(2, 2);
  w.recipe(1, {5}, {{4, 1}});
  w.recipe(1, {5}, {{3, 1}});
  w.item(3, 1);
  w.recipe(variantOut, {5}, {{2, 1}});
  return w.out;
}

// A generalized satellite: the island shares an input with the rest of the
// graph, so it is not an undirected component at all. Handle 9 is a bare
// workbench.
//
//   handle 1 T  <- r0 (x1, ws [9], S x1 + Y x1)      (the target)
//   handle 2 S  <- r1 (x1, ws [9], CB x1)
//   handle 3 Y  <- r2 (x1, ws [9], X x1)
//   handle 4 X     (leaf, shared input)
//   handle 5 CB <- r3 (x1, ws [9], E x1)             the cut / escape
//   handle 6 E  <- r4 (x1, ws [9], CB x1 + X x1)
//              <- r5 (x1, ws [9], W x1)
//   handle 7 W  <- r6 (x1, ws [9], E x1 + X x1)
//              <- r7 (x1, ws [9], O x1)
//   handle 8 O  <- r8 (x1, ws [9], W x1 + X x1)
//
// The E/W/O cycle can never net-produce CB, but X is consumed both by it and
// by the Y -> T chain, so removing CB leaves E/W/O connected to the target in
// the undirected graph. The escape enumeration still finds it.

aw::vector<std::byte> buildSatelliteLeakSample() {
  AwrWriter w;
  w.header(9, 7);
  w.item(1, 1);
  w.recipe(1, {9}, {{2, 1}, {3, 1}});
  w.item(2, 1);
  w.recipe(1, {9}, {{5, 1}});
  w.item(3, 1);
  w.recipe(1, {9}, {{4, 1}});
  w.item(5, 1);
  w.recipe(1, {9}, {{6, 1}});
  w.item(6, 2);
  w.recipe(1, {9}, {{4, 1}, {5, 1}});
  w.recipe(1, {9}, {{7, 1}});
  w.item(7, 2);
  w.recipe(1, {9}, {{4, 1}, {6, 1}});
  w.recipe(1, {9}, {{8, 1}});
  w.item(8, 1);
  w.recipe(1, {9}, {{4, 1}, {7, 1}});
  return w.out;
}

// A closed island with no escape at all, which the escape enumeration cannot
// describe. The only recipe linking the island to the needed items is a
// dominated recycling edge, so the query-time re-pruning is what removes it and
// leaves the island dangling. Handle 5 is a bare workbench.
//
//   handle 1 T <- r0 (x1, ws [5], A x1)      (the target)
//   handle 2 A <- r1 (x1, ws [5], L x1)
//              <- r2 (x1, ws [5], Y x10)      the recycling edge
//   handle 3 Y <- r3 (x1, ws [5], L x10)     the island
//   handle 4 L     (leaf)
//
// Inlining r3 into r2 commits the ten Y it makes, spends 10 L on one A while
// r1 spends one, so reprune drops r2. What is left of Y is r3 alone, which only
// makes Y, so {Y} is output-closed and dead. Y shares L with the target chain,
// so the undirected articulation search cannot see it either.

aw::vector<std::byte> buildClosedIslandSample() {
  AwrWriter w;
  w.header(5, 3);
  w.item(1, 1);
  w.recipe(1, {5}, {{2, 1}});
  w.item(2, 2);
  w.recipe(1, {5}, {{4, 1}});
  w.recipe(1, {5}, {{3, 10}});
  w.item(3, 1);
  w.recipe(1, {5}, {{4, 10}});
  return w.out;
}

// A colour orbit behind a generic tag, the shape variant-class elimination is
// aimed at. Handles 1..7 are real, 8..10 are tags. Handle 1 is the target.
//
//   target   <- r0 (x1, ws [1], TE x1)           the exit
//            <- r0 (x1, ws [1], A + TE x1)       when the named consumer is on
//   A        <- r1 (x1, ws [1], resourceA x1)    the direct producers
//            <- r2 (x1, ws [1], dyeA + P_A x1)   the dyes
//   B        <- r3 (x1, ws [1], resourceB x1)
//            <- r4 (x1, ws [1], dyeB + P_B x1)
//   TE = {A, B}  <- r5, r6 (tag edges)
//   P_A = {B}    <- r7 (tag edge)
//   P_B = {A}    <- r8 (tag edge)
//
// TE accepts both members, so no plan needs to dye one colour into the other:
// the plan already directly produces as many members as it spends through TE.
// r2, r4 and the member edges of P_A/P_B (r7, r8) are dropped. With the named
// consumer on, the exit wants A itself, so A leaves the orbit and r2 stays.

}  // namespace

AW_TEST(testPackPruning) {
  std::cout << "[Test] wasteful-pack certificates\n";
  aw::registerCraftingGraph(buildPackSample());
  expect(aw::getCraftingError() == nullptr, "pack sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nRecipe == 3 && graph.nReal == 4, "pack sample shape");
    expect(graph.packDominated.size() == graph.nRecipe &&
               graph.packCertificates.size() == graph.nRecipe,
           "one pack certificate slot per recipe");
    expect(graph.packDominated[0] == 1, "ingot <- nugget is certified");
    expect(graph.packDominated[1] == 1 && graph.packDominated[2] == 1,
           "the whole cycle carries certificates");
    const aw::PackCertificate &cert = graph.packCertificates[0];
    const uint32_t support[] = {0, 1, 2};
    const int64_t counts[] = {1, 9, 9};
    expect(cert.support.size() == 3, "certificate support size");
    for (int i = 0; i < 3 && cert.support.size() == 3; i++) {
      expect(cert.support[i] == support[i], "certificate support is the pack");
      expect(cert.count[i] == counts[i], "certificate counts are the pack");
    }
    expect(cert.zeroStock.size() == 2 && cert.zeroStock[0] == 2 &&
               cert.zeroStock[1] == 3,
           "certificate zero-stock set is pickaxe and nugget");
  }

  const aw::Handle all[] = {1, 2, 3, 4};
  {
    // With nothing in stock, every route is certified and no recipe survives:
    // the pickaxe is genuinely infeasible without ingot or nugget stock.
    const aw::Subgraph sub = aw::reachableSubgraph(3, all);
    expect(!subgraphHasRecipe(sub, 0), "the certified recipe is dropped");
    expect(sub.graph.nRecipe == 0, "the whole certified cycle is dropped");
  }
  {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[3] = 10;  // nugget
    // Satellite elimination and seed pruning are off here: stick is still an
    // unstocked leaf, so both would drop the pickaxe recipe no matter what the
    // pack pass does. This block is about the pack certificate's zero-stock
    // gate, so isolate it.
    aw::options.satellite.enabled = false;
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(3, all, inventory);
    aw::options.seedPruning = true;
    aw::options.satellite.enabled = true;
    expect(subgraphHasRecipe(sub, 0), "stocked nugget keeps the ingot recipe");
  }

  aw::options.pack.enabled = false;
  // Satellite elimination also drops this cycle once the pack is off, so it is
  // switched off as well to isolate the pack A/B switch. The seed filter would
  // drop the unseeded cycle on its own, so it is off too.
  aw::options.satellite.enabled = false;
  aw::options.seedPruning = false;
  aw::registerCraftingGraph(buildPackSample());
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.packDominated[0] == 0, "disabling the pass drops no recipe");
    const aw::Subgraph sub = aw::reachableSubgraph(3, all);
    expect(subgraphHasRecipe(sub, 0), "the recipe survives when disabled");
  }
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.pack.enabled = true;
}

AW_TEST(testPackPruningParity) {
  std::cout << "[Test] pack pruning preserves the optimum\n";

  auto total = [](const aw::PlanResult &r) {
    int64_t sum = 0;
    for (int64_t x : r.exec)
      sum += x;
    return sum;
  };

  // Copper: ingot and stick are leaves, so stock them.
  {
    const aw::vector<std::byte> bytes = buildPackSample();
    auto plan = [&](bool prune, aw::Amount amount) {
      aw::options.pack.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4};
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      inventory[0] = 1000000;  // ingot
      inventory[1] = 1000000;  // stick
      const aw::Subgraph sub = aw::reachableSubgraph(3, all, inventory);
      const aw::ItemId target = sub.translate(2);
      const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 3, 10}) {
      const auto full = plan(false, amount);
      const auto pruned = plan(true, amount);
      expect(full.first == pruned.first, "copper: status preserved");
      expect(full.second == pruned.second, "copper: optimum preserved");
    }
  }

  // Split production: B and C are leaves. No certificate exists, so parity is
  // the soundness check for the R3 branch.
  {
    const aw::vector<std::byte> bytes = buildPackBranchSample();
    aw::options.pack.enabled = true;
    aw::registerCraftingGraph(bytes);
    expect(aw::getCraftingGraph().packDominated[0] == 0,
           "a split-production branch yields no certificate");

    auto plan = [&](bool prune, aw::Amount amount) {
      aw::options.pack.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4};
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      inventory[2] = 1000000;  // B
      inventory[3] = 1000000;  // C
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::ItemId target = sub.translate(0);
      const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 3, 7}) {
      const auto full = plan(false, amount);
      const auto pruned = plan(true, amount);
      expect(full.first == pruned.first, "branch: status preserved");
      expect(full.second == pruned.second, "branch: optimum preserved");
    }
  }

  // Black candle: the composite-pruning sample also carries pack certificates.
  {
    const aw::vector<std::byte> bytes = buildBlackCandleSample();
    auto plan = [&](bool prune, aw::Amount amount) {
      aw::options.pack.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      for (aw::ItemId m = 0; m < graph.nReal; m++)
        if (graph.producersOf(m).empty())
          inventory[m] = 1000000000LL;
      const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
      const aw::ItemId target = sub.translate(3);
      const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 224, 300}) {
      const auto full = plan(false, amount);
      const auto pruned = plan(true, amount);
      expect(full.first == pruned.first, "black candle: status preserved");
      expect(full.second == pruned.second, "black candle: optimum preserved");
    }
  }

  aw::options.pack.enabled = true;
}

AW_TEST(testSatellitePruning) {
  std::cout << "[Test] satellite elimination\n";
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  // The other passes can remove the same island on these tiny graphs (a
  // break-even 2-cycle is exactly what the composite pass and the pack
  // certificates look for), so they are switched off here: this test is about
  // what *this* pass does on its own.
  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.pack.enabled = false;

  auto stockedOre = []() {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[3] = 1000;  // ore is handle 4, node 3
    return inventory;
  };

  // Break-even recycling: 1 plate -> 1 variant -> 1 plate. The island is closed
  // (the variant is made inside it) and can never repay the plate, so it goes.
  // The ore route stays only because the ore is stocked: with an empty stock it
  // is a dead component too.
  aw::registerCraftingGraph(buildSatelliteSample(1));
  {
    const aw::vector<aw::Amount> inventory = stockedOre();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(!subgraphHasRecipe(sub, 2), "the break-even island is dropped");
    expect(!subgraphHasRecipe(sub, 3), "its recycling edge goes with it");
    expect(subgraphHasRecipe(sub, 0) && subgraphHasRecipe(sub, 1),
           "the ore route survives because ore is stocked");
  }
  {
    // A stocked variant is a way to spend stock, so nothing may be dropped.
    aw::vector<aw::Amount> inventory = stockedOre();
    inventory[2] = 10;  // variant is handle 3, node 2
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 2) && subgraphHasRecipe(sub, 3),
           "a stocked island is kept");
  }
  {
    // With empty stock the raw-material route is dead too: nothing can run it,
    // so the certificate accepts the whole component hanging off the target
    // gear and every recipe goes. This only hides the missing ore; the plan is
    // infeasible either way.
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(sub.graph.nRecipe == 0,
           "an unstocked raw-material route is dropped too");
    expect(sub.graph.nItem == 1, "only the target survives");
  }

  // Gainful recycling: 1 plate -> 2 variants -> 2 plates. The island repays the
  // plate, so it must survive the pass.
  aw::registerCraftingGraph(buildSatelliteSample(2));
  {
    const aw::vector<aw::Amount> inventory = stockedOre();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 2) && subgraphHasRecipe(sub, 3),
           "a gainful island is kept");
  }

  // Disabling the pass.
  aw::options.satellite.enabled = false;
  aw::registerCraftingGraph(buildSatelliteSample(1));
  {
    const aw::vector<aw::Amount> inventory = stockedOre();
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 2) && subgraphHasRecipe(sub, 3),
           "the pass can be switched off");
  }
  aw::options.satellite.enabled = true;
  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.pack.enabled = true;
}

// The generalized escape enumeration drops an island that an undirected
// articulation component cannot see, because the island shares an input with
// the rest of the graph.

AW_TEST(testSatelliteLeakPruning) {
  std::cout << "[Test] generalized satellite elimination\n";
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.pack.enabled = false;

  auto keepsIsland = [&](const aw::Subgraph &sub) {
    for (uint32_t r : {3u, 4u, 5u, 6u, 7u, 8u})
      if (!subgraphHasRecipe(sub, r))
        return false;
    return true;
  };

  aw::registerCraftingGraph(buildSatelliteLeakSample());
  {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(!keepsIsland(sub), "an input-sharing island is dropped");
  }
  {
    // With the shared input and an island item stocked, the island can be
    // entered and the escape paid back from stock, so r3/r4 stay; the W/O
    // sub-cycle can still never repay E, so it goes.
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[3] = 10;  // handle 4, item node 3 (X)
    inventory[5] = 10;  // handle 6, item node 5 (E)
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(subgraphHasRecipe(sub, 3) && subgraphHasRecipe(sub, 4),
           "a stocked escape keeps the reachable part of the island");
    for (uint32_t r : {5u, 6u, 7u, 8u})
      expect(!subgraphHasRecipe(sub, r), "the unpayable sub-cycle is still dropped");
  }

  aw::options.satellite.enabled = false;
  aw::options.seedPruning = false;
  aw::registerCraftingGraph(buildSatelliteLeakSample());
  {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(keepsIsland(sub), "the generalized pass can be switched off");
  }
  aw::options.seedPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.pack.enabled = true;
}

// An island that leaks nothing at all has no escape to enumerate. It is dead
// with the empty certificate, and the closure over the post-reprune graph finds
// it in one linear scan.

AW_TEST(testClosedIslandPruning) {
  std::cout << "[Test] output-closed satellite elimination\n";
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.pack.enabled = false;
  aw::options.satellite.enabled = true;
  // The suite disables re-pruning globally; this case needs it, because the
  // dominant recycling edge is what reprune removes to close the island.
  aw::options.reprune.enabled = true;

  aw::registerCraftingGraph(buildClosedIslandSample());
  expect(aw::getCraftingError() == nullptr, "closed island sample parses");
  {
    // L is stocked, so the plan stays feasible and the island is not hidden by
    // an unstocked raw material.
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[3] = 1000;  // handle 4, item node 3 (L)
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(!subgraphHasRecipe(sub, 2), "reprune cuts the dominated recycling edge");
    expect(!subgraphHasRecipe(sub, 3), "the output-closed island is dropped");
    expect(sub.graph.nRecipe == 2, "only the target route survives");
  }
  // The island is the only other route and is strictly worse, so the optimum
  // must not move.
  {
    const aw::vector<std::byte> bytes = buildClosedIslandSample();
    auto plan = [&](bool prune) {
      aw::options.satellite.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      inventory[3] = 1000;
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), 1, inventory);
      int64_t total = 0;
      for (int64_t x : r.exec)
        total += x;
      return std::pair<aw::PlanStatus, int64_t>(r.status, total);
    };
    const auto full = plan(false);
    const auto pruned = plan(true);
    expect(full.first == pruned.first, "closed: status agrees with and without the pass");
    expect(full.second == pruned.second, "closed: optimum agrees with and without the pass");
  }

  aw::options.satellite.enabled = true;
  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.substitutionPruning = true;
  aw::options.pack.enabled = true;
  aw::options.reprune.enabled = false;
}

// A tag that every external consumer accepts makes its members interchangeable,
// so dyeing one member into another is never needed. The other passes do not
// see this: the class is not a satellite (it leaks its useful item) and no
// recipe is componentwise dominated.

AW_TEST(testSatellitePruningParity) {
  std::cout << "[Test] satellite elimination preserves the optimum\n";

  auto total = [](const aw::PlanResult& r) {
    int64_t sum = 0;
    for (int64_t x : r.exec)
      sum += x;
    return sum;
  };

  for (uint64_t variantOut : {1ULL, 2ULL, 3ULL}) {
    const aw::vector<std::byte> bytes = buildSatelliteSample(variantOut);
    auto plan = [&](bool prune, aw::Amount amount, aw::Amount oreStock,
                   aw::Amount variantStock) {
      aw::options.satellite.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4, 5};
      const aw::CraftingGraph& graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      inventory[3] = oreStock;
      inventory[2] = variantStock;
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::ItemId target = sub.translate(0);
      const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 4, 9}) {
      // An empty ore stock makes the raw-material route dead too; dropping it
      // hides the missing input but must keep the plan infeasible.
      for (aw::Amount ore : {0, 1000000}) {
        for (aw::Amount stock : {0, 5}) {
          const auto full = plan(false, amount, ore, stock);
          const auto pruned = plan(true, amount, ore, stock);
          expect(full.first == pruned.first, "status agrees with and without the pass");
          expect(full.second == pruned.second, "optimum agrees with and without the pass");
        }
      }
    }
  }
  // The input-sharing island: E/W/O can never net-produce the escape, so
  // dropping it must not move the optimum. Stocking E makes the island usable
  // and must keep it.
  {
    const aw::vector<std::byte> bytes = buildSatelliteLeakSample();
    auto plan = [&](bool prune, aw::Amount amount, aw::Amount eStock) {
      aw::options.satellite.enabled = prune;
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
      const aw::CraftingGraph& graph = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(graph.nItem, 0);
      inventory[3] = 10;      // handle 4, item node 3 (X, the shared input)
      inventory[5] = eStock;  // handle 6, item node 5 (E)
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::ItemId target = sub.translate(0);
      const aw::PlanResult r = aw::planCrafting(sub, target, amount, inventory);
      return std::pair<aw::PlanStatus, int64_t>(r.status, total(r));
    };
    for (aw::Amount amount : {1, 4}) {
      for (aw::Amount stock : {0, 5}) {
        const auto full = plan(false, amount, stock);
        const auto pruned = plan(true, amount, stock);
        expect(full.first == pruned.first, "leak: status agrees with and without the pass");
        expect(full.second == pruned.second,
               "leak: optimum agrees with and without the pass");
      }
    }
  }
  aw::options.satellite.enabled = true;
}

}  // namespace plan
