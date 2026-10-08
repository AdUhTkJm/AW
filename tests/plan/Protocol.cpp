// Plan protocol tests
//
// The plan request/response blobs and byproduct reporting.

#include "plan/Harness.h"
#include "plan/Samples.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <utility>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"
#include "aw/plan/Protocol.h"
#include "AwrWriter.h"

namespace plan {

namespace {

// A sibling family whose tag is the way back in:
//
//   item 1 (A) <- r0 (x1, workstations [1], byproduct item 2 (B) x1, input item 3 (T) x1)
//   item 2 (B) <- r2 (x1, workstations [1], no inputs)
//   item 3 (T) <- r1 (x1, cost 0, input item 2 (B) x1)
//
// Handles 1..2 are real and item 3 is the synthetic tag, so r1 is a tag edge.
// B is reached twice: as r0's byproduct and as r1's input. Only the second
// visit may expand it, and only then is r2 -- the free producer that seeds the
// whole family -- part of the subgraph. Dropping it leaves a cycle that no
// stock can start, which the seed filter turns into a false INFEASIBLE.
aw::vector<std::byte> buildByproductRecycleSample() {
  AwrWriter w;
  w.header(2, 3);
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 1}}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {1}, {}, {});
  w.item(3, 1);
  w.recipe(1, {}, {}, {{2, 1}});
  return w.out;
}

// A genuine multi-output recipe in schema v2:
//
//   item 1 (A) <- r0 (x1, workstation [1], byproduct item 2 (B) x1, no inputs)
//
// r0 is the only producer of B, so requesting B must reach the recipe through
// its byproduct edge and the response must name B as a byproduct of r0.
aw::vector<std::byte> buildByproductSample() {
  AwrWriter w;
  w.header(2, 1);
  w.item(1, 1);
  w.recipe(1, {1}, {{2, 1}}, {});
  return w.out;
}

// A byproduct feeds the next recipe:
//
//   item 1 (A) <- r0 (x2, ws [1], byproduct item 2 (B) x1, input item 3 (C) x1)
//   item 2 (B) <- r1 (x1, ws [1], input item 1 (A) x1)
//   item 3 (C) is a raw leaf.
//
// With C in stock, r0 makes 2 A and one B, then r1 turns one A into the second
// B: cost two, and it proves the balance matrix credits the byproduct row.
aw::vector<std::byte> buildByproductChainSample() {
  AwrWriter w;
  w.header(3, 2);
  w.item(1, 1);
  w.recipe(2, {1}, {{2, 1}}, {{3, 1}});
  w.item(2, 1);
  w.recipe(1, {1}, {{1, 1}});
  return w.out;
}

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
  aw::vector<aw::Handle> byproducts;
  aw::vector<aw::Amount> byproductAmounts;
  aw::vector<aw::Handle> inputs;
  aw::vector<aw::Amount> inputAmounts;
  aw::vector<aw::Handle> workstations;
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
    const std::uint64_t byproducts = in.var();
    std::uint32_t byproductHandle = 0;
    for (std::uint64_t k = 0; k < byproducts && in.ok; k++) {
      byproductHandle += (std::uint32_t) in.var();
      use.byproducts.push_back(byproductHandle);
      use.byproductAmounts.push_back((aw::Amount) in.var());
    }
    const std::uint64_t inputs = in.var();
    std::uint32_t handle = 0;
    for (std::uint64_t k = 0; k < inputs && in.ok; k++) {
      handle += (std::uint32_t) in.var();
      use.inputs.push_back(handle);
      use.inputAmounts.push_back((aw::Amount) in.var());
    }
    const std::uint64_t stations = in.var();
    std::uint32_t station = 0;
    for (std::uint64_t k = 0; k < stations && in.ok; k++) {
      station += (std::uint32_t) in.var();
      use.workstations.push_back(station);
    }
    out.uses.push_back(std::move(use));
  }
  out.ok = in.ok && in.pos == blob.size();
  return out;
}

}  // namespace

