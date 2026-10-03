// One Tarjan SCC pass, shared by every pass that needs components. See
// aw/utils/Scc.h for the contract.

#include "aw/utils/Scc.h"

#include "aw/utils/LargeStackCall.h"
#include "aw/utils/PodVector.h"

namespace aw {
namespace {

// The scratch of a single pass, plus the graph and the output it walks. Held
// separately so the recursion below takes one reference rather than a handful
// of spans and counters.
struct SccState {
  std::span<const uint32_t> offsets;
  std::span<const uint32_t> targets;
  std::span<int32_t> component;  // written for every reached node
  aw::vector<int32_t> index;     // DFS number, -1 while unvisited
  aw::vector<int32_t> low;       // lowest DFS number reachable, itself included
  aw::vector<uint8_t> onStack;   // 0/1: currently on the component stack
  aw::vector<uint32_t> stack;    // Tarjan's stack of open nodes
  int32_t timer = 0;
  int32_t nComponent = 0;
};

// The textbook recursive Tarjan step: number v, walk its out-edges, then close
// v's component when nothing below it reaches above it. Runs on the large
// stack, so the depth is bounded by the graph rather than by the thread.
void sccVisit(SccState &state, uint32_t v) noexcept {
  state.index[v] = state.low[v] = state.timer++;
  state.onStack[v] = 1;
  state.stack.push_back_unchecked(v);

  for (uint32_t e = state.offsets[v]; e < state.offsets[v + 1]; e++) {
    const uint32_t w = state.targets[e];
    if (state.index[w] < 0) {
      sccVisit(state, w);
      if (state.low[w] < state.low[v])
        state.low[v] = state.low[w];
    } else if (state.onStack[w]) {
      if (state.index[w] < state.low[v])
        state.low[v] = state.index[w];
    }
  }

  if (state.low[v] == state.index[v]) {
    for (;;) {
      const uint32_t w = state.stack.back();
      state.stack.pop_back();
      state.onStack[w] = 0;
      state.component[w] = state.nComponent;
      if (w == v)
        break;
    }
    state.nComponent++;
  }
}

void sccVisitRoots(SccState &state, std::span<const uint32_t> roots) noexcept {
  for (uint32_t root : roots)
    if (state.index[root] < 0)
      sccVisit(state, root);
}

void sccVisitAll(SccState &state) noexcept {
  const uint32_t n = (uint32_t) state.index.size();
  for (uint32_t v = 0; v < n; v++)
    if (state.index[v] < 0)
      sccVisit(state, v);
}

// Allocate the scratch for n nodes. The output array is not cleared: a node the
// walk never reaches must keep whatever the caller put there.
SccState makeState(std::span<const uint32_t> offsets,
                   std::span<const uint32_t> targets,
                   std::span<int32_t> component) noexcept {
  SccState state;
  state.offsets = offsets;
  state.targets = targets;
  state.component = component;
  const uint32_t n = (uint32_t) component.size();
  state.index.assign(n, -1);
  state.low.assign(n, 0);
  state.onStack.assign(n, 0);
  state.stack.reserve(n);
  return state;
}

}  // namespace

uint32_t computeSccs(std::span<const uint32_t> offsets,
                     std::span<const uint32_t> targets,
                     std::span<int32_t> component) noexcept {
  SccState state = makeState(offsets, targets, component);
  // Entered on the large stack.
  aw::ls::call(sccVisitAll, state);
  return (uint32_t) state.nComponent;
}

uint32_t computeSccs(std::span<const uint32_t> offsets,
                     std::span<const uint32_t> targets,
                     std::span<const uint32_t> roots,
                     std::span<int32_t> component) noexcept {
  SccState state = makeState(offsets, targets, component);
  // Entered on the large stack.
  aw::ls::call(sccVisitRoots, state, roots);
  return (uint32_t) state.nComponent;
}

}  // namespace aw
