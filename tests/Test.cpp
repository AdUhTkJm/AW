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
//        workstations [2, 4])
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
    emitVarInt(out, 2);  // absolute first id
    emitVarInt(out, 2);  // +2 -> 4
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

  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cout << failures << " check(s) failed\n";
  return 1;
}
