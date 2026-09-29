#include "aw/plan/Protocol.h"

#include <atomic>
#include <cstring>

#include "aw/plan/Options.h"
#include "aw/plan/Plan.h"

namespace aw {

namespace {

std::atomic<int> kernelStatus{(int) KernelStatus::OK};

// The request magic is 'A' 'W' 'P' 1, the response magic 'A' 'W' 'Q' 1.
constexpr uint8_t requestMagic[4] = {'A', 'W', 'P', 1};
constexpr uint8_t responseMagic[4] = {'A', 'W', 'Q', 1};

// LEB128 reader over a byte span. Every method fails rather than throwing, so
// the whole decoder can stay exception-free.
class Reader {
  std::span<const std::byte> bytes;
  size_t pos = 0;

public:
  explicit Reader(std::span<const std::byte> bytes) noexcept: bytes(bytes) {}

  [[nodiscard]]
  bool atEnd() const noexcept {
    return pos >= bytes.size();
  }

  [[nodiscard]]
  size_t remaining() const noexcept {
    return bytes.size() - pos;
  }

  bool readByte(uint8_t &value) noexcept {
    if (atEnd())
      return false;
    value = std::to_integer<uint8_t>(bytes[pos++]);
    return true;
  }

  // Rejects a group whose payload does not fit in the remaining bits, so a
  // crafted blob cannot wrap a value into a different one.
  bool readVarUInt(unsigned maxBytes, uint64_t &value) noexcept {
    value = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < maxBytes; i++) {
      uint8_t byte;
      if (!readByte(byte))
        return false;
      const uint64_t payload = (uint64_t) (byte & 0x7F);
      if (payload > (UINT64_MAX >> shift))
        return false;
      value |= payload << shift;
      if ((byte & 0x80) == 0)
        return true;
      shift += 7;
    }
    return false;
  }

  bool readVarInt(uint32_t &value) noexcept {
    uint64_t wide = 0;
    if (!readVarUInt(5, wide) || wide > UINT32_MAX)
      return false;
    value = (uint32_t) wide;
    return true;
  }

