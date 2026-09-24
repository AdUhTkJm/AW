// Unit tests for the .awr decoder and CSR construction. No test framework: a
// tiny assertion helper keeps the WSL build dependency-free.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "aw/CraftingGraph.h"
#include "aw/Plan.h"
#include "aw/Solver.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::cout << "  [FAIL] " << what << '\n';
    ++failures;
  }
}

void emitVarInt(std::vector<std::byte>& out, std::uint64_t value) {
  while ((value & ~0x7FULL) != 0) {
    out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<std::byte>(value));
}

// A dump that exercises: an item with several recipes, an item with none, a
// pseudo-resource, an empty-input recipe and the workstation encoding the
// current Java writer produces.
//
//   item 1 <- r0 (x4, needs item2 x2 + item3 x1), r1 (x1, needs item1 x5,
//        workstations [1, 2])
//   item 3 <- r2 (x1, needs item1 x1 + item3 x1)
std::vector<std::byte> buildSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
    emitVarInt(out, 1);  // -> item 1
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

// A dump for the reachability walk. Handles 1..4 are real (item nodes 0..3) and
// handle 5 is a pseudo-resource (node 4) whose members are handles 1 and 2.
//
//   item 1    <- rA (x1, workstations [3], inputs pseudo-5 x1)
//             <- rB (x1, workstations [4], inputs item-3 x1)
//   pseudo-5  <- rC (x1, no workstations, inputs item-1 x1)   (synthetic)
//             <- rD (x1, no workstations, inputs item-2 x1)   (synthetic)
//
// Recipe ids in file order: rA=0, rB=1, rC=2, rD=3.
std::vector<std::byte> buildReachSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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

// A two item cycle that only balances when the second recipe produces twice
// what it eats. With no inventory the only feasible plan is 2*a of r0 and a of
// r1, for 3*a total executions.
//
//   item 1 <- r0 (x1, workstation [1], input item 2 x1)
//   item 2 <- r1 (x2, workstation [2], input item 1 x1)
std::vector<std::byte> buildPlanSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildPlanLeafSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildDuplicateSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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

// T = {glass, stained}. Stained glass is gated by glass (stained x8 <- glass
// x8 + dye x1), so the synthetic edge `T <- stained` is dominated while
// `T <- glass` is not.
//
//   handle 1 glass   <- r0 (x1, ws [3], sand x1)
//   handle 2 stained <- r1 (x8, ws [3], glass x8 + dye x1)
//   handle 5 P       <- r2 (x1, ws [4], T x1)
//   handle 6 T       <- r3 (x1, no ws, stained x1)   (synthetic)
//                    <- r4 (x1, no ws, glass x1)     (synthetic)
std::vector<std::byte> buildGlassSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
    emitVarInt(out, 8);  // output amount
    emitVarInt(out, 1);  // one workstation
    emitVarInt(out, 3);
    emitVarInt(out, 2);  // two inputs
    emitVarInt(out, 8);  // glass amount
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
std::vector<std::byte> buildCounterSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildBulkSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildBlackCandleSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildCoalRegressionSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildAmountRatioSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildIndependentRouteSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildIntegerScalingSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildWorkstationGuardSample(bool sSuperset) {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildPackSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
std::vector<std::byte> buildPackBranchSample() {
  std::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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

aw::solver::Matrix makeMatrix(
    uint32_t rows, uint32_t cols,
    const std::vector<std::vector<std::pair<uint32_t, int64_t>>> &columns) {
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
    const std::vector<int64_t> b = {4, 1};
    const std::vector<int64_t> c = {1, 1};
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
    const std::vector<int64_t> b = {5};
    const std::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.objective == 3,
           "integrality rounds a fractional bound up");
  }

  // An empty row reads 0 >= b, so a positive requirement is unsatisfiable.
  {
    const aw::solver::Matrix A = makeMatrix(2, 1, {{{0, 1}}});
    const std::vector<int64_t> b = {1, 1};
    const std::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::INFEASIBLE, "infeasible model is detected");
  }

  // x0 >= 5 and -x0 >= -1 cannot both hold.
  {
    const aw::solver::Matrix A = makeMatrix(2, 1, {{{0, 1}, {1, -1}}});
    const std::vector<int64_t> b = {5, -1};
    const std::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::INFEASIBLE, "contradictory rows are detected");
  }

  // A negative right hand side is free starting stock.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 1}}});
    const std::vector<int64_t> b = {-5};
    const std::vector<int64_t> c = {1};
    const aw::solver::Result r = aw::solver::solve(A, b, c);
    expect(r.status == aw::PlanStatus::OK && r.objective == 0 && r.x[0] == 0,
           "negative rhs needs no production");
  }

  // A starting cap below the requirement must be grown, not treated as a
  // hard limit.
  {
    const aw::solver::Matrix A = makeMatrix(1, 1, {{{0, 1}}});
    const std::vector<int64_t> b = {5};
    const std::vector<int64_t> c = {1};
    aw::solver::Options tight;
    tight.objectiveCap = 1;
    const aw::solver::Result r = aw::solver::solve(A, b, c, tight);
    expect(r.status == aw::PlanStatus::OK && r.objective == 5,
           "grows the objective cap until it stops binding");
  }

  // One worker and a fixed seed must be reproducible.
  {
    const aw::solver::Matrix A = makeMatrix(1, 2, {{{0, 1}}, {{0, 1}}});
    const std::vector<int64_t> b = {7};
    const std::vector<int64_t> c = {1, 2};
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
  const std::vector<int64_t> b = {7};
  const std::vector<int64_t> c = {1, 1};

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
    const std::vector<int64_t> b2 = {7};
    const std::vector<int64_t> c2 = {1, 1};
    aw::solver::Options options;
    options.reducedCostGap = 0.5;
    const aw::solver::Result r = aw::solver::solve(A2, b2, c2, options);
    expect(r.status == aw::PlanStatus::OK && r.provenOptimal && r.objective == 4,
           "a capped column keeps the optimum");
    expect(r.fixedColumns == 0, "a positively bounded column is not reported fixed");
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
  std::vector<aw::Amount> inventory(graph.nItem, 0);
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
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
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
  // own.
  aw::setRecipePruningEnabled(false);
  const aw::Handle all[] = {1, 2, 3};
  const aw::Subgraph sub = aw::reachableSubgraph(1, all);
  aw::setRecipePruningEnabled(true);
  expect(sub.graph.nItem == 2, "only the output and its leaf are reachable");
  expect(sub.graph.nRecipe == 3, "the subgraph keeps every surviving recipe");
}

