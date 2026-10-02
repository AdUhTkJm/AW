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
// Emits schema v2 by default, so every existing sample also exercises the
// byproduct-aware parser; the three-argument `recipe` writes an empty
// byproduct list.
struct AwrWriter {
  aw::vector<std::byte> out = {std::byte{'A'}, std::byte{'W'}, std::byte{'R'}, std::byte{2}};
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
    recipe(amount, ws, {}, inputs);
  }

  void recipe(std::int64_t amount, std::initializer_list<std::uint32_t> ws,
              std::initializer_list<std::pair<std::uint32_t, std::int64_t>> byproducts,
              std::initializer_list<std::pair<std::uint32_t, std::int64_t>> inputs) {
    emitVarInt(out, (std::uint64_t) amount);
    emitVarInt(out, ws.size());
    std::uint32_t previous = 0;
    for (std::uint32_t w : ws) {
      emitVarInt(out, w - previous);
      previous = w;
    }
    emitVarInt(out, byproducts.size());
    previous = 0;
    for (const auto& [handle, byproductAmount] : byproducts) {
      emitVarInt(out, (std::uint64_t) byproductAmount);
      emitVarInt(out, handle - previous);
      previous = handle;
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
