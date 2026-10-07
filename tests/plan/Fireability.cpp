// Fireability tests
//
// Fireability of a plan and the start-up cuts derived from it.

#include "plan/Harness.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Solver.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// A two-item cycle whose entry recipe eats two units of the shared seed while
// only one is in stock. The balance admits r0:2 / r1:1, but neither recipe is
// enabled, so the plan is unrealizable. A check that only asks whether the SCC
// touches stock would accept it; the quantities are what matter.
//
//   handle 1 (S) <- r0 (x3, ws [1], input C x1)
//   handle 2 (C) <- r1 (x1, ws [1], input S x2)
aw::vector<std::byte> buildSeedQuantitySample() {
  AwrWriter w;
  w.header(2, 2);
  w.item(1, 1);
  w.recipe(3, {1}, {{2, 1}});
  w.item(2, 1);
  w.recipe(1, {1}, {{1, 2}});
  return w.out;
}

// A two-recipe amplifier pair with a large batch, which is what makes the seed
// a *quantity* rather than a token:
//
//   handle 1 (A)  <- r0 (x64, ws [1], input B x16)
//   handle 2 (B)  <- r1 (x1,  ws [1], input A x1)
//
// The pair is net positive in A (+63 per round), so the net balance accepts
// r0:2 / r1:32 for 64 A, but the first firing of either needs 16 B or 1 A and
// neither exists. The entry cut has to weigh sixteen units, which is what one
// execution of the amplifier eats, not one token per cycle firing.
aw::vector<std::byte> buildAmplifierPairSample() {
  AwrWriter w;
  w.header(2, 2);
  w.item(1, 1);
  w.recipe(64, {1}, {{2, 16}});
  w.item(2, 1);
  w.recipe(1, {1}, {{1, 1}});
  return w.out;
}

// A self-consuming amplifier with a separate seed route, and the stock to pay
// for it. `A x3 <- A x1, L2 x1` is net positive, so the balance accepts two
// amplifications and no seed for four A, which is the cheapest plan and cannot
// fire. The startup barrier
//
//   c(r,i) * [x_r >= 1] <= stock_i + sum_{k != r, net > 0} net_k * x_k
//
// turns that into `[x_r >= 1] <= [seed used]`, so the plan is gone before the
// solver returns it. The no-good retry finds the seeded runner-up too, just
// more slowly.
//
//   handle 1 (L)  leaf, 10 in stock
//   handle 2 (A)  <- r0 (x3, ws [1], input A x1, L2 x1)  amplifier
//                 <- r1 (x1, ws [1], input L x1)         seed
//   handle 3 (L2) leaf, 1000 in stock
//   handle 4 (T)  <- r2 (x1, ws [1], input A x4)         target
aw::vector<std::byte> buildStartupBarrierSample() {
  AwrWriter w;
  w.header(4, 4);
  w.item(1, 0);
  w.item(2, 2);
  w.recipe(3, {1}, {{2, 1}, {3, 1}});
  w.recipe(1, {1}, {{1, 1}});
  w.item(3, 0);
  w.item(4, 1);
  w.recipe(1, {1}, {{2, 4}});
  return w.out;
}

// A two-item recycle cycle next to a seed route. `A x5 <- B x1` with
// `B x2 <- A x1` is net positive, so the balance accepts one execution of each
// for four A, and that is the cheapest plan; nothing can fire from an empty
// stock. After the no-good the runner-up starts the loop from the stocked leaf
// and is fireable, so the retry turns a rejected plan into a real one.
//
//   handle 1 (L) leaf, 10 in stock
//   handle 2 (A) <- r0 (x5, ws [1], input B x1)    recycle
//                <- r2 (x1, ws [1], input L x1)    seed
//   handle 3 (B) <- r1 (x2, ws [1], input A x1)    recycle
//   handle 4 (T) <- r3 (x1, ws [1], input A x4)    target
aw::vector<std::byte> buildCycleRetrySample() {
  AwrWriter w;
  w.header(4, 4);
  w.item(1, 0);
  w.item(2, 2);
  w.recipe(5, {1}, {{3, 1}});
  w.recipe(1, {1}, {{1, 1}});
  w.item(3, 1);
  w.recipe(2, {1}, {{2, 1}});
  w.item(4, 1);
  w.recipe(1, {1}, {{2, 4}});
  return w.out;
}