  bool readVarLong(int64_t &value) noexcept {
    uint64_t wide = 0;
    if (!readVarUInt(10, wide) || wide > (uint64_t) INT64_MAX)
      return false;
    value = (int64_t) wide;
    return true;
  }
};

void writeVarUInt(aw::vector<std::byte> &out, uint64_t value) noexcept {
  while ((value & ~0x7FULL) != 0) {
    out.push_back((std::byte) ((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back((std::byte) value);
}

// Little-endian raw bits. The Java side reads it back with
// Double.longBitsToDouble on a little-endian ByteBuffer, so no decimal
// formatting is involved and the value is bit-exact.
void writeF64(aw::vector<std::byte> &out, double value) noexcept {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  for (unsigned i = 0; i < 8; i++)
    out.push_back((std::byte) ((bits >> (8 * i)) & 0xFF));
}

bool readMagic(Reader &in, const uint8_t (&magic)[4]) noexcept {
  for (uint8_t expected : magic) {
    uint8_t found = 0;
    if (!in.readByte(found) || found != expected)
      return false;
  }
  return true;
}

// Handles are 1-based, emitted as ascending deltas so a request stays small.
// A zero delta is rejected: for the first entry it would name handle 0, and for
// a later one it would repeat the previous handle.
bool readHandleList(Reader &in, aw::vector<Handle> &out, uint32_t count,
                    const char *what, std::string &error) noexcept {
  out.clear();
  // Every entry costs at least one byte, so the remaining byte count bounds how
  // many can be present. This is what keeps a corrupt count from asking for a
  // huge reservation.
  if (count > in.remaining()) {
    error = std::string("truncated ") + what + " list";
    return false;
  }
  out.reserve(count);
  uint32_t handle = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t delta = 0;
    if (!in.readVarInt(delta)) {
      error = std::string("truncated ") + what + " handle";
      return false;
    }
    if (delta == 0) {
      error = std::string(what) + " handles must be ascending and unique";
      return false;
    }
    if (UINT32_MAX - handle < delta) {
      error = std::string(what) + " handle overflows";
      return false;
    }
    handle += delta;
    out.push_back_unchecked(handle);
  }
  return true;
}

}  // namespace

KernelStatus getKernelStatus() noexcept {
  return (KernelStatus) kernelStatus.load(std::memory_order_relaxed);
}

void setKernelStatus(KernelStatus status) noexcept {
  kernelStatus.store((int) status, std::memory_order_relaxed);
}

bool decodePlanRequest(std::span<const std::byte> bytes, PlanRequest &out,
                       std::string &error) noexcept {
  Reader in(bytes);
  out = PlanRequest{};

  if (!readMagic(in, requestMagic)) {
    error = "not an Applied Wheelchair plan request";
    return false;
  }
  if (!in.readVarLong(out.amount) || out.amount <= 0) {
    error = "the plan amount must be positive";
    return false;
  }
  if (!in.readVarInt(out.target) || out.target == 0) {
    error = "the plan target must be a positive handle";
    return false;
  }

  uint32_t workstations = 0;
  if (!in.readVarInt(workstations)) {
    error = "truncated workstation count";
    return false;
  }
  if (!readHandleList(in, out.workstations, workstations, "workstation", error))
    return false;

  uint32_t stock = 0;
  if (!in.readVarInt(stock)) {
    error = "truncated stock count";
    return false;
  }
  // Each stock entry costs at least a handle delta plus an amount, so the
  // remaining byte count bounds how many can be present. This is what keeps a
  // corrupt count from asking for a huge reservation.
  if ((size_t) stock * 2 > in.remaining()) {
    error = "truncated stock list";
    return false;
  }
  out.stockHandles.clear();
  out.stockAmounts.clear();
  out.stockHandles.reserve(stock);
  out.stockAmounts.reserve(stock);
  {
    uint32_t handle = 0;
    for (uint32_t i = 0; i < stock; i++) {
      uint32_t delta = 0;
      if (!in.readVarInt(delta)) {
        error = "truncated stock handle";
        return false;
      }
      if (delta == 0) {
        error = "stock handles must be ascending and unique";
        return false;
      }
      if (UINT32_MAX - handle < delta) {
        error = "stock handle overflows";
        return false;
      }
      handle += delta;
      int64_t amount = 0;
      if (!in.readVarLong(amount)) {
        error = "truncated stock amount";
        return false;
      }
      out.stockHandles.push_back_unchecked(handle);
      out.stockAmounts.push_back_unchecked(amount);
    }
  }

  if (!in.atEnd()) {
    error = "trailing bytes after the plan request";
    return false;
  }
  return true;
}

PlanResponse runPlan(const PlanRequest &request) noexcept {
  PlanResponse response;

  const CraftingGraph &graph = getCraftingGraph();
  if (graph.nItem == 0 || request.target > graph.nItem)
    return response;

  // The planner wants a dense, source-item-indexed stock vector. The request
  // carries it sparsely because a player holds a handful of stacks, not 12k
  // resources.
  aw::vector<Amount> inventory = aw::vector<Amount>::zeroes(graph.nItem);
  for (size_t i = 0; i < request.stockHandles.size(); i++) {
    const Handle handle = request.stockHandles[i];
    if (handle >= 1 && handle <= graph.nItem)
      inventory[CraftingGraph::itemNode(handle)] += request.stockAmounts[i];
  }

  const Subgraph sub =
      reachableSubgraph(request.target, request.workstations, inventory);
  if (sub.graph.nItem == 0)
    return response;

  const NodeId target = sub.translate(CraftingGraph::itemNode(request.target));
  if (target == UINT32_MAX)
    return response;

  const PlanResult plan =
      planCrafting(sub, target, request.amount, inventory, solverOptions);
  response.status = plan.status;
  response.provenOptimal = plan.provenOptimal;
  response.gap = plan.gap;
  response.bestBound = plan.bestBound;
  response.numConflicts = plan.numConflicts;
  response.numBranches = plan.numBranches;
  response.fixedColumns = plan.fixedColumns;

  // A solver failure leaves `exec` empty; anything else is a full assignment,
  // one entry per subgraph recipe.
  if (plan.exec.size() != sub.graph.nRecipe)
    return response;

  // Translate the subgraph indices back into source handles here, where
  // itemOrigin is still in scope. Only the recipes the plan executes are
  // written out: the solver returns one value per subgraph recipe and most are
  // zero.
  response.uses.reserve(sub.graph.nRecipe / 4 + 1);
  for (uint32_t r = 0; r < sub.graph.nRecipe; r++) {
    const Amount count = plan.exec[r];
    if (count <= 0)
      continue;

    PlanRecipeUse use;
    use.count = count;
    use.outputHandle = CraftingGraph::itemHandle(sub.itemOrigin[sub.graph.output[r]]);
    use.outputAmount = sub.graph.outputAmt[r];

    const auto inputs = sub.graph.r2i.targetsOf(r);
    const auto amounts = sub.graph.r2i.weightsOf(r);
    use.inputHandles.reserve(inputs.size());
    use.inputAmounts.reserve(inputs.size());
    for (size_t k = 0; k < inputs.size(); k++) {
      use.inputHandles.push_back_unchecked(CraftingGraph::itemHandle(sub.itemOrigin[inputs[k]]));
      use.inputAmounts.push_back_unchecked(amounts[k]);
    }
    response.uses.push_back(std::move(use));
  }
  return response;
}

aw::vector<std::byte> encodePlanResponse(const PlanResponse &response) noexcept {
  aw::vector<std::byte> out = {
      (std::byte) responseMagic[0], (std::byte) responseMagic[1],
      (std::byte) responseMagic[2], (std::byte) responseMagic[3]};

  writeVarUInt(out, (uint64_t) response.status);
  out.push_back((std::byte) (response.provenOptimal ? 1 : 0));
  writeF64(out, response.gap);
  writeF64(out, response.bestBound);
  writeVarUInt(out, (uint64_t) response.numConflicts);
  writeVarUInt(out, (uint64_t) response.numBranches);
  writeVarUInt(out, response.fixedColumns);

  writeVarUInt(out, (uint64_t) response.uses.size());
  for (const PlanRecipeUse &use : response.uses) {
    writeVarUInt(out, (uint64_t) use.count);
    writeVarUInt(out, use.outputHandle);
    writeVarUInt(out, (uint64_t) use.outputAmount);

    writeVarUInt(out, (uint64_t) use.inputHandles.size());
    uint32_t previous = 0;
    for (size_t k = 0; k < use.inputHandles.size(); k++) {
      writeVarUInt(out, use.inputHandles[k] - previous);
      previous = use.inputHandles[k];
      writeVarUInt(out, (uint64_t) use.inputAmounts[k]);
    }
  }
  return out;
}

aw::vector<std::byte> planBlob(std::span<const std::byte> request,
                               std::string &error) noexcept {
  PlanRequest decoded;
  if (!decodePlanRequest(request, decoded, error))
    return {};

  const CraftingGraph &graph = getCraftingGraph();
  if (graph.nItem == 0) {
    error = "no crafting graph has been registered";
    return {};
  }
  if (decoded.target > graph.nItem) {
    error = "the plan target is outside the registered graph";
    return {};
  }
  // reachableSubgraph ignores handles it does not know and planCrafting ignores
  // stock it does not know, which would quietly turn a Java-side bug into a
  // plausible-looking plan. Reject them instead.
  for (Handle handle : decoded.workstations) {
    if (handle == 0 || handle > graph.nItem) {
      error = "a workstation handle is outside the registered graph";
      return {};
    }
  }
  for (Handle handle : decoded.stockHandles) {
    if (handle == 0 || handle > graph.nItem) {
      error = "a stock handle is outside the registered graph";
      return {};
    }
  }

  const KernelStatusGuard guard(KernelStatus::PLANNING);
  return encodePlanResponse(runPlan(decoded));
}

}  // namespace aw
