// Unit tests for the .awr decoder and CSR construction. No test framework: a
// tiny assertion helper keeps the WSL build dependency-free.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

#include "aw/CraftingGraph.h"

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

}  // namespace

int main() {
  testSample();
  testRejectsBadInput();
  testReachability();

  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cout << failures << " check(s) failed\n";
  return 1;
}
