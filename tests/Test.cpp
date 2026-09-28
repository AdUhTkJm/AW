// Unit tests for the .awr decoder and CSR construction. No test framework: a
// tiny assertion helper keeps the WSL build dependency-free.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "aw/CraftingGraph.h"
#include "aw/Options.h"
#include "aw/OptionsJson.h"
#include "aw/Plan.h"
#include "aw/Protocol.h"
#include "aw/Solver.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::cout << "  [FAIL] " << what << '\n';
    ++failures;
  }
}

bool subgraphHasRecipe(const aw::Subgraph &sub, uint32_t source);

void emitVarInt(aw::vector<std::byte> &out, std::uint64_t value) {
  while ((value & ~0x7FULL) != 0) {
    out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<std::byte>(value));
}

// Compact writer for the tag-pruning samples below. Handles must be emitted in
// ascending order, exactly as the Java writer does. `ws` and `inputs` are in
// ascending handle order; the writer stores deltas. A real recipe with no
// workstation is dropped by registerCraftingGraph, so every real recipe here
// names one.
struct AwrWriter {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  std::uint32_t previousHandle = 0;

  void header(std::uint32_t nReal, std::uint32_t entries) {
    emitVarInt(out, nReal);
    emitVarInt(out, entries);
  }

  void item(std::uint32_t handle, std::uint32_t recipes) {
    emitVarInt(out, handle - previousHandle);
    previousHandle = handle;
    emitVarInt(out, recipes);
  }

  void recipe(std::int64_t amount, std::initializer_list<std::uint32_t> ws,
              std::initializer_list<std::pair<std::uint32_t, std::int64_t>> inputs) {
    emitVarInt(out, (std::uint64_t) amount);
    emitVarInt(out, ws.size());
    std::uint32_t previous = 0;
    for (std::uint32_t w : ws) {
      emitVarInt(out, w - previous);
      previous = w;
    }
    emitVarInt(out, inputs.size());
    previous = 0;
    for (const auto& [handle, inputAmount] : inputs) {
      emitVarInt(out, (std::uint64_t) inputAmount);
      emitVarInt(out, handle - previous);
      previous = handle;
    }
  }
};

// A dump that exercises: an item with several recipes, an item with none, a
// pseudo-resource, an empty-input recipe and the workstation encoding the
// current Java writer produces.
//
//   item 1 <- r0 (x4, needs item2 x2 + item3 x1), r1 (x1, needs item1 x5,
//        workstations [1, 2])
//   item 3 <- r2 (x1, needs item1 x1 + item3 x1)
aw::vector<std::byte> buildSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 2);  // realResourceCount
  emitVarInt(out, 2);  // entryCount

  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 4);  // r0 output amount
    emitVarInt(out, 0);  // no workstations
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 2);
    emitVarInt(out, 2);  // -> item 2
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> item 3
  }
  {
    emitVarInt(out, 1);  // r1 output amount
    emitVarInt(out, 2);  // two workstations
    emitVarInt(out, 1);  // absolute first id -> handle 1
    emitVarInt(out, 1);  // +1 -> handle 2
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 5);
    emitVarInt(out, 2);  // -> item 2
  }

  emitVarInt(out, 2);  // output delta -> handle 3 (pseudo)
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 0);  // no workstations
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> item 1
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> item 3
  }
  return out;
}

// A dump with three real recipes for handle 1, all with a workstation:
//
//   r0: item 1 x1 <- item 1 x1      (consumes as much as it makes)
//   r1: item 1 x1 <- item 1 x2      (consumes more than it makes)
//   r2: item 1 x3 <- item 1 x1      (the only net producer)
//
// Registration must drop r0 and r1 and keep r2, with no query involved.
aw::vector<std::byte> buildLossySample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 1);  // realResourceCount
  emitVarInt(out, 1);  // entryCount
  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 3);  // three recipes
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // input amount
    emitVarInt(out, 1);  // -> item 1
  }
  {
    emitVarInt(out, 1);  // r1 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 2);  // input amount
    emitVarInt(out, 1);  // -> item 1
  }
  {
    emitVarInt(out, 3);  // r2 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // input amount
    emitVarInt(out, 1);  // -> item 1
  }
  return out;
}

// A dump with degenerate input amounts. Every recipe is real and has a
// workstation:
//
//   item 1 x4 <- item 2 x2               (r0, no degenerate input)
//   item 1 x1 <- item 1 x0 + item 2 x1   (r1, workstations [1])
//   item 1 x1 <- item 1 x0 + item 2 x1   (r2, workstations [2])
//   item 1 x5 <- item 1 x0               (r3, workstations [1])
//
// An x0 input is not flow, so registration drops the edge, not the recipe: r1
// and r2 become the same recipe and fold together (their workstations are
// united), and r3 turns into an input-less recipe. Item 2 is an unstocked leaf
// no recipe produces, so r0 and r1 are unusable without stock and the
// input-less r3 is the only route to item 1.
aw::vector<std::byte> buildZeroInputSample() {
  AwrWriter w;
  w.header(2, 1);  // real handles 1..2, one entry, for handle 1
  w.item(1, 4);
  w.recipe(4, {1}, {{2, 2}});
  w.recipe(1, {1}, {{1, 0}, {2, 1}});
  w.recipe(1, {2}, {{1, 0}, {2, 1}});
  w.recipe(5, {1}, {{1, 0}});
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount
  emitVarInt(out, 2);  // entries: outputs 1 and 5

  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 1);  // rA output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 3);  // handle 3
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 5);  // -> pseudo handle 5
  }
  {
    emitVarInt(out, 1);  // rB output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 4);  // handle 4
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> item handle 3
  }

  emitVarInt(out, 4);  // output delta -> handle 5
  emitVarInt(out, 2);  // two synthetic recipes
  {
    emitVarInt(out, 1);  // rC output amount
    emitVarInt(out, 0);  // no workstations
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> item handle 1
  }
  {
    emitVarInt(out, 1);  // rD output amount
    emitVarInt(out, 0);  // no workstations
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> item handle 2
  }
  return out;
}

// A dump for the dead-node cleanup. Handles 1..3 are real.
//
//   item 1 <- rT  (x1, workstations [1], inputs item-2 x1)
//          <- rT2 (x1, workstations [1], inputs item-3 x1)
//   item 2 <- rA  (x1, workstations [2], inputs item-3 x1)
//
// item 3 has no producer. Recipe ids in file order: rT=0, rT2=1, rA=2. When
// only workstation 1 is allowed, item 2 keeps a producer in the source graph
// but has none in the subgraph, so the cleanup must drop rT.
aw::vector<std::byte> buildDeadNodeSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 3);  // realResourceCount
  emitVarInt(out, 2);  // entries: outputs 1 and 2

  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 1);  // rT output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 2);  // -> item handle 2
  }
  {
    emitVarInt(out, 1);  // rT2 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 3);  // -> item handle 3
  }

  emitVarInt(out, 1);  // output delta -> handle 2
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // rA output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 2);  // handle 2
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 3);  // -> item handle 3
  }
  return out;
}

// A two item cycle that only balances when the second recipe produces twice
// what it eats. With no inventory the only feasible plan is 2*a of r0 and a of
// r1, for 3*a total executions.
//
//   item 1 <- r0 (x1, workstation [1], input item 2 x1)
//   item 2 <- r1 (x2, workstation [2], input item 1 x1)
aw::vector<std::byte> buildPlanSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 2);  // realResourceCount
  emitVarInt(out, 2);  // entries: handles 1 and 2

  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> item 2
  }

  emitVarInt(out, 1);  // output delta -> handle 2
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 2);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 2);  // handle 2
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> item 1
  }
  return out;
}

// item 1 <- r0 consumes item 2, which has no recipe at all.
aw::vector<std::byte> buildPlanLeafSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 2);  // realResourceCount
  emitVarInt(out, 1);  // entries: handle 1
  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> item 2, a leaf
  }
  return out;
}

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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 3);  // realResourceCount; workstations 1..3 are real
  emitVarInt(out, 1);  // one entry: output handle 1

  emitVarInt(out, 1);  // output delta -> handle 1
  emitVarInt(out, 4);  // four recipes
  {
    emitVarInt(out, 1);  // rA output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 2);  // -> item handle 2
  }
  {
    emitVarInt(out, 1);  // rB output amount
    emitVarInt(out, 2);  // two workstations
    emitVarInt(out, 2);  // handle 2
    emitVarInt(out, 1);  // +1 -> handle 3
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 2);  // -> item handle 2
  }
  {
    emitVarInt(out, 2);  // rC output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 2);  // -> item handle 2
  }
  {
    emitVarInt(out, 1);  // rD output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 2);  // amount 2
    emitVarInt(out, 2);  // -> item handle 2
  }
  return out;
}

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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 5);  // realResourceCount
  emitVarInt(out, 4);  // entries: handles 1, 2, 5, 6

  emitVarInt(out, 1);  // output delta -> handle 1 (glass)
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 3);
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 4);  // -> handle 4 (sand)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (stained glass)
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 3);
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 1);  // glass amount
    emitVarInt(out, 1);  // -> handle 1
    emitVarInt(out, 1);  // dye amount
    emitVarInt(out, 2);  // -> handle 3
  }

  emitVarInt(out, 3);  // output delta -> handle 5 (P)
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 4);
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 6);  // -> pseudo handle 6 (T)
  }

  emitVarInt(out, 1);  // output delta -> handle 6 (T)
  emitVarInt(out, 2);  // two synthetic recipes
  {
    emitVarInt(out, 1);  // T <- stained
    emitVarInt(out, 0);  // no workstations
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2
  }
  {
    emitVarInt(out, 1);  // T <- glass
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 1
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 6);  // realResourceCount
  emitVarInt(out, 5);  // entries: handles 3, 4, 5, 6, 7

  emitVarInt(out, 3);  // output delta -> handle 3 (a)
  emitVarInt(out, 1);  // one recipe
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 1);  // -> handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, 1);  // -> handle 1 (m1)
  }

  emitVarInt(out, 1);  // output delta -> handle 4 (b)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (a)
  }

  emitVarInt(out, 1);  // output delta -> handle 5 (c)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (b)
  }

  emitVarInt(out, 1);  // output delta -> handle 6 (P)
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 1);  // P <- T x5
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);
    emitVarInt(out, 7);  // -> handle 7 (T)
  }
  {
    emitVarInt(out, 1);  // P <- c x1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // -> handle 5 (c)
  }

  emitVarInt(out, 1);  // output delta -> handle 7 (T)
  emitVarInt(out, 2);  // two synthetic recipes
  {
    emitVarInt(out, 1);  // T <- m1
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 1
  }
  {
    emitVarInt(out, 1);  // T <- m2
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2
  }
  return out;
}

