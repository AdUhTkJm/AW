#ifndef AW_TEST_AWR_WRITER_H
#define AW_TEST_AWR_WRITER_H

// Test helper: a semantic writer for the .awr blob format, so the samples in
// Test.cpp read as graphs (header, items, recipes) instead of raw varint
// streams. Mirrors what the mod's Java writer emits.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "aw/utils/PodVector.h"

inline void emitVarInt(aw::vector<std::byte> &out, std::uint64_t value) {
  while ((value & ~0x7FULL) != 0) {
    out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<std::byte>(value));
}

// Compact writer for the samples in Test.cpp. Handles must be emitted in
// ascending order, exactly as the Java writer does. `ws` and `inputs` are in
// ascending handle order; the writer stores deltas. A real recipe with no
// workstation is dropped by registerCraftingGraph, so every real recipe here
// names one.
//
// Emits schema v3 by default, so every existing sample also exercises the
// byproduct- and cost-aware parser. A recipe costs 1 unless `cost()` preceded
// it, which is what an ordinary recipe costs; a batched level of a chanced
// recipe is what actually spells something else. `version()` downgrades the
// writer so the compatibility path (a v1/v2 blob, where every real recipe costs
// one execution and every pseudo-resource is free) stays covered; v1 has no
// byproduct list, so a byproduct passed to a v1 writer is dropped.
struct AwrWriter {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{3}};
  std::uint32_t previousHandle = 0;
  // Cost of the recipe written next, in underlying recipe executions. Reset to
  // 1 by `recipe`, so only the recipes that spell a cost carry one.
  std::int64_t pendingCost = 1;
  std::uint8_t schema = 3;

  // Write the older layouts, which the parser still accepts. The cost is not
  // part of them and is left out.
  void version(std::uint8_t value) {
    schema = value;
    out[3] = static_cast<std::byte>(value);
  }

  void header(std::uint32_t nReal, std::uint32_t entries) {
    emitVarInt(out, nReal);
    emitVarInt(out, entries);
  }

  void item(std::uint32_t handle, std::uint32_t recipes) {
    emitVarInt(out, handle - previousHandle);
    previousHandle = handle;
    emitVarInt(out, recipes);
  }

  // Cost of the recipe written next. One execution of a chanced recipe's batch
  // level is a whole batch of underlying executions, so it costs that many
  // steps; see BaseCraftingGraph::cost.
  void cost(std::int64_t value) { pendingCost = value; }

  void recipe(std::int64_t amount, std::initializer_list<std::uint32_t> ws,
              std::initializer_list<std::pair<std::uint32_t, std::int64_t>> inputs) {
    recipe(amount, ws, {}, inputs);
  }

  void recipe(std::int64_t amount, std::initializer_list<std::uint32_t> ws,
              std::initializer_list<std::pair<std::uint32_t, std::int64_t>> byproducts,
              std::initializer_list<std::pair<std::uint32_t, std::int64_t>> inputs) {
    if (schema >= 3) {
      emitVarInt(out, (std::uint64_t) pendingCost);
      pendingCost = 1;
    }
    emitVarInt(out, (std::uint64_t) amount);
    emitVarInt(out, ws.size());
    std::uint32_t previous = 0;
    for (std::uint32_t w : ws) {
      emitVarInt(out, w - previous);
      previous = w;
    }
    if (schema >= 2) {
      emitVarInt(out, byproducts.size());
      previous = 0;
      for (const auto& [handle, byproductAmount] : byproducts) {
        emitVarInt(out, (std::uint64_t) byproductAmount);
        emitVarInt(out, handle - previous);
        previous = handle;
      }
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

#endif