// A directed three-recipe ring, which the eager pass cannot see: no two members
// consume each other's output, so `unitePartners` never merges them and there
// is no 1- or 2-cycle to cut. The ring is net positive (A +4, B +1 per round),
// so the balance runs it from nothing and the cheapest plan for the target is
// unstartable. Only the post-solve component group, which keeps all three
// members out of the funding sum, can exclude it.
//
//   handle 1 (L) leaf, 10 in stock
//   handle 2 (A) <- r0 (x5, ws [1], input B x1)   ring
//                <- r1 (x1, ws [1], input L x1)   seed, cost 10
//   handle 3 (B) <- r2 (x2, ws [1], input C x1)   ring
//   handle 4 (C) <- r3 (x1, ws [1], input A x1)   ring
//   handle 5 (T) <- r4 (x1, ws [1], input A x4)   target
aw::vector<std::byte> buildDirectedRingSample() {
  AwrWriter w;
  w.header(5, 5);
  w.item(1, 0);
  w.item(2, 2);
  w.recipe(5, {1}, {{3, 1}});
  w.cost(10);
  w.recipe(1, {1}, {{1, 1}});
  w.item(3, 1);
  w.recipe(2, {1}, {{4, 1}});
  w.item(4, 1);
  w.recipe(1, {1}, {{2, 1}});
  w.item(5, 1);
  w.recipe(1, {1}, {{2, 4}});
  return w.out;
}

}  // namespace