void testSample() {
  std::cout << "[Test] sample dump\n";
  const std::vector<std::byte> bytes = buildSample();
  aw::registerCraftingGraph(bytes);
  const aw::CraftingGraph &graph = aw::getCraftingGraph();

  expect(graph.nReal == 2, "realResourceCount");
  expect(graph.nItem == 3, "itemCount");
  expect(graph.nRecipe == 3, "recipeCount");
  expect(!graph.isRealItem(2), "handle 3 is a pseudo-resource");
  expect(graph.isRealItem(0) && graph.isRealItem(1), "handles 1 and 2 are real");
  expect(graph.recipeNode(0) == 3 && graph.recipeNode(2) == 5, "recipe node numbering");

  const auto itemTargets = graph.i2r.targetsOf(0);
  expect(itemTargets.size() == 2 && itemTargets[0] == 3 && itemTargets[1] == 4,
       "item 1 -> recipes 0, 1");
  const auto itemWeights = graph.i2r.weightsOf(0);
  expect(itemWeights.size() == 2 && itemWeights[0] == 4 && itemWeights[1] == 1,
       "item 1 edge weights");
  expect(graph.i2r.targetsOf(1).empty(), "item 2 has no producing recipe");
  const auto pseudo = graph.i2r.targetsOf(2);
  expect(pseudo.size() == 1 && pseudo[0] == 5, "item 3 -> recipe 2");

  const auto r0Targets = graph.r2i.targetsOf(0);
  expect(r0Targets.size() == 2 && r0Targets[0] == 1 && r0Targets[1] == 2,
       "recipe 0 inputs are items 2, 3");
  const auto r0Weights = graph.r2i.weightsOf(0);
  expect(r0Weights.size() == 2 && r0Weights[0] == 2 && r0Weights[1] == 1, "recipe 0 amounts");
  expect(graph.r2i.targetsOf(1).size() == 1 &&
         graph.r2i.targetsOf(1)[0] == 0,
       "recipe 1 input is item 1");
  expect(graph.r2i.targetsOf(2).size() == 2, "recipe 2 has two inputs");

  expect(graph.output[0] == 0 && graph.outputAmt[0] == 4, "recipe 0 output");
  expect(graph.output[1] == 0 && graph.outputAmt[1] == 1, "recipe 1 output");
  expect(graph.output[2] == 2 && graph.outputAmt[2] == 1, "recipe 2 output");

  expect(graph.workstations.numVertices() == 3, "one workstation row per recipe");
  const auto ws1 = graph.workstations.targetsOf(1);
  expect(ws1.size() == 2 && ws1[0] == 0 && ws1[1] == 1, "recipe 1 workstations");
  expect(graph.workstations.targetsOf(0).empty(), "recipe 0 has no workstations");
  expect(graph.workstations.targetsOf(2).empty(), "recipe 2 has no workstations");
}

