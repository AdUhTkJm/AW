#include "aw/CraftingGraph.h"

#include <array>

namespace aw {
CraftingGraph graph;
// We choose to first parse with errors and use default nodes.
// When there's an error at the end (which should be rare), we then hand it to Java.
const char *error;

namespace {

constexpr std::array<uint8_t, 4> kMagic = {'A', 'W', 'R', 0x01};

#define fail(msg, ...) { error = msg; return __VA_ARGS__; }

using uint = uint32_t;
using ulong = uint64_t;

class ByteReader {
  std::span<const std::byte> bytes;
  size_t pos = 0;
public:
  explicit ByteReader(std::span<const std::byte> bytes) noexcept: bytes(bytes) {}

  [[nodiscard]]
  bool atEnd() const noexcept { 
    return pos >= bytes.size();
  }

  [[nodiscard]]
  size_t position() const noexcept {
    return pos;
  }

  uint8_t readByte() {
    if (atEnd())
      return 0;

    return std::to_integer<uint8_t>(bytes[pos++]);
  }

  void skip(size_t count) {
    if (count > bytes.size() - pos)
      return;

    pos += count;
  }

  uint readVarInt() {
    uint value = 0;
    for (size_t i = 0; i < 5; ++i) {
      uint8_t next = readByte();
      value |= ((uint) (next & 0x7F)) << (7 * i);
      if ((next & 0x80) == 0)
        return value;
    }
    fail("varint longer than 5 bytes", 0);
  }

  ulong readVarLong() {
    ulong value = 0;
    for (size_t i = 0; i < 10; ++i) {
      uint8_t next = readByte();
      if (i == 9 && (next & 0xFE) != 0)
        fail("varlong overflows 64 bits", 0);

      value |= ((ulong) (next & 0x7F)) << (7 * i);
      if ((next & 0x80) == 0)
        return value;
    }
    fail("varlong longer than 10 bytes", 0);
  }

  uint readHandle() {
    uint handle = readVarInt();
    if (handle == 0)
      fail("resource handle 0 is invalid", 0);
    return handle;
  }
};

void checkMagic(ByteReader& in) {
  for (std::uint8_t expected : kMagic) {
    if (in.readByte() != expected)
      fail("invalid magic bits");
  }
}

void readHeader(ByteReader& in, uint& realResourceCount, uint& outputCount) {
  realResourceCount = in.readVarInt();
  outputCount = in.readVarInt();
}

void skipWorkstations(ByteReader& in) {
  uint count = in.readVarInt();
  if (count == 0)
    return;
  in.readHandle();
  for (uint i = 1; i < count; ++i)
    in.readVarInt();
}

// Everything the first pass learns, used to size the arrays the second pass
// fills. Recipe ids are handed out in file order.
struct Layout {
  uint nReal = 0;
  uint nOutput = 0;
  uint maxHandle = 0;
  // Maps `handle-1` to its number of recipes (recipes per item).
  std::vector<uint> rpi;
  // Maps `recipe` to its number of items (items per recipe).
  std::vector<uint> ipr;

  void noteHandle(uint handle) {
    if (handle <= maxHandle)
      return;

    maxHandle = handle;
    if (rpi.size() < handle)
      rpi.resize(handle, 0);
  }
};

// We do a 2-pass parsing.
// Here we need to confirm some parameters that shapes the graph.
Layout scan(std::span<const std::byte> bytes) {
  ByteReader in(bytes);
  Layout layout;

  // Check magic bits.
  checkMagic(in);

  // Check header.
  layout.nReal = in.readVarInt();
  layout.nOutput = in.readVarInt();

  uint output = 0;
  for (uint entry = 0; entry < layout.nOutput; entry++) {
    uint delta = in.readVarInt();
    if (delta == 0)
      fail("output handles must be strictly ascending", layout);
    if (output > UINT_MAX - delta)
      fail("output handle overflow", layout);
    
    output += delta;
    layout.noteHandle(output);

    uint nRecipe = in.readVarInt();
    layout.rpi[output - 1] += nRecipe;

    for (uint i = 0; i < nRecipe; i++) {
      in.readVarLong();  // output amount
      skipWorkstations(in);

      uint nInput = in.readVarInt();
      layout.ipr.push_back(nInput);

      uint input = 0;
      for (uint j = 0; j < nInput; j++) {
        in.readVarLong();  // input amount
        uint delta = in.readVarInt();
        if (input > UINT_MAX - delta) {
          fail("input handle overflow", layout);
        }
        input += delta;
        if (input == 0)
          fail("resource handle 0 is not valid", layout);
        layout.noteHandle(input);
      }
    }
  }

  if (!in.atEnd())
    fail("trailing bytes after the last entry", layout);
  return layout;
}

void prefixSum(std::vector<uint> &v) {
  v.push_back(0);
  for (uint i = 0; i < v.size(); i++)
    v[i + 1] += v[i];
}

}  // namespace

void registerCraftingGraph(std::span<const std::byte> bytes) noexcept {
  Layout layout = scan(bytes);

  uint nRecipe = layout.ipr.size();
  graph.nReal = layout.nReal;
  graph.nItem = layout.maxHandle;
  graph.nRecipe = nRecipe;

  prefixSum(layout.rpi);
  graph.i2r.offsets = std::move(layout.rpi);
  graph.i2r.targets.resize(nRecipe);
  graph.i2r.weights.resize(nRecipe);

  prefixSum(layout.ipr);
  graph.r2i.offsets = std::move(layout.ipr);
  graph.r2i.targets.resize(graph.r2i.offsets.back());
  graph.r2i.weights.resize(graph.r2i.offsets.back());

  graph.output.resize(nRecipe);
  graph.outputAmt.resize(nRecipe);

  // Pass 2: fill. `itemCursor` and `recipeCursor` track the next free
  // slot in each row, mirroring the offsets arrays.
  ByteReader in(bytes);
  checkMagic(in);
  uint nReal = in.readVarInt();
  uint nOutput = in.readVarInt();

  std::vector<uint> itemCursor(graph.i2r.offsets.begin(),
                      graph.i2r.offsets.end() - 1);
  std::vector<uint> recipeCursor(graph.r2i.offsets.begin(),
                      graph.r2i.offsets.end() - 1);

  uint output = 0;
  uint recipe = 0;
  for (uint entry = 0; entry < nOutput; entry++) {
    output += in.readVarInt();
    uint nRecipe = in.readVarInt();

    for (uint i = 0; i < nRecipe; ++i) {
      Amount outputAmt = in.readVarLong();
      skipWorkstations(in);
      uint nInput = in.readVarInt();

      graph.output[recipe] = output - 1;
      graph.outputAmt[recipe] = outputAmt;

      uint itemSlot = itemCursor[output - 1]++;
      graph.i2r.targets[itemSlot] = graph.recipeNode(recipe);
      graph.i2r.weights[itemSlot] = outputAmt;

      uint inputSlot = recipeCursor[recipe];
      uint input = 0;
      for (uint j = 0; j < nInput; j++) {
        Amount inputAmount = in.readVarLong();
        input += in.readVarInt();
        graph.r2i.targets[inputSlot] = input - 1;
        graph.r2i.weights[inputSlot] = inputAmount;
        inputSlot++;
      }

      recipe++;
    }
  }
}

const char *getCraftingError() noexcept {
  return error;
}

void clearCraftingError() noexcept {
  error = nullptr;
}

const CraftingGraph &getCraftingGraph() noexcept {
  return graph;
}

} // namespace aw