// The post-solve fireability check. A stock-adjacent SCC is not enough: the
// entry recipe can need more of the seed than the stock holds.
AW_TEST(testFireability) {
  std::cout << "[Test] fireability check\n";
  aw::registerCraftingGraph(buildSeedQuantitySample());
  expect(aw::getCraftingError() == nullptr, "seed quantity sample parses");
  const aw::Handle all[] = {1};

  // One seed: the filter keeps the cycle (S is in stock) but r0 needs two and
  // nothing else is enabled, so the net-balanced r0:2 / r1:1 plan cannot fire.
  // The entry cut weighs the two the cycle needs against the one in stock and
  // proves the plan impossible; with the cuts off the same instance is only
  // rejected.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 1;  // one S
    const aw::Subgraph sub = aw::reachableSubgraph(2, all, inventory);
    const aw::ItemId target = sub.translate(1);  // handle 2 (C)
    expect(target != UINT32_MAX, "the target is in the subgraph");

    aw::solver::Options noCuts;
    noCuts.maxStartupGroups = 0;
    const aw::PlanResult lazy = aw::planCrafting(sub, target, 1, inventory, noCuts);
    expect(lazy.status == aw::PlanStatus::CYCLE_UNFULFILLED,
           "a cycle that needs two seeds but holds one is rejected");
    expect(!lazy.provenOptimal, "a rejected plan claims nothing");

    const aw::PlanResult cut = aw::planCrafting(sub, target, 1, inventory);
    expect(cut.status == aw::PlanStatus::INFEASIBLE,
           "the entry cut proves the short-seeded cycle infeasible");
  }

  // Two seeds: r0 fires, r1 refills S, and the loop reaches any amount.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 2;
    const aw::Subgraph sub = aw::reachableSubgraph(2, all, inventory);
    const aw::PlanResult r = aw::planCrafting(sub, sub.translate(1), 5, inventory);
    expect(r.status == aw::PlanStatus::OK, "two seeds make the cycle fireable");
  }

  // The no-good retry, and the cut that makes it unnecessary: the cheapest
  // balance-feasible plan recycles with no seed and cannot fire. The 2-cycle
  // cut forces the seed before the first solve, so a single solve returns the
  // seeded runner-up. With both the cuts and the retries off, the greedy DAG
  // pre-pass still knows the seeded route, and planCrafting falls back to it
  // instead of returning CYCLE_UNFULFILLED with nothing usable.
  aw::registerCraftingGraph(buildCycleRetrySample());
  expect(aw::getCraftingError() == nullptr, "cycle retry sample parses");
  {
    const aw::Handle stations[] = {1, 2, 3, 4};
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    inventory[0] = 10;  // handle 1 (L)
    const aw::Subgraph sub = aw::reachableSubgraph(4, stations, inventory);
    const aw::ItemId target = sub.translate(3);  // handle 4 (T)
    expect(target != UINT32_MAX, "the retry target is in the subgraph");
    expect(sub.graph.nRecipe == 4, "the retry sample keeps every route");

    aw::solver::Options off;
    off.maxCycleRetries = 0;
    off.maxStartupGroups = 0;  // exercise the fallback, not the cuts
    const aw::PlanResult rejected = aw::planCrafting(sub, target, 1, inventory, off);
    expect(rejected.status == aw::PlanStatus::OK,
           "the greedy fallback answers when the cuts and retries are off");

    aw::solver::Options cutOnly;
    cutOnly.maxCycleRetries = 0;
    const aw::PlanResult cut = aw::planCrafting(sub, target, 1, inventory, cutOnly);
    expect(cut.status == aw::PlanStatus::OK,
           "the 2-cycle cut finds the seeded plan without a retry");

    const aw::PlanResult retried = aw::planCrafting(sub, target, 1, inventory);
    expect(retried.status == aw::PlanStatus::OK,
           "the no-good retry finds the seeded plan");

    // The seeding route costs four executions: one seed, one round of each
    // recycle step, and the target. The rejected plan cost three and used no
    // seed at all. The cut and the retry both land on the four-execution route;
    // the greedy fallback is startable too but takes a longer route, so only
    // its use of the seed is asserted.
    for (const aw::PlanResult *plan : {&cut, &retried}) {
      int64_t total = 0;
      int64_t seed = 0;
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
        total += plan->exec[r];
        const auto inputs = sub.graph.inputsOf(r);
        if (inputs.size() == 1 && sub.itemOrigin[inputs[0]] == 0)  // handle 1 (L)
          seed += plan->exec[r];
      }
      expect(total == 4, "the seeded plan is the runner-up");
      expect(seed > 0, "the seeded plan draws the seed from stock");
    }
    {
      int64_t seed = 0;
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
        const auto inputs = sub.graph.inputsOf(r);
        if (inputs.size() == 1 && sub.itemOrigin[inputs[0]] == 0)  // handle 1 (L)
          seed += rejected.exec[r];
      }
      expect(seed > 0, "the fallback is the seeded route, not the recycle");
    }
  }

  // The 1-cycle (self-loop) cut, on a self-consuming amplifier. The unseeded
  // cost-3 plan is rejected and, when the eager self-loop cut is off, the
  // greedy DAG pre-pass supplies a startable route as a fallback; the eager cut
  // posts the same inequality up front, so a single solve is enough.
  aw::registerCraftingGraph(buildStartupBarrierSample());
  expect(aw::getCraftingError() == nullptr, "startup barrier sample parses");
  {
    const aw::Handle stations[] = {1};
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    inventory[0] = 10;     // handle 1 (L)
    inventory[2] = 1000;   // handle 3 (L2)
    const aw::Subgraph sub = aw::reachableSubgraph(4, stations, inventory);
    const aw::ItemId target = sub.translate(3);  // handle 4 (T)
    expect(target != UINT32_MAX, "the barrier target is in the subgraph");
    expect(sub.graph.nRecipe == 3, "the barrier sample keeps every route");

    aw::solver::Options off;
    off.maxCycleRetries = 0;
    off.maxStartupGroups = 0;
    const aw::PlanResult rejected = aw::planCrafting(sub, target, 1, inventory, off);
    expect(rejected.status == aw::PlanStatus::OK,
           "the greedy fallback answers without barriers");

    aw::solver::Options cutOnly;
    cutOnly.maxCycleRetries = 0;
    const aw::PlanResult cut = aw::planCrafting(sub, target, 1, inventory, cutOnly);
    expect(cut.status == aw::PlanStatus::OK,
           "the self-loop cut finds the seeded plan without a retry");

    const aw::PlanResult retried = aw::planCrafting(sub, target, 1, inventory);
    expect(retried.status == aw::PlanStatus::OK,
           "the barrier cut plus no-good finds the seeded plan");
    // The unseeded plan costs three and cannot fire; the cut and the retry
    // both land on the four-execution route. The greedy fallback is startable
    // too but longer, so it is only checked to be a route other than the
    // unseeded one.
    for (const aw::PlanResult *plan : {&cut, &retried}) {
      int64_t total = 0;
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++)
        total += plan->exec[r];
      expect(total == 4, "the seeded plan is the runner-up");
    }
    {
      int64_t fallbackTotal = 0;
      for (uint32_t r = 0; r < sub.graph.nRecipe; r++)
        fallbackTotal += rejected.exec[r];
      expect(fallbackTotal > 0 && fallbackTotal != 3,
             "the fallback is a startable route, not the unseeded amplifier");
    }
  }

  // The post-solve component group, on a directed three-recipe ring. The eager
  // pass only unites mutually-consuming pairs, so the ring reaches the solver
  // whole; grouping all three members is what keeps the ring's own output out
  // of the funding sum and forces the seed.
  aw::registerCraftingGraph(buildDirectedRingSample());
  expect(aw::getCraftingError() == nullptr, "directed ring sample parses");
  {
    const aw::Handle stations[] = {1};
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    inventory[0] = 10;  // handle 1 (L)
    const aw::Subgraph sub = aw::reachableSubgraph(5, stations, inventory);
    const aw::ItemId target = sub.translate(4);  // handle 5 (T)
    expect(target != UINT32_MAX, "the ring target is in the subgraph");
    expect(sub.graph.nRecipe == 5, "the ring sample keeps every route");

    aw::solver::Options off;
    off.maxStartupGroups = 0;  // the eager pass cannot see a directed ring
    off.maxCycleRetries = 0;
    const aw::PlanResult fallback = aw::planCrafting(sub, target, 1, inventory, off);
    expect(fallback.status == aw::PlanStatus::OK,
           "the greedy fallback answers a directed ring");

    // One retry is enough: the rejection posts the whole-ring group, the ring's
    // own output can no longer fund its start, and the rerun solves the seeded
    // route. The unstartable balance plan runs one ring round for four, so the
    // old per-recipe groups had nothing to exclude; the grouped cut has to
    // force the seed.
    aw::solver::Options oneRetry;
    oneRetry.maxStartupGroups = 0;
    oneRetry.maxCycleRetries = 1;
    const aw::PlanResult ring = aw::planCrafting(sub, target, 1, inventory, oneRetry);
    expect(ring.status == aw::PlanStatus::OK,
           "the component group makes the directed ring startable in one retry");

    int64_t fallbackCost = 0;
    int64_t ringCost = 0;
    int64_t ringSeed = 0;
    for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
      fallbackCost += sub.graph.cost[r] * fallback.exec[r];
      ringCost += sub.graph.cost[r] * ring.exec[r];
      const auto inputs = sub.graph.inputsOf(r);
      if (inputs.size() == 1 && sub.itemOrigin[inputs[0]] == 0)  // handle 1 (L)
        ringSeed += ring.exec[r];
    }
    // One seed (10) plus one round of the ring and the target is 14. The
    // fallback cannot run the ring at all and pays four seeds, 41.
    expect(ringCost == 14, "the component group lands on the seeded ring route");
    expect(ringSeed == 1, "the ring route draws exactly one seed");
    expect(ringCost < fallbackCost,
           "the component cut beats the greedy fallback on the directed ring");
  }
}