aw::vector<std::byte> buildCounterSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 7);  // realResourceCount
  emitVarInt(out, 7);  // entries: handles 3, 4, 5, 6, 7, 8, 9

  emitVarInt(out, 3);  // output delta -> handle 3 (w)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (a)
  }

  emitVarInt(out, 1);  // -> handle 4 (a)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // -> handle 5 (b)
  }

  emitVarInt(out, 1);  // -> handle 5 (b)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 1 (base)
  }

  emitVarInt(out, 1);  // -> handle 6 (m)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 9);  // -> pseudo handle 9 (J)
  }

  emitVarInt(out, 1);  // -> handle 7 (P)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 8);  // -> pseudo handle 8 (T)
  }

  emitVarInt(out, 1);  // -> handle 8 (T)
  emitVarInt(out, 2);  // T <- m, T <- w
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 6);  // -> handle 6 (m)
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (w)
  }

  emitVarInt(out, 1);  // -> handle 9 (J)
  emitVarInt(out, 2);  // J <- w, J <- z
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (w)
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (z), a leaf
  }
  return out;
}

// T = {m, w} with `m x8 <- w x1`. The amount condition forbids pruning.
//
//   handle 1 m <- r0 (x8, ws [2], w x1)
//   handle 3 T <- r1 (x1, no ws, m x1)
//              <- r2 (x1, no ws, w x1)
aw::vector<std::byte> buildBulkSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 2);  // realResourceCount
  emitVarInt(out, 2);  // entries: handles 1, 3

  emitVarInt(out, 1);  // output delta -> handle 1 (m)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 8);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // workstation handle 2
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (w)
  }

  emitVarInt(out, 2);  // output delta -> handle 3 (T)
  emitVarInt(out, 2);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 1 (m)
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (w)
  }
  return out;
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

// Dominated-input substitution. `X <- T` loses to `X x9 <- w` once every member
// of the tag T is paid for with w: A is made directly from w, and B only through
// C, which is the real-input chain the substitution has to follow upward. T is a
// simple tag of {A, B}.
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

// A batched member whose tag edge is a free sink for the batch surplus. The
// guard in Prune.cpp must keep `T <- m` even though every recipe of m consumes
// w at an equal rate: `m x4 <- w x6` emits four units per execution, so a plan
// that runs it for the target gets three units of m for free and spends one of
// them on T.
//
//   handle 1 base <- r0 (x4, ws [1], T x1)     (T recycles back into base)
//   handle 2 w    <- r1 (x6, ws [1], base x4)
//   handle 3 m    <- r2 (x4, ws [1], w x6)     (the batch)
//   handle 4 goal <- r3 (x9, ws [1], G x1)
//   handle 5 T    <- r4 (x1, no ws, w x1)      (synthetic)
//                 <- r5 (x1, no ws, m x1)      (synthetic)
//   handle 6 G    <- r6 (x1, no ws, m x1)      (synthetic, single member)
//
// With `T <- m` the target needs r0 + r1 + r2 + r3 = 4 real crafts: the batch
// makes four m, G eats one and `T <- m` eats one, and the T that r0 needs comes
// back out of the surplus. Without it the T must come from w, which forces a
// second r0/r1 pair, so the same target costs 6 real crafts and dropping the
// edge raises the optimum.
aw::vector<std::byte> buildBatchRecycleSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount: base, w, m, goal
  emitVarInt(out, 6);  // entries: handles 1..6

  emitVarInt(out, 1);  // output delta -> handle 1 (base)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 4);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // -> handle 5 (T)
  }

  emitVarInt(out, 1);  // -> handle 2 (w)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 6);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 4);
    emitVarInt(out, 1);  // -> handle 1 (base)
  }

  emitVarInt(out, 1);  // -> handle 3 (m)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 4);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 6);
    emitVarInt(out, 2);  // -> handle 2 (w)
  }

  emitVarInt(out, 1);  // -> handle 4 (goal)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 9);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 6);  // -> handle 6 (G)
  }

  emitVarInt(out, 1);  // -> handle 5 (T)
  emitVarInt(out, 2);  // T <- w, T <- m
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);  // no workstation
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (w)
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (m)
  }

  emitVarInt(out, 1);  // -> handle 6 (G)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);  // no workstation
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (m)
  }
  return out;
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

  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, n + 1);  // realResourceCount: w + m_i
  emitVarInt(out, n + 1);  // entries: handles 2..n+1 and the tag

  // m_i <- w x1, on workstation w.
  for (uint32_t i = 0; i < n; i++) {
    emitVarInt(out, i == 0 ? 2 : 1);  // output delta -> handle 2 + i
    emitVarInt(out, 1);               // one recipe
    emitVarInt(out, 1);               // output amount
    emitVarInt(out, 1);               // one workstation
    emitVarInt(out, witness);
    emitVarInt(out, 1);               // one input
    emitVarInt(out, 1);               // amount
    emitVarInt(out, witness);         // -> handle 1
  }

  // T <- w, then T <- m_i for every member.
  emitVarInt(out, 1);      // output delta -> handle n + 2 (tag)
  emitVarInt(out, n + 1);  // n + 1 synthetic member edges
  for (uint32_t i = 0; i <= n; i++) {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 0);  // no workstations
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);  // amount
    emitVarInt(out, i == 0 ? witness : 1 + i);  // -> w, else -> m_i
  }
  return out;
}

// The composite-dominance motivating example from docs/algorithm.typ. Black
// candle can be made from black dye or from black pigment, and black pigment
// can be made from black dye (256) or from a black candle (224, a net loss).
// The 224 route must be dropped by the ceremony, the 256 route kept.
//
//   handle 1 black_candle <- r0 (x1, ws [1], black_dye x1 + candle x1)
//                         <- r1 (x1, ws [1], candle x1 + black x256)
//   handle 2 black_dye    <- r2 (x1, ws [1], dye_base x1)
//   handle 3 candle       <- r3 (x1, ws [1], candle_base x1)
//   handle 4 black        <- r4 (x256, ws [1], black_dye x1)
//                         <- r5 (x224, ws [1], black_candle x1)
//   handles 5, 6 are leaves.
aw::vector<std::byte> buildBlackCandleSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 6);  // realResourceCount
  emitVarInt(out, 4);  // entries: handles 1, 2, 3, 4

  emitVarInt(out, 1);  // output delta -> handle 1 (black_candle)
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (black_dye)
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 3 (candle)
  }
  {
    emitVarInt(out, 1);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 2);  // two inputs, ascending handles
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (candle)
    emitVarInt(out, 256);
    emitVarInt(out, 1);  // -> handle 4 (black)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (black_dye)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // -> handle 5 (dye_base)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (candle)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r3 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 6);  // -> handle 6 (candle_base)
  }

  emitVarInt(out, 1);  // output delta -> handle 4 (black)
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 256);  // r4 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);   // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);   // -> handle 2 (black_dye)
  }
  {
    emitVarInt(out, 224);  // r5 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);    // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);    // -> handle 1 (black_candle)
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 5);  // realResourceCount
  emitVarInt(out, 4);  // entries: handles 1, 2, 3, 6

  emitVarInt(out, 1);  // output delta -> handle 1 (coal)
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // torch x4
    emitVarInt(out, 2);  // -> handle 2 (torch)
  }
  {
    emitVarInt(out, 9);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (coal_block)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (torch)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 4);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (stick)
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 6 (J)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (coal_block)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r3 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 1);  // -> handle 1 (coal)
  }

  emitVarInt(out, 3);  // output delta -> handle 6 (J)
  emitVarInt(out, 2);  // two synthetic recipes
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // J <- stick (handle 4)
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 0);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // J <- leaf (handle 5)
  }
  return out;
}

// R is more efficient in Z than S: 5 X from one Z versus 4 X from one Z.
// R must survive even though S has a larger output.
//
//   handle 1 X <- R (x5, ws [1], Y x1)
//              <- S (x4, ws [1], Z x1)
//   handle 2 Y <- r (x1, ws [1], Z x1)
//   handle 3 Z is a leaf.
aw::vector<std::byte> buildAmountRatioSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 3);
  emitVarInt(out, 2);

  emitVarInt(out, 1);  // -> handle 1 (X)
  emitVarInt(out, 2);
  {
    emitVarInt(out, 5);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // Y x1
  }
  {
    emitVarInt(out, 4);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }

  emitVarInt(out, 1);  // -> handle 2 (Y)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }
  return out;
}

