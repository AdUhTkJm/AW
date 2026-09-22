#ifndef AW_PLAN_H
#define AW_PLAN_H

#include <cstdint>
#include <span>
#include <vector>

#include "aw/CraftingGraph.h"
#include "aw/Status.h"

namespace aw {

struct PlanResult {
  PlanStatus status = PlanStatus::INVALID_INPUT;

  // Executions per recipe, indexed by subgraph recipe id. 
  // Empty unless the LP reached optimality.
  std::vector<double> exec;

  // Number of simplex pivots executed, for profiling usage.
  uint32_t iterations = 0;
};

// Gathers inventory from the source item-node space into the subgraph item
// node space. Entries past the end of `bySourceNode` count as zero.
std::vector<Amount> subgraphInventory(const Subgraph& sub,
                                      std::span<const Amount> bySourceNode);

// Plans `amount` new units of the item at subgraph node `target`.
PlanResult planCrafting(const Subgraph& sub, NodeId target, Amount amount,
                        std::span<const Amount> inventoryBySourceNode = {});

}  // namespace aw

#endif