// The eager 1- and 2-cycle cuts. They are implied by fireability, so they are
// posted before the first solve: an instance whose cycle cannot start comes
// back INFEASIBLE instead of CYCLE_UNFULFILLED, and one whose cycle needs a seed
// is answered in a single solve.
AW_TEST(testStartupCuts) {
  std::cout << "[Test] eager startup cuts\n";
  aw::registerCraftingGraph(buildAmplifierPairSample());
  expect(aw::getCraftingError() == nullptr, "amplifier pair sample parses");

  const aw::Handle all[] = {1};
  aw::solver::Options noCuts;
  noCuts.maxStartupGroups = 0;

  // Nothing to start from: the cut proves no firing sequence exists, while the
  // plain balance model still returns the net-balanced r0:2 / r1:32 plan for
  // the fireability check to refuse. The seed filter has to be off, or the
  // unseeded pair never reaches the solver at all.
  {
    aw::options.seedPruning = false;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    aw::options.seedPruning = true;
    const aw::ItemId target = sub.translate(0);  // handle 1 (A)
    expect(target != UINT32_MAX, "the pair target is in the subgraph");

    const aw::PlanResult lazy = aw::planCrafting(sub, target, 64, {}, noCuts);
    expect(lazy.status == aw::PlanStatus::CYCLE_UNFULFILLED,
           "the unstartable pair is only rejected without the cut");

    const aw::PlanResult cut = aw::planCrafting(sub, target, 64, {});
    expect(cut.status == aw::PlanStatus::INFEASIBLE,
           "the 2-cycle cut proves the pair unstartable");
  }

  // Sixteen A is exactly the entry the amplifier needs, and it is the item r1
  // turns into the B the amplifier eats.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 16;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    expect(target != UINT32_MAX, "the seeded pair target is in the subgraph");
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::OK, "sixteen A seeds the pair");
    expect(plan.exec.size() == 2 && plan.exec[0] == 2 && plan.exec[1] == 32,
           "the A-seeded pair plan is r0:2 / r1:32");
    expect(aw::planIsFireable(sub, inventory,
                              std::span<const int64_t>(plan.exec.data(), plan.exec.size())),
           "the returned pair plan fires");
  }

  // Sixteen B is the other way in, and the cheaper one: the amplifier can run
  // first and nothing has to be converted.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[1] = 16;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::OK, "sixteen B seeds the pair");
    expect(plan.exec.size() == 2 && plan.exec[0] == 1 && plan.exec[1] == 0,
           "the B-seeded pair plan is one amplifier run");
    expect(aw::planIsFireable(sub, inventory,
                              std::span<const int64_t>(plan.exec.data(), plan.exec.size())),
           "the B-seeded pair plan fires");
  }

  // Eight A funds the first firing of the seed recipe but not the sixteen B the
  // amplifier eats, so the instance is impossible: whatever the balance says,
  // sixteeen units of A or B have to be on the shelf before the cycle turns
  // over, and the shelf holds eight A. Only the joint seed cut sees that, since
  // it weighs the two rows against one demand; the entry group prices `r1` at
  // one A against the eight in stock and passes.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 8;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::INFEASIBLE,
           "the joint seed cut proves eight A unstartable");
  }

  // The same shelf split between the two items does start the cycle: eight A
  // pays for eight firings of the seed recipe, which makes the sixteen B the
  // amplifier eats. The seed cut has to accept it, and the plan it lets through
  // has to be fireable, so this is the case a per-item lump of 16 would get
  // wrong.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 8;
    inventory[1] = 8;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::OK, "eight A and eight B seed the pair");
    expect(aw::planIsFireable(sub, inventory,
                              std::span<const int64_t>(plan.exec.data(), plan.exec.size())),
           "the mixed-seed plan fires");
  }

  // Fifteen and one also add up to the sixteen the round needs, and fifteen
  // alone does not. These are the two sides of the joint inequality.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 15;
    inventory[1] = 1;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::OK, "fifteen A and one B seed the pair");
    expect(aw::planIsFireable(sub, inventory,
                              std::span<const int64_t>(plan.exec.data(), plan.exec.size())),
           "the fifteen-one plan fires");
  }
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[0] = 15;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::INFEASIBLE,
           "fifteen A is one short of the round");
  }

  // A shelf of B alone still starts the pair through the amplifier's own side,
  // and the seed cut leaves that route alone: its demand is on the sum, not on
  // the item the entry group happens to price.
  {
    aw::vector<aw::Amount> inventory(2, 0);
    inventory[1] = 16;
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::ItemId target = sub.translate(0);
    const aw::PlanResult plan = aw::planCrafting(sub, target, 64, inventory);
    expect(plan.status == aw::PlanStatus::OK, "sixteen B seeds the pair with the cut on");
  }
}

}  // namespace plan