// Y has an independent route to a leaf, so `X <- Y` is not dominated by
// `X <- Z`.
//
//   handle 1 X <- R (x1, ws [1], Y x1)
//              <- S (x1, ws [1], Z x1)
//   handle 2 Y <- r (x1, ws [1], K x1)
//   handles 3 Z and 4 K are leaves.
aw::vector<std::byte> buildIndependentRouteSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);
  emitVarInt(out, 2);

  emitVarInt(out, 1);
  emitVarInt(out, 2);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // Y x1
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }

  emitVarInt(out, 1);  // -> handle 2 (Y)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // K x1
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 3);
  emitVarInt(out, 2);

  emitVarInt(out, 1);
  emitVarInt(out, 2);
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);
    emitVarInt(out, 2);  // Y x3
  }
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }

  emitVarInt(out, 1);  // -> handle 2 (Y)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 2);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }
  return out;
}

// The same shape as buildIntegerScalingSample, but Z can be crafted from a leaf
// W. That keeps S undominated in both modes, so R is the only recipe the
// ceiling (or the floored inline count) can affect:
//
//   handle 1 X <- R (x1, ws [1], Y x3)
//              <- S (x1, ws [1], Z x1)
//   handle 2 Y <- r (x2, ws [1], Z x1)
//   handle 3 Z <- z (x1, ws [1], W x1)
//   handle 4 W is a leaf.
//
// ceil(3 / 2) = 2, so `2 Y + R` leaves a spare Y and is not bounded by S. The
// nonoptimal relaxation floors to 1, which wrongly bounds R by S. Recipe ids:
// R=0, S=1, r=2, z=3.
aw::vector<std::byte> buildRelaxedCompositeSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount
  emitVarInt(out, 3);  // entries: handles 1, 2, 3

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 2);  // R and S
  {
    emitVarInt(out, 1);  // R output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 3);
    emitVarInt(out, 2);  // Y x3
  }
  {
    emitVarInt(out, 1);  // S output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (Y)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 2);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // Z x1
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (Z)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // W x1
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 6);  // realResourceCount
  emitVarInt(out, 3);  // entries: handles 1, 2, 3

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 2);  // R and S
  {
    emitVarInt(out, 1);  // R output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS_A
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (Y)
  }
  {
    emitVarInt(out, 1);  // S output amount
    if (sSuperset) {
      emitVarInt(out, 2);
      emitVarInt(out, 4);  // absolute WS_A
      emitVarInt(out, 1);  // +1 -> WS_B
    } else {
      emitVarInt(out, 1);
      emitVarInt(out, 5);  // WS_B only
    }
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (Z)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (Y)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS_A
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (Z)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (Z)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS_A
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 6);  // -> handle 6 (BASE)
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 9);  // realResourceCount
  emitVarInt(out, 4);  // entries: handles 1, 2, 3, 4

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 3);  // R, S1, S2
  {
    emitVarInt(out, 1);  // R output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 7);  // WS_A
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (Y1) x1
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 3 (Y2) x1
  }
  {
    emitVarInt(out, 1);  // S1 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 8);  // WS_B
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (P) x1
  }
  {
    emitVarInt(out, 1);  // S2 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 9);  // WS_C
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (Y2) x1
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (Y1)
  emitVarInt(out, 1);  // r1
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 7);  // WS_A
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (P)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (Y2)
  emitVarInt(out, 1);  // r2
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 7);  // WS_A
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 6);  // -> handle 6 (Q)
  }

  emitVarInt(out, 1);  // output delta -> handle 4 (P)
  emitVarInt(out, 1);  // p
  {
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 7);  // WS_A
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // -> handle 5 (ORE)
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 5);  // realResourceCount
  emitVarInt(out, 2);  // entries: handles 1, 2

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 2);  // r0, r1
  {
    emitVarInt(out, 2);  // r0 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 4);  // WS_A
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 2);
    emitVarInt(out, 2);  // -> handle 2 (A) x2
  }
  {
    emitVarInt(out, 2);  // r1 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 5);  // WS_B
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (A) x1
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (A)
  emitVarInt(out, 1);  // r2
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 4);  // WS_A
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (B) x1
  }
  return out;
}

// Output-amount normalization for the relaxed direct pass. The free X recipe is
// cheaper per unit than the six-output recipe that eats an essence, so the
// normalized comparison drops the latter although one execution of it yields
// six X and one execution of the former only one:
//
//   handle 1 X <- r0 (x1, ws [4], nothing)
//               <- r1 (x6, ws [4], essence x3)
//   handle 2 essence <- r2 (x1, ws [4], Z x1)
//   handle 3 Z is a leaf, handle 4 is WS.
//
// essence has a producer on purpose, so the composite pass leaves r1 alone and
// the direct flag below is the only thing under test.
aw::vector<std::byte> buildNormalizedDominanceSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount: X, essence, Z, WS
  emitVarInt(out, 2);  // entries: handles 1, 2

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 2);  // r0, r1
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS
    emitVarInt(out, 0);  // no inputs
  }
  {
    emitVarInt(out, 6);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS
    emitVarInt(out, 1);
    emitVarInt(out, 3);
    emitVarInt(out, 2);  // -> handle 2 (essence) x3
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (essence)
  emitVarInt(out, 1);  // r2
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (Z) x1
  }
  return out;
}

// Two recipes with the same per-unit column but different batch sizes. The
// normalized comparison must leave both alive: for a single X the finer recipe
// is far cheaper, so collapsing the two into one SCC (what a non-strict
// comparison does) would overproduce X and blow the plan up.
//
//   handle 1 X <- r0 (x1, ws [4], A x1)
//               <- r1 (x6, ws [4], A x6)   ; same A per X
//   handle 2 A <- r2 (x1, ws [4], Z x1)
//   handle 3 Z is a leaf, handle 4 is WS.
aw::vector<std::byte> buildEqualRatioSample() {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount: X, A, Z, WS
  emitVarInt(out, 2);  // entries: handles 1, 2

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 2);  // r0, r1
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (A) x1
  }
  {
    emitVarInt(out, 6);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS
    emitVarInt(out, 1);
    emitVarInt(out, 6);
    emitVarInt(out, 2);  // -> handle 2 (A) x6
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (A)
  emitVarInt(out, 1);  // r2
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // WS
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (Z) x1
  }
  return out;
}

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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount
  emitVarInt(out, 3);  // entries: handles 1, 3, 4

  emitVarInt(out, 1);  // output delta -> handle 1 (ingot)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 9);
    emitVarInt(out, 4);  // -> handle 4 (nugget)
  }

  emitVarInt(out, 2);  // output delta -> handle 3 (pickaxe)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 3);
    emitVarInt(out, 1);  // -> handle 1 (ingot)
    emitVarInt(out, 2);
    emitVarInt(out, 1);  // -> handle 2 (stick)
  }

  emitVarInt(out, 1);  // output delta -> handle 4 (nugget)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (pickaxe)
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 4);  // realResourceCount
  emitVarInt(out, 3);  // entries: handles 1, 2, 3

  emitVarInt(out, 1);  // output delta -> handle 1 (X)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 3);
    emitVarInt(out, 2);  // -> handle 2 (A)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (A)
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 2);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (B)
  }
  {
    emitVarInt(out, 2);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (C)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (B)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r3 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // workstation handle 1
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 1 (X)
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 5);  // realResourceCount
  emitVarInt(out, 3);  // entries: handles 1, 2, 3

  emitVarInt(out, 1);  // output delta -> handle 1 (gear)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // workstation handle 5
    emitVarInt(out, 1);  // one input
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (plate)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (plate)
  emitVarInt(out, 2);  // two recipes
  {
    emitVarInt(out, 1);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // workstation handle 5
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (ore)
  }
  {
    emitVarInt(out, 1);  // r3 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // workstation handle 5
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 3 (variant)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (variant)
  emitVarInt(out, 1);
  {
    emitVarInt(out, variantOut);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // workstation handle 5
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (plate)
  }
  return out;
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
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(out, 9);  // realResourceCount (handle 9 is the workbench)
  emitVarInt(out, 7);  // entries: handles 1, 2, 3, 5, 6, 7, 8

  emitVarInt(out, 1);  // output delta -> handle 1 (T)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r0 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);  // workstation handle 9
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 2 (S)
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 3 (Y)
  }

  emitVarInt(out, 1);  // output delta -> handle 2 (S)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r1 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 5);  // -> handle 5 (CB)
  }

  emitVarInt(out, 1);  // output delta -> handle 3 (Y)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r2 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (X)
  }

  emitVarInt(out, 2);  // output delta -> handle 5 (CB)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r3 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 6);  // -> handle 6 (E)
  }

  emitVarInt(out, 1);  // output delta -> handle 6 (E)
  emitVarInt(out, 2);
  {
    emitVarInt(out, 1);  // r4 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 2);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (X)
    emitVarInt(out, 1);
    emitVarInt(out, 1);  // -> handle 5 (CB)
  }
  {
    emitVarInt(out, 1);  // r5 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 7);  // -> handle 7 (W)
  }

  emitVarInt(out, 1);  // output delta -> handle 7 (W)
  emitVarInt(out, 2);
  {
    emitVarInt(out, 1);  // r6 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 2);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (X)
    emitVarInt(out, 1);
    emitVarInt(out, 2);  // -> handle 6 (E)
  }
  {
    emitVarInt(out, 1);  // r7 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 1);
    emitVarInt(out, 1);
    emitVarInt(out, 8);  // -> handle 8 (O)
  }

  emitVarInt(out, 1);  // output delta -> handle 8 (O)
  emitVarInt(out, 1);
  {
    emitVarInt(out, 1);  // r8 output amount
    emitVarInt(out, 1);
    emitVarInt(out, 9);
    emitVarInt(out, 2);
    emitVarInt(out, 1);
    emitVarInt(out, 4);  // -> handle 4 (X)
    emitVarInt(out, 1);
    emitVarInt(out, 3);  // -> handle 7 (W)
  }
  return out;
}

