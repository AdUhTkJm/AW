// Shared samples: .awr dumps and graph probes read by more than one test file.

#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>

#include "AwrWriter.h"
#include "aw/plan/CraftingGraph.h"
#include "aw/utils/PodVector.h"

namespace plan {

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

// A two item cycle that only balances when the second recipe produces twice
// what it eats. With no inventory the only feasible plan is 2*a of r0 and a of
// r1, for 3*a total executions.
//
//   item 1 <- r0 (x1, workstation [1], input item 2 x1)
//   item 2 <- r1 (x2, workstation [2], input item 1 x1)
aw::vector<std::byte> buildPlanSample() {
  AwrWriter w;
  w.header(2, 2);
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 1}});
  w.item(2, 1);
  w.recipe(2, {2}, {{1, 1}});
  return w.out;
}

// item 1 <- r0 consumes item 2, which has no recipe at all.
aw::vector<std::byte> buildPlanLeafSample() {
  AwrWriter w;
  w.header(2, 1);
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 1}});
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
  AwrWriter w;
  w.header(4, 6);
  w.item(1, 1);
  w.recipe(4, {1}, {{5, 1}});
  w.item(2, 1);
  w.recipe(6, {1}, {{1, 4}});
  w.item(3, 1);
  w.recipe(4, {1}, {{2, 6}});
  w.item(4, 1);
  w.recipe(9, {1}, {{6, 1}});
  w.item(5, 2);
  w.recipe(1, {}, {{2, 1}});
  w.recipe(1, {}, {{3, 1}});
  w.item(6, 1);
  w.recipe(1, {}, {{3, 1}});
  return w.out;
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
  AwrWriter w;
  w.header(6, 4);
  w.item(1, 2);
  w.recipe(1, {1}, {{2, 1}, {3, 1}});
  w.recipe(1, {1}, {{3, 1}, {4, 256}});
  w.item(2, 1);
  w.recipe(1, {1}, {{5, 1}});
  w.item(3, 1);
  w.recipe(1, {1}, {{6, 1}});
  w.item(4, 2);
  w.recipe(256, {1}, {{2, 1}});
  w.recipe(224, {1}, {{1, 1}});
  return w.out;
}

bool subgraphHasRecipe(const aw::Subgraph &sub, uint32_t source) {
  for (aw::ItemId r : sub.recipeOrigin)
    if (r == source)
      return true;
  return false;
}

}  // namespace plan