AW_TEST(testPlanProtocol) {
  std::cout << "[Test] plan protocol\n";
  const aw::TagInlineMode savedInlining = aw::options.tagInlining;
  aw::options.tagInlining = aw::TagInlineMode::OFF;
  aw::registerCraftingGraph(buildPlanSample());
  expect(aw::getCraftingError() == nullptr, "protocol sample parses");

  std::string error;
  // One unit of item 1 seeds the two-item loop, so the recycling plan is both
  // optimal and executable. The target's stock does not change the balance.
  const auto request = encodePlanRequest(1, 4, {1, 2}, {{1, 1}});
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
    expect(decoded.uses[0].workstations.size() == 1 && decoded.uses[0].workstations[0] == 1,
           "r0 names its machine");
    expect(decoded.uses[1].count == 4 && decoded.uses[1].output == 2 &&
               decoded.uses[1].outputAmount == 2,
           "r1 produces item 2 four times");
    expect(decoded.uses[1].inputs.size() == 1 && decoded.uses[1].inputs[0] == 1 &&
               decoded.uses[1].inputAmounts[0] == 1,
           "r1 consumes item 1");
    expect(decoded.uses[1].workstations.size() == 1 && decoded.uses[1].workstations[0] == 2,
           "r1 names its machine");
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

AW_TEST(testByproducts) {
  std::cout << "[Test] multi-output recipes\n";
  aw::registerCraftingGraph(buildByproductSample());
  expect(aw::getCraftingError() == nullptr, "byproduct sample parses");

  std::string error;
  // Request the byproduct: r0 is reachable only through that edge.
  const auto response = aw::planBlob(encodePlanRequest(2, 1, {1}, {}), error);
  expect(error.empty(), "a byproduct request is accepted");

  const DecodedResponse decoded = decodePlanResponse(response);
  expect(decoded.ok, "the byproduct response decodes exactly");
  expect(decoded.status == (int) aw::PlanStatus::OK, "the byproduct plan is feasible");
  expect(decoded.uses.size() == 1, "one recipe is executed");
  if (decoded.uses.size() == 1) {
    expect(decoded.uses[0].count == 1 && decoded.uses[0].output == 1 &&
               decoded.uses[0].outputAmount == 1,
           "r0 anchors on item 1");
    expect(decoded.uses[0].byproducts.size() == 1 && decoded.uses[0].byproducts[0] == 2 &&
               decoded.uses[0].byproductAmounts[0] == 1,
           "r0 names item 2 as a byproduct");
    expect(decoded.uses[0].inputs.empty(), "r0 has no inputs");
  }

  // The anchor request reaches the same row, again with the byproduct attached.
  error.clear();
  const auto anchored = aw::planBlob(encodePlanRequest(1, 1, {1}, {}), error);
  const DecodedResponse anchoredDecoded = decodePlanResponse(anchored);
  expect(anchoredDecoded.ok && anchoredDecoded.uses.size() == 1,
         "the anchor request uses the same recipe");
  if (anchoredDecoded.uses.size() == 1)
    expect(anchoredDecoded.uses[0].byproducts.size() == 1 &&
               anchoredDecoded.uses[0].byproducts[0] == 2,
           "the anchor plan still reports the byproduct");

  // A byproduct consumed by a second recipe. Both rows of r0 appear in the
  // balance: the anchor (A x2) and the byproduct (B x1).
  aw::registerCraftingGraph(buildByproductChainSample());
  expect(aw::getCraftingError() == nullptr, "byproduct chain parses");
  error.clear();
  const auto chain = aw::planBlob(encodePlanRequest(2, 2, {1}, {{3, 1}}), error);
  expect(error.empty(), "the chain request is accepted");
  const DecodedResponse chainDecoded = decodePlanResponse(chain);
  expect(chainDecoded.ok && chainDecoded.status == (int) aw::PlanStatus::OK,
         "the byproduct chain plan is feasible");
  if (chainDecoded.ok && chainDecoded.status == (int) aw::PlanStatus::OK) {
    expect(chainDecoded.uses.size() == 2, "both recipes are executed");
    long total = 0;
    for (const DecodedUse& use : chainDecoded.uses)
      total += use.count;
    expect(total == 2, "the chain costs two executions");
  }
  // A byproduct that feeds back into the tag its own recipe consumes. The walk
  // has to expand B because r1 consumes it, even though r0 already produced it
  // as a byproduct: the subgraph then holds the free producer r2, and the
  // instance is a plain cost-2 plan instead of an unstartable cycle.
  aw::registerCraftingGraph(buildByproductRecycleSample());
  expect(aw::getCraftingError() == nullptr, "the recycling family parses");
  {
    const aw::Handle recycleAll[] = {1, 2};
    const aw::Subgraph recycleSub = aw::reachableSubgraph(1, recycleAll);
    expect(recycleSub.graph.nItem == 3 && recycleSub.graph.nRecipe == 3,
           "a byproduct a tag edge consumes still expands its own producers");
    expect(recycleSub.graph.nItem == 1 || recycleSub.graph.nRecipe != 0,
           "the recycling family keeps a seed");
  }
  error.clear();
  const auto recycle = aw::planBlob(encodePlanRequest(1, 1, {1, 2}, {}), error);
  expect(error.empty(), "the recycling request is accepted");
  const DecodedResponse recycleDecoded = decodePlanResponse(recycle);
  expect(recycleDecoded.ok && recycleDecoded.status == (int) aw::PlanStatus::OK,
         "the recycling family is feasible from an empty inventory");
  if (recycleDecoded.ok && recycleDecoded.status == (int) aw::PlanStatus::OK) {
    expect(recycleDecoded.uses.size() == 3, "all three recipes are executed");
    long recycleTotal = 0;
    for (const DecodedUse& use : recycleDecoded.uses)
      recycleTotal += use.count;
    expect(recycleTotal == 3, "two paid executions and the free tag edge");
  }
}

}  // namespace plan