aw::solver::Matrix makeMatrix(
    uint32_t rows, uint32_t cols,
    const aw::vector<aw::vector<std::pair<uint32_t, int64_t>>> &columns) {
  aw::solver::Matrix A;
  A.rows = rows;
  A.cols = cols;
  A.colStart.push_back(0);
  for (uint32_t j = 0; j < cols; j++) {
    for (const auto &entry : columns[j]) {
      A.rowIndex.push_back(entry.first);
      A.value.push_back(entry.second);
    }
    A.colStart.push_back((uint32_t) A.rowIndex.size());
  }
  return A;
}

void testSolver() {
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

void testReducedCostFixing() {
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
void testFlash() {
  std::cout << "[Test] flash mode\n";

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
}

void testZeroCostColumns() {
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

void testPlan() {
  std::cout << "[Test] crafting plan\n";
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "plan sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  const aw::Handle all[] = {1, 2};
  aw::Subgraph sub = aw::reachableSubgraph(1, all);
  expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 2, "plan subgraph shape");

  const aw::NodeId target = sub.translate(0);
  expect(target == 0, "target is found in the subgraph");
  expect(sub.translate(99) == UINT32_MAX, "missing item reports no index");

  const aw::PlanResult none = aw::planCrafting(sub, target, 4, {});
  expect(none.status == aw::PlanStatus::OK, "plan without inventory is optimal");
  expect(none.exec.size() == 2, "plan has one count per recipe");
  expect(none.exec[0] == 8, "r0 count");
  expect(none.exec[1] == 4, "r1 count");

  // 100 spare item 2 units cover the cycle losses, so r0 alone suffices.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[1] = 100;
  const aw::PlanResult stocked = aw::planCrafting(sub, target, 4, inventory);
  expect(stocked.status == aw::PlanStatus::OK, "plan with inventory is optimal");
  expect(stocked.exec[0] == 4, "stocked r0 count");
  expect(stocked.exec[1] == 0, "stocked r1 count");
}

void testPlanInfeasible() {
  std::cout << "[Test] infeasible plan\n";
  aw::registerCraftingGraph(buildPlanLeafSample());
  expect(aw::getCraftingError() == nullptr, "leaf sample parses");

  const aw::Handle all[] = {1};
  // Satellite elimination now drops the dead raw-material route as well, which
  // would hide the missing leaf this test is about; keep the pass off so the
  // reachability shape stays visible.
  aw::options.satellite.enabled = false;
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  aw::options.satellite.enabled = true;
  expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 1, "leaf subgraph shape");

  const aw::NodeId target = sub.translate(0);
  const aw::PlanResult r = aw::planCrafting(sub, target, 4, {});
  expect(r.status == aw::PlanStatus::INFEASIBLE, "missing leaf makes the plan infeasible");
}

void testDuplicateRecipes() {
  std::cout << "[Test] duplicate recipes\n";
  aw::registerCraftingGraph(buildDuplicateSample());
  expect(aw::getCraftingError() == nullptr, "duplicate sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // rA and rB collapse; rC and rD stay because their amounts differ.
  expect(graph.nRecipe == 3, "recipes that differ only by workstation fold together");
  expect(graph.nItem == 3 && graph.nReal == 3, "duplicate sample shape");

  // The survivor keeps rA's id (0) and order, and gains rB's workstations.
  const auto recipes = graph.i2r.targetsOf(0);
  expect(recipes.size() == 3 && recipes[0] == graph.nItem &&
         recipes[1] == graph.nItem + 1 && recipes[2] == graph.nItem + 2,
         "surviving recipes keep their file order");
  const auto ws = graph.workstations.targetsOf(0);
  expect(ws.size() == 3 && ws[0] == 0 && ws[1] == 1 && ws[2] == 2,
         "workstation sets are merged and deduplicated");
  expect(graph.outputAmt[0] == 1 && graph.outputAmt[1] == 2 &&
         graph.outputAmt[2] == 1,
         "output amounts survive the fold");
  expect(graph.r2i.weightsOf(2).size() == 1 && graph.r2i.weightsOf(2)[0] == 2,
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
  const aw::Handle all[] = {1, 2, 3};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  aw::options.satellite.enabled = true;
  aw::options.directPruning = true;
  aw::options.recipePruning = true;
  expect(sub.graph.nItem == 2, "only the output and its leaf are reachable");
  expect(sub.graph.nRecipe == 3, "the subgraph keeps every surviving recipe");
}

void testSample() {
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
  expect(graph.recipeNode(0) == 3 && graph.recipeNode(1) == 4, "recipe node numbering");

  const auto itemTargets = graph.i2r.targetsOf(0);
  expect(itemTargets.size() == 1 && itemTargets[0] == 3, "item 1 -> recipe 0");
  const auto itemWeights = graph.i2r.weightsOf(0);
  expect(itemWeights.size() == 1 && itemWeights[0] == 1, "item 1 edge weights");
  expect(graph.i2r.targetsOf(1).empty(), "item 2 has no producing recipe");
  const auto pseudo = graph.i2r.targetsOf(2);
  expect(pseudo.size() == 1 && pseudo[0] == 4, "item 3 -> recipe 1");

  const auto r0Targets = graph.r2i.targetsOf(0);
  expect(r0Targets.size() == 1 && r0Targets[0] == 1, "recipe 0 input is item 2");
  const auto r0Weights = graph.r2i.weightsOf(0);
  expect(r0Weights.size() == 1 && r0Weights[0] == 5, "recipe 0 amount");
  const auto r1Targets = graph.r2i.targetsOf(1);
  expect(r1Targets.size() == 2 && r1Targets[0] == 0 && r1Targets[1] == 2,
         "recipe 1 inputs are items 1, 3");

  expect(graph.output[0] == 0 && graph.outputAmt[0] == 1, "recipe 0 output");
  expect(graph.output[1] == 2 && graph.outputAmt[1] == 1, "recipe 1 output");

  expect(graph.workstations.numVertices() == 2, "one workstation row per recipe");
  const auto ws0 = graph.workstations.targetsOf(0);
  expect(ws0.size() == 2 && ws0[0] == 0 && ws0[1] == 1, "recipe 0 workstations");
  expect(graph.workstations.targetsOf(1).empty(), "recipe 1 has no workstations");
}

void testNetLossRecipes() {
  std::cout << "[Test] net-loss recipe drop\n";
  aw::registerCraftingGraph(buildLossySample());
  expect(aw::getCraftingError() == nullptr, "lossy sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // The two recipes that consume at least as much as they make are gone, even
  // though each has a workstation. Only the net producer survives.
  expect(graph.nRecipe == 1, "net-loss recipes are dropped at registration");
  expect(graph.output[0] == 0 && graph.outputAmt[0] == 3, "the survivor is r2");
  const auto inputs = graph.r2i.targetsOf(0);
  const auto weights = graph.r2i.weightsOf(0);
  expect(inputs.size() == 1 && inputs[0] == 0 && weights[0] == 1,
         "the survivor keeps its input");

  // The drop is baked into the graph, so no query can resurrect the losses,
  // not even one that holds stock of the item.
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.satellite.enabled = false;
  aw::options.deadNodePruning = false;
  const aw::Handle all[] = {1};
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[0] = 100;
  const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
  aw::options.deadNodePruning = true;
  aw::options.directPruning = true;
  aw::options.satellite.enabled = true;
  aw::options.recipePruning = true;
  expect(sub.graph.nRecipe == 1 && sub.graph.outputAmt[0] == 3,
         "stock cannot resurrect a dropped net-loss recipe");
}

void testNonPositiveInputs() {
  std::cout << "[Test] degenerate input drop\n";
  aw::registerCraftingGraph(buildZeroInputSample());
  expect(aw::getCraftingError() == nullptr, "zero-input sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // The x0 edges are gone before any pass can read an amount.
  for (uint32_t r = 0; r < graph.nRecipe; r++) {
    for (aw::Amount amount : graph.r2i.weightsOf(r))
      expect(amount > 0, "no non-positive input amount survives registration");
  }
  expect(graph.nRecipe == 3, "the x0 edges are dropped, not their recipes");
  expect(graph.r2i.numEdges() == 2, "a dropped edge leaves no empty slot behind");
  expect(graph.i2r.targetsOf(0).size() == 3, "all three survivors still make item 1");

  // r0, then the fold of r1 and r2, then r3. The fold keeps the lowest recipe id
  // and gains both workstations.
  expect(graph.outputAmt[0] == 4 && graph.outputAmt[1] == 1 && graph.outputAmt[2] == 5,
         "surviving output amounts keep their file order");
  expect(graph.r2i.targetsOf(0).size() == 1 && graph.r2i.targetsOf(0)[0] == 1 &&
             graph.r2i.weightsOf(0)[0] == 2,
         "r0 is untouched");
  expect(graph.r2i.targetsOf(1).size() == 1 && graph.r2i.targetsOf(1)[0] == 1 &&
             graph.r2i.weightsOf(1)[0] == 1,
         "the folded recipe keeps only the real input");
  const auto foldedWs = graph.workstations.targetsOf(1);
  expect(foldedWs.size() == 2 && foldedWs[0] == 0 && foldedWs[1] == 1,
         "recipes that differed only by a degenerate input fold, workstations united");
  expect(graph.r2i.targetsOf(2).empty(), "r3 keeps its output with no input at all");

  // An input-less recipe is not a curiosity to be husked out later: it is the
  // only route that does not need the unstocked leaf, and the plan must use it.
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.satellite.enabled = false;
  aw::options.deadNodePruning = false;
  const aw::Handle all[] = {1, 2};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  expect(sub.graph.nRecipe == 3, "the input-less recipe reaches the subgraph");
  const aw::PlanResult plan = aw::planCrafting(sub, sub.translate(0), 1, {});
  aw::options.deadNodePruning = true;
  aw::options.satellite.enabled = true;
  aw::options.substitutionPruning = true;
  aw::options.directPruning = true;
  aw::options.recipePruning = true;
  expect(plan.status == aw::PlanStatus::OK && plan.provenOptimal, "the plan is proven optimal");
  expect(plan.exec.size() == 3 && plan.exec[0] == 0 && plan.exec[1] == 0 && plan.exec[2] == 1,
         "one firing of the input-less recipe covers the request");
}

void testDegenerateOnlyInput() {
  std::cout << "[Test] a degenerate input is dropped, not the recipe\n";
  aw::registerCraftingGraph(buildDegenerateOnlyInputSample());
  expect(aw::getCraftingError() == nullptr, "degenerate-only sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  expect(graph.nRecipe == 1, "the only recipe for item 1 survives");
  expect(graph.r2i.targetsOf(0).empty(), "its x0 input is gone");
  expect(graph.i2r.targetsOf(0).size() == 1, "item 1 is still produced");

  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.substitutionPruning = false;
  aw::options.satellite.enabled = false;
  aw::options.deadNodePruning = false;
  const aw::Handle all[] = {1, 2};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  const aw::PlanResult plan = aw::planCrafting(sub, sub.translate(0), 1, {});
  aw::options.deadNodePruning = true;
  aw::options.satellite.enabled = true;
  aw::options.substitutionPruning = true;
  aw::options.directPruning = true;
  aw::options.recipePruning = true;
  expect(plan.status == aw::PlanStatus::OK, "item 1 stays feasible");
  expect(plan.exec.size() == 1 && plan.exec[0] == 1, "the surviving recipe is the route");
}

void testReachability() {
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
  const aw::NodeId items[] = {0, 1, 4};
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
  const auto inputs = sub.graph.r2i.targetsOf(0);
  expect(inputs.size() == 1 && inputs[0] == 2, "rA consumes the renumbered pseudo");
  expect(sub.graph.output[0] == 0, "rA output item");

  // The pseudo's synthetic recipes need no workstation and are always kept.
  expect(sub.graph.output[1] == 2 && sub.graph.output[2] == 2, "synthetic outputs");
  const auto pseudoRecipes = sub.graph.i2r.targetsOf(2);
  expect(pseudoRecipes.size() == 2 &&
         pseudoRecipes[0] == sub.graph.nItem + 1 &&
         pseudoRecipes[1] == sub.graph.nItem + 2,
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
    for (aw::NodeId target : sub.graph.i2r.targetsOf(i))
      if (target < sub.graph.nItem || target >= sub.graph.nItem + sub.graph.nRecipe)
        ok = false;
  for (uint32_t r = 0; ok && r < sub.graph.nRecipe; ++r) {
    if (sub.graph.output[r] >= sub.graph.nItem)
      ok = false;
    for (aw::NodeId target : sub.graph.r2i.targetsOf(r))
      if (target >= sub.graph.nItem)
        ok = false;
    bool listed = false;
    for (aw::NodeId target : sub.graph.i2r.targetsOf(sub.graph.output[r]))
      if (target == sub.graph.nItem + r)
        listed = true;
    if (!listed)
      ok = false;
  }
  expect(ok, "subgraph CSRs are well formed");
}

void testDeadNodePruning() {
  std::cout << "[Test] dead-node cleanup\n";
  aw::options.tagPruning = false;
  aw::options.recipePruning = false;
  aw::options.directPruning = false;
  aw::options.pack.enabled = false;
  aw::options.satellite.enabled = false;

  aw::registerCraftingGraph(buildDeadNodeSample());
  expect(aw::getCraftingError() == nullptr, "dead-node sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.nReal == 3 && graph.nItem == 3 && graph.nRecipe == 3,
           "dead-node sample shape");
  }

  const aw::Handle onlyT[] = {1};  // rT and rT2 run; rA needs workstation 2.
  aw::vector<aw::Amount> inventory(3, 0);

  // With the cleanup off, item 2 is a leaf that no surviving recipe produces,
  // and rT is still in the subgraph.
  aw::options.deadNodePruning = false;
  {
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, inventory);
    expect(sub.graph.nItem == 3 && sub.graph.nRecipe == 2,
           "cleanup off keeps the dead leaf");
    expect(subgraphHasRecipe(sub, 0), "rT survives with the cleanup off");
    expect(sub.translate(1) != UINT32_MAX, "item 2 survives with the cleanup off");
  }

  // With it on, rT goes and item 2 disappears with it. rT2 keeps the target and
  // the raw material (item 3) alive.
  aw::options.deadNodePruning = true;
  {
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, inventory);
    expect(sub.graph.nItem == 2 && sub.graph.nRecipe == 1,
           "cleanup drops the dead branch");
    expect(subgraphHasRecipe(sub, 1), "the raw-material route survives");
    expect(!subgraphHasRecipe(sub, 0), "the dead consumer is dropped");
    expect(!subgraphHasRecipe(sub, 2), "the unreachable producer is kept out");
    expect(sub.translate(0) != UINT32_MAX, "the target survives");
    expect(sub.translate(1) == UINT32_MAX, "the dead item is gone");
    expect(sub.translate(2) != UINT32_MAX, "the raw material survives");
  }

  // Stock on item 2 makes rT usable, so nothing is dead.
  {
    aw::vector<aw::Amount> stocked = inventory;
    stocked[1] = 5;  // item 2
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyT, stocked);
    expect(sub.graph.nItem == 3 && sub.graph.nRecipe == 2,
           "stock keeps the branch");
    expect(subgraphHasRecipe(sub, 0), "rT survives when its input is stocked");
  }

  // The cleanup must not change the plan optimum.
  {
    aw::vector<aw::Amount> stocked = inventory;
    stocked[2] = 10;  // item 3, the shared raw input
    auto total = [](const aw::PlanResult &plan) {
      int64_t sum = 0;
      for (int64_t x : plan.exec)
        sum += x;
      return sum;
    };
    aw::options.deadNodePruning = false;
    const aw::Subgraph full = aw::reachableSubgraph(1, onlyT, stocked);
    const aw::PlanResult fullPlan = aw::planCrafting(full, full.translate(0), 1, stocked);
    aw::options.deadNodePruning = true;
    const aw::Subgraph pruned = aw::reachableSubgraph(1, onlyT, stocked);
    const aw::PlanResult prunedPlan =
        aw::planCrafting(pruned, pruned.translate(0), 1, stocked);
    expect(fullPlan.status == prunedPlan.status, "dead-node: status agrees");
    expect(fullPlan.status != aw::PlanStatus::OK || total(fullPlan) == total(prunedPlan),
           "dead-node: optimum agrees");
  }

  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.pack.enabled = true;
  aw::options.satellite.enabled = true;
}

void testRejectsBadInput() {
  std::cout << "[Test] malformed input\n";
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
  aw::vector<std::byte> badStation = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
  emitVarInt(badStation, 1);  // one real resource
  emitVarInt(badStation, 1);  // one output
  emitVarInt(badStation, 1);  // output handle 1
  emitVarInt(badStation, 1);  // one recipe
  emitVarInt(badStation, 1);  // output amount
  emitVarInt(badStation, 1);  // one workstation
  emitVarInt(badStation, 2);  // handle 2 > realResourceCount
  emitVarInt(badStation, 0);  // no inputs
  expect(rejects(badStation), "workstation handle out of range");

  // A rejected blob must leave the last good graph in place: testSample()
  // installed the sample dump and every call above bailed out early.
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  expect(graph.nReal == 2 && graph.nItem == 3 && graph.nRecipe == 2,
         "rejected blob leaves the previous graph in place");
  const auto itemTargets = graph.i2r.targetsOf(0);
  expect(itemTargets.size() == 1 && itemTargets[0] == 3,
         "previous graph is still coherent after a rejected blob");

  // A failure must not poison the next attempt with a stale error.
  const aw::vector<std::byte> bad = {std::byte{'X'}};
  aw::registerCraftingGraph(bad);
  expect(aw::getCraftingError() != nullptr, "bad blob reports an error");
  aw::registerCraftingGraph(buildSample());
  expect(aw::getCraftingError() == nullptr, "good blob after a bad one succeeds");
  aw::clearCraftingError();
}

void testTagPruning() {
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
    for (aw::NodeId recipe : sub.recipeOrigin)
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
    // The A/B switch turns the drop off entirely. Satellite elimination is
    // disabled too: it drops the same dead tag island independently of the tag
    // pass.
    aw::options.tagPruning = false;
    aw::options.satellite.enabled = false;
    const aw::Subgraph sub = aw::reachableSubgraph(5, all);
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

void testWideTagPruning() {
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
void testTagTransitiveClosure() {
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

void testTagPruningParity() {
  std::cout << "[Test] tag pruning preserves the optimum\n";
  aw::registerCraftingGraph(buildGlassSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  // Leaves are free only via inventory, so stock every item with no recipe.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::NodeId m = 0; m < graph.nReal; m++)
    if (graph.i2r.targetsOf(m).empty())
      inventory[m] = 1000000000ULL;

  auto plan = [&](bool prune) {
    aw::options.tagPruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(5, all, inventory);
    const aw::NodeId target = sub.translate(4);
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
void testTagBatchingGuard() {
  std::cout << "[Test] a batched member keeps its tag edge\n";
  aw::registerCraftingGraph(buildBatchRecycleSample());
  expect(aw::getCraftingError() == nullptr, "batch recycle sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  // `T <- m`: T is pseudo handle 5, m is handle 3. It has to stay flagged as
  // undominated, because m's only recipe emits four units at a time.
  bool batchedEdgeKept = false;
  for (aw::NodeId recipeNode : graph.i2r.targetsOf(4)) {
    const uint32_t r = recipeNode - graph.nItem;
    const auto inputs = graph.r2i.targetsOf(r);
    if (inputs.size() == 1 && inputs[0] == 2)
      batchedEdgeKept = graph.tagEdgeDominated[r] == 0;
  }
  expect(batchedEdgeKept, "the batched member T <- m survives the guard");

  const aw::Handle all[] = {1, 2, 3, 4};
  aw::vector<aw::Amount> inventory(graph.nItem, 0);

  // Count only real recipes: the synthetic tag edges are free.
  auto realSteps = [&](bool prune) {
    aw::options.tagPruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::NodeId target = sub.translate(3);
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
  expect(full.first == aw::PlanStatus::OK && pruned.first == aw::PlanStatus::OK,
         "both batch recycle variants are feasible");
  expect(full.second == 4, "the unpruned batch recycle optimum is 4 real crafts");
  expect(pruned.second == full.second,
         "tag pruning does not change the batch recycle optimum");
}

// Nonoptimal mode is on purpose: it may plan worse, but it must never break
// feasibility. These are the samples that witness the guards it drops, so the
// relaxed run must actually mark the edge/recipe -- and, for the batch sample,
// plan worse.
void testNonoptimal() {
  std::cout << "[Test] nonoptimal mode\n";
  const aw::Handle all[] = {1, 2, 3, 4};

  // A batched member: without the guard `T <- m` is marked dominated and the
  // batch surplus is no longer a free sink, so the real optimum rises from 4
  // to 6.
  aw::options.nonoptimal = true;
  aw::registerCraftingGraph(buildBatchRecycleSample());
  expect(aw::getCraftingError() == nullptr, "batch recycle sample parses");
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    bool batchedEdgeDominated = false;
    for (aw::NodeId recipeNode : graph.i2r.targetsOf(4)) {
      const uint32_t r = recipeNode - graph.nItem;
      const auto inputs = graph.r2i.targetsOf(r);
      if (inputs.size() == 1 && inputs[0] == 2)
        batchedEdgeDominated = graph.tagEdgeDominated[r] == 1;
    }
    expect(batchedEdgeDominated, "the relaxed pass drops the batched tag edge");

    aw::vector<aw::Amount> inventory(graph.nItem, 0);
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::NodeId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);
    int64_t real = 0;
    if (r.status == aw::PlanStatus::OK)
      for (uint32_t i = 0; i < sub.graph.nRecipe; i++)
        if (sub.graph.output[i] < sub.graph.nReal)
          real += r.exec[i];
    expect(r.status == aw::PlanStatus::OK, "the relaxed batch plan stays feasible");
    expect(real == 6, "dropping the batched edge costs two real crafts");
  }

  // The composite ceiling: R needs 3 Y but the only producer emits 2, so
  // floor(3 / 2) = 1 wrongly dominates R. The exact run leaves R alone.
  aw::registerCraftingGraph(buildRelaxedCompositeSample());
  expect(aw::getCraftingError() == nullptr, "relaxed composite sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 1,
         "the nonoptimal pass floors the inline count");

  aw::options.nonoptimal = false;
  aw::registerCraftingGraph(buildRelaxedCompositeSample());
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "the ceiling keeps R undominated in exact mode");

  // Output normalization in the direct pass: the free single-output recipe is
  // cheaper per unit than the six-output recipe that eats an essence, so the
  // relaxed pass drops the latter although one execution of it is not
  // replaceable by one execution of the former.
  aw::options.nonoptimal = true;
  aw::registerCraftingGraph(buildNormalizedDominanceSample());
  expect(aw::getCraftingError() == nullptr, "normalized dominance sample parses");
  expect(aw::getCraftingGraph().recipeDirectDominated[1] == 1,
         "the relaxed pass normalizes the direct comparison");
  aw::options.nonoptimal = false;
  aw::registerCraftingGraph(buildNormalizedDominanceSample());
  expect(aw::getCraftingGraph().recipeDirectDominated[1] == 0,
         "raw column dominance keeps the six-output recipe");

  // End to end the relaxed pass trades steps for materials: six X cost one r1
  // plus three essence crafts (4 steps), but with r1 dropped they cost six free
  // r0 crafts (6 steps) and no essence.
  {
    const aw::Handle all[] = {1, 2, 3, 4};
    auto plan = [&](bool relaxed) {
      aw::options.nonoptimal = relaxed;
      aw::registerCraftingGraph(buildNormalizedDominanceSample());
      const aw::CraftingGraph &g = aw::getCraftingGraph();
      aw::vector<aw::Amount> inventory(g.nItem, 0);
      inventory[2] = 1000;  // Z, the leaf behind essence
      const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
      const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), 6, inventory);
      int64_t total = 0;
      for (int64_t x : r.exec)
        total += x;
      return total;
    };
    expect(plan(false) == 4, "the raw plan batches the six-output recipe");
    expect(plan(true) == 6, "the normalized plan falls back to single crafts");
    aw::options.nonoptimal = false;
  }

  // Equal per-unit columns must not collapse: a coarse batch cannot replace a
  // fine one without overproducing, which is how the relaxed pass once turned
  // one nether brick wall into six.
  {
    const aw::Handle all[] = {1, 2, 3, 4};
    aw::options.nonoptimal = true;
    aw::registerCraftingGraph(buildEqualRatioSample());
    const aw::CraftingGraph &g = aw::getCraftingGraph();
    expect(g.recipeDirectDominated[0] == 0 && g.recipeDirectDominated[1] == 0,
           "equal per-unit columns stay incomparable");
    aw::vector<aw::Amount> inventory(g.nItem, 0);
    inventory[2] = 1000;  // Z
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    const aw::PlanResult r = aw::planCrafting(sub, sub.translate(0), 1, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    expect(total == 2, "the finer recipe serves a single X");
    aw::options.nonoptimal = false;
  }
}

void testTagInlining() {
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
    for (aw::NodeId m = 0; m < graph.nReal; m++)
      if (graph.i2r.targetsOf(m).empty())
        inventory[m] = 1000000000ULL;
    inventory[1] = stainedStock;  // handle 2: stained glass, the dominated member
    const aw::Subgraph sub = aw::reachableSubgraph(5, all, inventory);
    const aw::NodeId target = sub.translate(4);
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

void testFreeTagObjective() {
  std::cout << "[Test] tag conversions are free\n";

  aw::registerCraftingGraph(buildFreeTagSample());
  expect(aw::getCraftingError() == nullptr, "free tag sample parses");
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[0] = 1000000000ULL;  // m1
  inventory[1] = 1000000000ULL;  // m2

  const aw::Subgraph sub = aw::reachableSubgraph(6, all, inventory);
  const aw::NodeId target = sub.translate(5);
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
  // The row is `>=`, so overproduction is legal and harmless (the caller never
  // executes a tag edge). It must at least cover the five units P consumes.
  expect(tags >= 5, "the tag covers exactly the consumption at minimum");

  // The fixing pass decides a zero-cost column's neutral domain differently
  // from a costed one. Check that it does not over-tighten the tag edges.
  {
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    const aw::PlanResult fixed = aw::planCrafting(sub, target, 1, inventory, options);
    expect(fixed.status == aw::PlanStatus::OK, "reduced-cost fixing keeps the plan");
    int64_t fixedReal = 0;
    for (uint32_t i = 0; i < sub.graph.nRecipe; i++)
      if (sub.graph.output[i] < sub.graph.nReal)
        fixedReal += fixed.exec[i];
    expect(fixedReal == 1, "reduced-cost fixing keeps the real optimum");
  }
}

void testRecipePruning() {
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
    for (aw::NodeId r : sub.recipeOrigin)
      if (r == 5)
        kept = true;
    expect(kept, "stocking the guard input keeps the dominated recipe");
    const aw::NodeId target = sub.translate(3);
    const aw::PlanResult r = aw::planCrafting(sub, target, 224, inventory);
    expect(r.status == aw::PlanStatus::OK, "a stocked guard still plans");
  }
  {
    // The A/B switch turns the drop off entirely. Satellite elimination is
    // disabled too: it would drop the same dead island on its own.
    const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
    aw::options.recipePruning = false;
    aw::options.satellite.enabled = false;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all);
    aw::options.satellite.enabled = true;
    aw::options.recipePruning = true;
    bool kept = false;
    for (aw::NodeId r : sub.recipeOrigin)
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
    for (aw::NodeId station : graph.recipeDominatorWorkstations[0])
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

void testRecipePruningWorkstations() {
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
    for (aw::NodeId r : sub.recipeOrigin)
      if (r == 0)
        return true;
    return false;
  };
  expect(keepsR(onlyA), "a player without the dominator's station keeps R");
  expect(!keepsR(both), "a player with the dominator's station drops R");

  auto plan = [&](bool prune, std::span<const aw::Handle> ws) {
    aw::options.recipePruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    const aw::NodeId target = sub.translate(0);
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

void testRecipeDominatorWorkstations() {
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
    const aw::vector<aw::NodeId> want = {7, 8};
    expect(graph.recipeDominatorWorkstations[0] == want,
           "both dominators contribute their stations");
  }

  // Leaves are free only through inventory.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::NodeId m = 0; m < graph.nReal; m++)
    if (graph.i2r.targetsOf(m).empty())
      inventory[m] = 1000000000LL;

  const aw::Handle a[] = {7};
  const aw::Handle ab[] = {7, 8};
  const aw::Handle ac[] = {7, 9};

  auto keepsR = [&](std::span<const aw::Handle> ws) {
    const aw::Subgraph sub = aw::reachableSubgraph(1, ws, inventory);
    for (aw::NodeId r : sub.recipeOrigin)
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
    const aw::NodeId target = sub.translate(0);
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

void testRecipePruningParity() {
  std::cout << "[Test] composite pruning preserves the optimum\n";
  aw::registerCraftingGraph(buildBlackCandleSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};

  // Leaves (handles 5 and 6) are free only through inventory.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::NodeId m = 0; m < graph.nReal; m++)
    if (graph.i2r.targetsOf(m).empty())
      inventory[m] = 1000000000LL;

  auto plan = [&](bool prune, aw::Amount amount) {
    aw::options.recipePruning = prune;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::NodeId target = sub.translate(3);
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

void testDirectDominancePruning() {
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
    const aw::vector<aw::NodeId> want = {4};  // node 4 = handle 5 = WS_B
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
    const aw::NodeId target = sub.translate(0);
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

void testSubstitutionPruning() {
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
    const aw::vector<aw::NodeId> guards = {1, 2};  // A, B
    expect(graph.recipeSubstitutedGuards[0] == guards,
           "the tag expands into its members for the stock guard");
    const aw::vector<aw::NodeId> ws = {5};  // WS
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
             aw::vector<aw::NodeId>{1},
         "the real input is its own guard");

  // Substitution preserves the optimum: leaves are free only through stock.
  aw::registerCraftingGraph(buildSubstitutionSample());
  const aw::CraftingGraph &g = aw::getCraftingGraph();
  aw::vector<aw::Amount> inv(g.nItem, 0);
  for (aw::NodeId m = 0; m < g.nReal; m++)
    if (g.i2r.targetsOf(m).empty())
      inv[m] = 1000000000LL;
  auto plan = [&](bool prune, aw::Amount amount) {
    aw::options.substitutionPruning = prune;
    const aw::Handle all[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inv);
    const aw::NodeId target = sub.translate(0);
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

void testReducedCostParity() {
  std::cout << "[Test] reduced-cost fixing preserves the plan optimum\n";
  aw::registerCraftingGraph(buildBlackCandleSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};

  // Leaves are free only through inventory.
  aw::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::NodeId m = 0; m < graph.nReal; m++)
    if (graph.i2r.targetsOf(m).empty())
      inventory[m] = 1000000000LL;

  auto plan = [&](double gap, aw::Amount amount) {
    aw::solver::Options options;
    options.reducedCostGap = gap;
    const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
    const aw::NodeId target = sub.translate(3);
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

bool subgraphHasRecipe(const aw::Subgraph &sub, uint32_t source) {
  for (aw::NodeId r : sub.recipeOrigin)
    if (r == source)
      return true;
  return false;
}

void testPackPruning() {
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
    // Satellite elimination is off here: stick is still an unstocked leaf, so
    // the generalized pass drops the pickaxe recipe no matter what the pack
    // pass does. This block is about the pack certificate's zero-stock gate.
    aw::options.satellite.enabled = false;
    const aw::Subgraph sub = aw::reachableSubgraph(3, all, inventory);
    aw::options.satellite.enabled = true;
    expect(subgraphHasRecipe(sub, 0), "stocked nugget keeps the ingot recipe");
  }

  aw::options.pack.enabled = false;
  // Satellite elimination also drops this cycle once the pack is off, so it is
  // switched off as well to isolate the pack A/B switch.
  aw::options.satellite.enabled = false;
  aw::registerCraftingGraph(buildPackSample());
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.packDominated[0] == 0, "disabling the pass drops no recipe");
    const aw::Subgraph sub = aw::reachableSubgraph(3, all);
    expect(subgraphHasRecipe(sub, 0), "the recipe survives when disabled");
  }
  aw::options.satellite.enabled = true;
  aw::options.pack.enabled = true;
}

void testPackPruningParity() {
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
      const aw::NodeId target = sub.translate(2);
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
      const aw::NodeId target = sub.translate(0);
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
      for (aw::NodeId m = 0; m < graph.nReal; m++)
        if (graph.i2r.targetsOf(m).empty())
          inventory[m] = 1000000000LL;
      const aw::Subgraph sub = aw::reachableSubgraph(4, all, inventory);
      const aw::NodeId target = sub.translate(3);
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

void testSatellitePruning() {
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
void testSatelliteLeakPruning() {
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
  aw::registerCraftingGraph(buildSatelliteLeakSample());
  {
    aw::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    const aw::Subgraph sub = aw::reachableSubgraph(1, all, inventory);
    expect(keepsIsland(sub), "the generalized pass can be switched off");
  }
  aw::options.satellite.enabled = true;
  aw::options.tagPruning = true;
  aw::options.recipePruning = true;
  aw::options.directPruning = true;
  aw::options.pack.enabled = true;
}

void testSatellitePruningParity() {
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
      const aw::NodeId target = sub.translate(0);
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
      const aw::NodeId target = sub.translate(0);
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

}  // namespace

// Recomputes the balance `planCrafting` hands the solver and returns whether
// `exec` satisfies it. Mirrors `deriveMissing` in tools/Bench.cpp.
bool greedyBalances(const aw::Subgraph &sub, aw::NodeId target, aw::Amount amount,
                    const aw::vector<aw::Amount> &inventory,
                    const aw::vector<int64_t> &exec) {
  const aw::BaseCraftingGraph &g = sub.graph;
  aw::vector<__int128> balance(g.nItem, 0);
  for (uint32_t r = 0; r < g.nRecipe; r++) {
    const __int128 times = exec[r];
    if (times == 0)
      continue;
    balance[g.output[r]] += (__int128) g.outputAmt[r] * times;
    const auto inputs = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      balance[inputs[k]] -= (__int128) weights[k] * times;
  }
  for (uint32_t i = 0; i < g.nItem; i++) {
    const aw::NodeId source = sub.itemOrigin[i];
    const aw::Amount available = source < inventory.size() ? inventory[source] : 0;
    const __int128 required = i == target ? (__int128) amount : -(__int128) available;
    if (balance[i] < required)
      return false;
  }
  return true;
}

void testGreedyDag() {
  std::cout << "[Test] greedy DAG pre-pass\n";

  // A DAG with a zero-input route: greedy must find it and produce a plan that
  // balances.
  aw::registerCraftingGraph(buildZeroInputSample());
  expect(aw::getCraftingError() == nullptr, "greedy DAG sample parses");
  {
    const aw::Handle all[] = {1, 2};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    const aw::NodeId target = sub.translate(0);
    const aw::vector<int64_t> exec = aw::greedyDagPlan(sub, target, 7, {});
    expect(!exec.empty(), "greedy finds a zero-input route");
    expect(exec.size() == sub.graph.nRecipe, "greedy result has one count per recipe");
    if (!exec.empty())
      expect(greedyBalances(sub, target, 7, {}, exec), "greedy DAG plan balances");
    // Flash mode must pass the same plan through untouched and still feasible.
    aw::solver::Options options;
    options.flash = true;
    const aw::PlanResult flash = aw::planCrafting(sub, target, 7, {}, options);
    expect(flash.status == aw::PlanStatus::OK && greedyBalances(sub, target, 7, {}, flash.exec),
           "flash returns the greedy plan and it balances");
  }

  // A pure cycle with no stock: cutting the back-edge removes the only producer
  // of the second item, so greedy declines instead of inventing a plan.
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "greedy cycle sample parses");
  {
    const aw::Handle all[] = {1, 2};
    const aw::Subgraph sub = aw::reachableSubgraph(1, all);
    const aw::NodeId target = sub.translate(0);
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
    const aw::NodeId target = sub.translate(0);
    expect(aw::greedyDagPlan(sub, target, 4, {}).empty(), "greedy declines a missing leaf");
    expect(aw::planCrafting(sub, target, 4, {}).status == aw::PlanStatus::INFEASIBLE,
           "planner still reports the missing leaf");
  }
}

// ---------------------------------------------------------------------------
// Plan protocol
// ---------------------------------------------------------------------------

// Mirrors the mod's PlanCodec: handles ascend, the first delta is absolute.
aw::vector<std::byte> encodePlanRequest(
    aw::Handle target, aw::Amount amount,
    std::initializer_list<aw::Handle> workstations,
    std::initializer_list<std::pair<aw::Handle, aw::Amount>> stock) {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'P'}, std::byte{1}};
  emitVarInt(out, (std::uint64_t) amount);
  emitVarInt(out, target);
  emitVarInt(out, workstations.size());
  {
    std::uint32_t previous = 0;
    for (aw::Handle handle : workstations) {
      emitVarInt(out, handle - previous);
      previous = handle;
    }
  }
  emitVarInt(out, stock.size());
  {
    std::uint32_t previous = 0;
    for (const auto& [handle, available] : stock) {
      emitVarInt(out, handle - previous);
      previous = handle;
      emitVarInt(out, (std::uint64_t) available);
    }
  }
  return out;
}

// The response decoder, kept independent of src/Protocol.cpp so a matching bug
// on both sides cannot hide.
struct BlobReader {
  std::span<const std::byte> bytes;
  std::size_t pos = 0;
  bool ok = true;

  std::uint8_t byte() {
    if (pos >= bytes.size()) {
      ok = false;
      return 0;
    }
    return std::to_integer<std::uint8_t>(bytes[pos++]);
  }

  std::uint64_t var() {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 10; i++) {
      const std::uint8_t b = byte();
      value |= (std::uint64_t) (b & 0x7F) << (7 * i);
      if (!(b & 0x80))
        return value;
    }
    ok = false;
    return value;
  }

  double f64() {
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 8; i++)
      bits |= (std::uint64_t) byte() << (8 * i);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
};

struct DecodedUse {
  aw::Amount count = 0;
  aw::Handle output = 0;
  aw::Amount outputAmount = 0;
  aw::vector<aw::Handle> inputs;
  aw::vector<aw::Amount> inputAmounts;
};

struct DecodedResponse {
  bool ok = false;
  int status = -1;
  bool provenOptimal = false;
  aw::vector<DecodedUse> uses;
};

DecodedResponse decodePlanResponse(const aw::vector<std::byte>& blob) {
  DecodedResponse out;
  BlobReader in{blob};
  if (in.byte() != 'A' || in.byte() != 'W' || in.byte() != 'Q' || in.byte() != 1)
    return out;
  out.status = (int) in.var();
  out.provenOptimal = in.byte() != 0;
  (void) in.f64();  // gap
  (void) in.f64();  // bestBound
  (void) in.var();  // numConflicts
  (void) in.var();  // numBranches
  (void) in.var();  // fixedColumns
  const std::uint64_t uses = in.var();
  for (std::uint64_t i = 0; i < uses && in.ok; i++) {
    DecodedUse use;
    use.count = (aw::Amount) in.var();
    use.output = (aw::Handle) in.var();
    use.outputAmount = (aw::Amount) in.var();
    const std::uint64_t inputs = in.var();
    std::uint32_t handle = 0;
    for (std::uint64_t k = 0; k < inputs && in.ok; k++) {
      handle += (std::uint32_t) in.var();
      use.inputs.push_back(handle);
      use.inputAmounts.push_back((aw::Amount) in.var());
    }
    out.uses.push_back(std::move(use));
  }
  out.ok = in.ok && in.pos == blob.size();
  return out;
}

void testPlanProtocol() {
  std::cout << "[Test] plan protocol\n";
  const aw::TagInlineMode savedInlining = aw::options.tagInlining;
  aw::options.tagInlining = aw::TagInlineMode::OFF;
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "protocol sample parses");

  std::string error;
  const auto request = encodePlanRequest(1, 4, {1, 2}, {});
  const auto response = aw::planBlob(request, error);
  expect(error.empty(), "a well-formed request is accepted");
  expect(!response.empty(), "a response came back");

  const DecodedResponse decoded = decodePlanResponse(response);
  expect(decoded.ok, "the response decodes exactly");
  expect(decoded.status == (int) aw::PlanStatus::OK, "the plan is feasible");
  expect(decoded.provenOptimal, "the small plan is proven optimal");
  expect(decoded.uses.size() == 2, "two recipes are executed");
  if (decoded.uses.size() == 2) {
    expect(decoded.uses[0].count == 8 && decoded.uses[0].output == 1 &&
               decoded.uses[0].outputAmount == 1,
           "r0 produces item 1 eight times");
    expect(decoded.uses[0].inputs.size() == 1 && decoded.uses[0].inputs[0] == 2 &&
               decoded.uses[0].inputAmounts[0] == 1,
           "r0 consumes item 2");
    expect(decoded.uses[1].count == 4 && decoded.uses[1].output == 2 &&
               decoded.uses[1].outputAmount == 2,
           "r1 produces item 2 four times");
    expect(decoded.uses[1].inputs.size() == 1 && decoded.uses[1].inputs[0] == 1 &&
               decoded.uses[1].inputAmounts[0] == 1,
           "r1 consumes item 1");
  }

  // 100 spare item 2 units cover the cycle losses, so r0 alone suffices.
  error.clear();
  const auto stocked = aw::planBlob(encodePlanRequest(1, 4, {1, 2}, {{2, 100}}), error);
  expect(error.empty(), "a request with stock is accepted");
  const DecodedResponse stockedDecoded = decodePlanResponse(stocked);
  expect(stockedDecoded.ok && stockedDecoded.uses.size() == 1,
         "stock removes the recycling recipe");
  if (stockedDecoded.uses.size() == 1)
    expect(stockedDecoded.uses[0].count == 4, "r0 runs only four times with stock");

  // Malformed requests. Each one must be rejected, not silently coerced.
  auto rejects = [&](const aw::vector<std::byte>& blob, const char* what) {
    std::string message;
    const auto result = aw::planBlob(blob, message);
    expect(result.empty() && !message.empty(), what);
  };
  rejects({}, "an empty request is rejected");
  rejects({std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}, std::byte{1}},
          "a recipe blob is not a plan request");
  {
    auto zero = encodePlanRequest(1, 4, {1, 2}, {});
    zero[4] = std::byte{0};  // varint amount
    rejects(zero, "a zero amount is rejected");
  }
  {
    auto repeated = encodePlanRequest(1, 4, {1, 2}, {});
    // Overwrite the second workstation delta with a zero.
    repeated[8] = std::byte{0};
    rejects(repeated, "a repeated workstation handle is rejected");
  }
  {
    auto trailing = encodePlanRequest(1, 4, {1, 2}, {});
    trailing.push_back(std::byte{0});
    rejects(trailing, "trailing bytes are rejected");
  }
  rejects(encodePlanRequest(99, 4, {1, 2}, {}), "an out-of-range target is rejected");
  rejects(encodePlanRequest(1, 4, {1, 99}, {}), "an out-of-range workstation is rejected");
  rejects(encodePlanRequest(1, 4, {1, 2}, {{99, 1}}), "an out-of-range stock handle is rejected");

  aw::options.tagInlining = savedInlining;
}

void testOptionsJson() {
  std::cout << "[Test] options json\n";
  const aw::Options savedPlanner = aw::options;
  const aw::solver::Options savedSolver = aw::solverOptions;
  std::string error;

  expect(aw::applyPlannerOptionsJson(
             R"({"tagPruning": false, "maxTagMembers": 12, "tagInlining": "queryTime", "pack": {"enabled": false}})",
             error),
         "a partial planner patch applies");
  expect(!aw::options.tagPruning, "a planner switch is updated");
  expect(aw::options.maxTagMembers == 12, "a planner budget is updated");
  expect(aw::options.tagInlining == aw::TagInlineMode::QUERY_TIME, "the tag-inlining enum is updated");
  expect(!aw::options.pack.enabled, "a nested planner object is updated");
  expect(aw::options.recipePruning, "an absent key keeps its current value");

  error.clear();
  expect(!aw::applyPlannerOptionsJson(R"({"nope": 1})", error) && !error.empty(),
         "an unknown planner key is rejected");
  expect(aw::options.maxTagMembers == 12, "a rejected patch changes nothing");

  error.clear();
  expect(!aw::applyPlannerOptionsJson(R"({"maxTagMembers": -1})", error) && !error.empty(),
         "a negative budget is rejected");
  expect(!aw::applyPlannerOptionsJson(R"({"pack": 3})", error), "a non-object nested field is rejected");
  expect(!aw::applyPlannerOptionsJson(R"({"tagInlining": "sometimes"})", error),
         "an unknown tag-inlining mode is rejected");
  expect(!aw::applyPlannerOptionsJson("not json", error), "a malformed document is rejected");

  error.clear();
  const std::string dumped = aw::plannerOptionsJson();
  expect(aw::applyPlannerOptionsJson(dumped, error), "the dumped planner options apply verbatim");
  expect(error.empty(), "round-tripping the planner options reports no error");

  error.clear();
  expect(aw::applySolverOptionsJson(R"({"flash": true, "numWorkers": 1, "maxTimeSeconds": 0.5})", error),
         "a partial solver patch applies");
  expect(aw::solverOptions.flash && aw::solverOptions.numWorkers == 1, "solver scalars are updated");
  expect(aw::solverOptions.maxTimeSeconds == 0.5, "a solver double is updated");
  expect(aw::solverOptions.relativeGap == savedSolver.relativeGap, "an absent solver key keeps its value");
  expect(!aw::applySolverOptionsJson(R"({"maxTimeSeconds": "slow"})", error),
         "a wrong solver type is rejected");

  error.clear();
  const std::string dumpedSolver = aw::solverOptionsJson();
  expect(aw::applySolverOptionsJson(dumpedSolver, error), "the dumped solver options apply verbatim");

  aw::options = savedPlanner;
  aw::solverOptions = savedSolver;
}

int main() {
  // Nonoptimal mode may prune more than the optimum allows, so the parity tests
  // below -- which assert that pruning preserves the optimum -- must run with
  // it off. The dedicated testNonoptimal exercises the relaxed behaviour.
  aw::options.nonoptimal = false;

  testSample();
  testRejectsBadInput();
  testNetLossRecipes();
  testNonPositiveInputs();
  testDegenerateOnlyInput();
  testReachability();
  testDeadNodePruning();
  testSolver();
  testReducedCostFixing();
  testFlash();
  testZeroCostColumns();
  testPlan();
  testPlanInfeasible();
  testGreedyDag();
  testDuplicateRecipes();
  testTagPruning();
  testWideTagPruning();
  testTagTransitiveClosure();
  testTagPruningParity();
  testTagBatchingGuard();
  testNonoptimal();
  testTagInlining();
  testFreeTagObjective();
  testRecipePruning();
  testRecipePruningWorkstations();
  testRecipeDominatorWorkstations();
  testRecipePruningParity();
  testDirectDominancePruning();
  testSubstitutionPruning();
  testReducedCostParity();
  testPackPruning();
  testPackPruningParity();
  testSatellitePruning();
  testSatelliteLeakPruning();
  testSatellitePruningParity();
  testPlanProtocol();
  testOptionsJson();

  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cout << failures << " check(s) failed\n";
  return 1;
}