void testReachability() {
  std::cout << "[Test] reachable subgraph\n";
  const std::vector<std::byte> bytes = buildReachSample();
  aw::registerCraftingGraph(bytes);
  expect(aw::getCraftingError() == nullptr, "reach sample parses");
  const aw::CraftingGraph& graph = aw::getCraftingGraph();
  expect(graph.nReal == 4 && graph.nItem == 5 && graph.nRecipe == 4, "reach sample shape");
  expect(graph.isRealItem(3) && !graph.isRealItem(4), "handle 5 is a pseudo-resource");

  const aw::Handle allowed[] = {3};  // -> item node 2
  aw::Subgraph sub = aw::reachableSubgraph(1, allowed);

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

void testRejectsBadInput() {
  std::cout << "[Test] malformed input\n";
  auto rejects = [](std::span<const std::byte> bytes) {
    aw::registerCraftingGraph(bytes);
    return aw::getCraftingError() != nullptr;
  };

  expect(rejects({}), "empty blob");
  const std::vector<std::byte> wrongMagic = {std::byte{'X'}, std::byte{'W'}, std::byte{'R'},
                         std::byte{1}, std::byte{0}, std::byte{0}};
  expect(rejects(wrongMagic), "bad magic");

  std::vector<std::byte> truncated = buildSample();
  truncated.resize(truncated.size() - 1);
  expect(rejects(truncated), "truncated");

  std::vector<std::byte> trailing = buildSample();
  trailing.push_back(std::byte{0});
  expect(rejects(trailing), "trailing byte");

  auto zeroDelta = buildSample();
  // This is the first output delta. Handle stays 0.
  zeroDelta[6] = std::byte{0};
  expect(rejects(zeroDelta), "handle 0 output");

  // A workstation must name a real resource (handle <= realResourceCount).
  std::vector<std::byte> badStation = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{1}};
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
  expect(graph.nReal == 2 && graph.nItem == 3 && graph.nRecipe == 3,
         "rejected blob leaves the previous graph in place");
  const auto itemTargets = graph.i2r.targetsOf(0);
  expect(itemTargets.size() == 2 && itemTargets[0] == 3 && itemTargets[1] == 4,
         "previous graph is still coherent after a rejected blob");

  // A failure must not poison the next attempt with a stale error.
  const std::vector<std::byte> bad = {std::byte{'X'}};
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
    std::vector<aw::Amount> inventory(nItem, 0);
    inventory[1] = 10;  // handle 2 (stained glass)
    const aw::Subgraph sub = aw::reachableSubgraph(5, all, inventory);
    expect(keepsStainedEdge(sub), "inventory keeps the dominated tag edge");
  }
  {
    // The A/B switch turns the drop off entirely.
    aw::setTagPruningEnabled(false);
    const aw::Subgraph sub = aw::reachableSubgraph(5, all);
    aw::setTagPruningEnabled(true);
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

void testTagPruningParity() {
  std::cout << "[Test] tag pruning preserves the optimum\n";
  aw::registerCraftingGraph(buildGlassSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5};

  // Leaves are free only via inventory, so stock every item with no recipe.
  std::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::NodeId m = 0; m < graph.nReal; m++)
    if (graph.i2r.targetsOf(m).empty())
      inventory[m] = 1000000000ULL;

  auto plan = [&](bool prune) {
    aw::setTagPruningEnabled(prune);
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
  aw::setTagPruningEnabled(true);
  expect(full.first == aw::PlanStatus::OK && pruned.first == aw::PlanStatus::OK,
         "both plan variants are feasible");
  expect(full.second == pruned.second,
         "pruning does not change the optimum");
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
    std::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
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
    // The A/B switch turns the drop off entirely.
    const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
    aw::setRecipePruningEnabled(false);
    const aw::Subgraph sub = aw::reachableSubgraph(4, all);
    aw::setRecipePruningEnabled(true);
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
  // WS_A. A player holding WS_A must keep R.
  aw::registerCraftingGraph(buildWorkstationGuardSample(false));
  expect(aw::getCraftingError() == nullptr, "workstation guard sample parses");
  expect(aw::getCraftingGraph().recipeDominated[0] == 0,
         "a disjoint workstation set blocks composite domination");

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

  // BASE (handle 6 -> item node 5) is the only leaf; the player holds it.
  std::vector<aw::Amount> inventory(graph.nItem, 0);
  inventory[5] = 100;

  auto plan = [&](bool prune) {
    aw::setRecipePruningEnabled(prune);
    const aw::Subgraph sub = aw::reachableSubgraph(1, onlyA, inventory);
    const aw::NodeId target = sub.translate(0);
    const aw::PlanResult r = aw::planCrafting(sub, target, 1, inventory);
    int64_t total = 0;
    for (int64_t x : r.exec)
      total += x;
    return std::pair<aw::PlanStatus, int64_t>(r.status, total);
  };

  const auto full = plan(false);
  const auto pruned = plan(true);
  expect(full.first == aw::PlanStatus::OK, "the unpruned route plans on WS_A");
  expect(pruned.first == full.first, "workstation guard preserves the plan status");
  expect(pruned.second == full.second, "workstation guard preserves the optimum");
  aw::setRecipePruningEnabled(true);
}

void testRecipePruningParity() {
  std::cout << "[Test] composite pruning preserves the optimum\n";
  aw::registerCraftingGraph(buildBlackCandleSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};

  // Leaves (handles 5 and 6) are free only through inventory.
  std::vector<aw::Amount> inventory(graph.nItem, 0);
  for (aw::NodeId m = 0; m < graph.nReal; m++)
    if (graph.i2r.targetsOf(m).empty())
      inventory[m] = 1000000000LL;

  auto plan = [&](bool prune, aw::Amount amount) {
    aw::setRecipePruningEnabled(prune);
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
  aw::setRecipePruningEnabled(true);
}

void testReducedCostParity() {
  std::cout << "[Test] reduced-cost fixing preserves the plan optimum\n";
  aw::registerCraftingGraph(buildBlackCandleSample());
  const aw::CraftingGraph &graph = aw::getCraftingGraph();
  const aw::Handle all[] = {1, 2, 3, 4, 5, 6};

  // Leaves are free only through inventory.
  std::vector<aw::Amount> inventory(graph.nItem, 0);
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
    std::vector<aw::Amount> inventory(aw::getCraftingGraph().nItem, 0);
    inventory[3] = 10;  // nugget
    const aw::Subgraph sub = aw::reachableSubgraph(3, all, inventory);
    expect(subgraphHasRecipe(sub, 0), "stocked nugget keeps the ingot recipe");
  }

  aw::setPackPruningEnabled(false);
  aw::registerCraftingGraph(buildPackSample());
  {
    const aw::CraftingGraph &graph = aw::getCraftingGraph();
    expect(graph.packDominated[0] == 0, "disabling the pass drops no recipe");
    const aw::Subgraph sub = aw::reachableSubgraph(3, all);
    expect(subgraphHasRecipe(sub, 0), "the recipe survives when disabled");
  }
  aw::setPackPruningEnabled(true);
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
    const std::vector<std::byte> bytes = buildPackSample();
    auto plan = [&](bool prune, aw::Amount amount) {
      aw::setPackPruningEnabled(prune);
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4};
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      std::vector<aw::Amount> inventory(graph.nItem, 0);
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
    const std::vector<std::byte> bytes = buildPackBranchSample();
    aw::setPackPruningEnabled(true);
    aw::registerCraftingGraph(bytes);
    expect(aw::getCraftingGraph().packDominated[0] == 0,
           "a split-production branch yields no certificate");

    auto plan = [&](bool prune, aw::Amount amount) {
      aw::setPackPruningEnabled(prune);
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4};
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      std::vector<aw::Amount> inventory(graph.nItem, 0);
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
    const std::vector<std::byte> bytes = buildBlackCandleSample();
    auto plan = [&](bool prune, aw::Amount amount) {
      aw::setPackPruningEnabled(prune);
      aw::registerCraftingGraph(bytes);
      const aw::Handle all[] = {1, 2, 3, 4, 5, 6};
      const aw::CraftingGraph &graph = aw::getCraftingGraph();
      std::vector<aw::Amount> inventory(graph.nItem, 0);
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

  aw::setPackPruningEnabled(true);
}

}  // namespace

int main() {
  testSample();
  testRejectsBadInput();
  testReachability();
  testSolver();
  testReducedCostFixing();
  testPlan();
  testPlanInfeasible();
  testDuplicateRecipes();
  testTagPruning();
  testTagPruningParity();
  testRecipePruning();
  testRecipePruningWorkstations();
  testRecipePruningParity();
  testReducedCostParity();
  testPackPruning();
  testPackPruningParity();

  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cout << failures << " check(s) failed\n";
  return 1;
}
