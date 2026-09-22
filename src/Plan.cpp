#include "aw/Plan.h"
#include "aw/LpSolver.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace aw {
namespace {

struct ColumnEntry {
  uint32_t row;
  double value;
};

}  // namespace

std::vector<Amount> subgraphInventory(const Subgraph &sub,
                                      std::span<const Amount> bySourceNode) {
  std::vector<Amount> inventory(sub.graph.nItem, 0);
  for (uint32_t i = 0; i < sub.graph.nItem; i++) {
    const NodeId source = sub.itemOrigin[i];
    if (source < bySourceNode.size())
      inventory[i] = bySourceNode[source];
  }
  return inventory;
}

// The LP is
//
//   minimize    sum_r x_r
//   subject to  produced(i) - consumed(i) >= b_i        for every item i
//               x_r >= 0
//
// with b_target = amount and b_i = -inventory(i) for every other item. The
// target inventory deliberately does not reduce b_target: the caller always
// gets `amount` freshly produced units. Recipes may still consume the target,
// so cycles that run through the output keep working.
//
// `inventoryBySourceNode` is indexed by source item node (handle - 1); entries
// past the end count as zero. An empty span means an empty inventory.
PlanResult planCrafting(const Subgraph &sub, NodeId target, Amount amount,
                        std::span<const Amount> inventoryBySourceNode) {
  PlanResult result;

  const BaseCraftingGraph &g = sub.graph;
  if (target >= g.nItem)
    return result;

  const uint32_t n = g.nRecipe;
  const uint32_t m = g.nItem;

  // One balance row per item:
  //   produced(i) - consumed(i) >= b_i
  // A recipe r adds +outputAmt at its output row and -inputAmt at each input
  // row.  Inputs are ascending in the subgraph, but the output row may collide
  // with one of them, so merge equal rows.
  lp::Matrix A;
  A.rows = m;
  A.cols = n;
  A.colStart.reserve((size_t) n + 1);
  A.colStart.push_back(0);
  std::vector<ColumnEntry> column;
  for (uint32_t r = 0; r < n; r++) {
    column.clear();
    column.push_back({g.output[r], (double) g.outputAmt[r]});
    const auto inputs = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      column.push_back({inputs[k], -(double) weights[k]});

    std::sort(column.begin(), column.end(),
              [](const ColumnEntry &lhs, const ColumnEntry &rhs) {
                return lhs.row < rhs.row;
              });
    size_t kept = 0;
    for (size_t k = 0; k < column.size();) {
      const uint32_t row = column[k].row;
      double value = 0.0;
      while (k < column.size() && column[k].row == row) {
        value += column[k].value;
        k++;
      }
      if (value != 0.0) {
        column[kept].row = row;
        column[kept].value = value;
        kept++;
      }
    }
    for (size_t k = 0; k < kept; k++) {
      A.rowIndex.push_back(column[k].row);
      A.value.push_back(column[k].value);
    }
    A.colStart.push_back((uint32_t) A.rowIndex.size());
  }

  std::vector<double> rhs(m, 0.0);
  for (uint32_t i = 0; i < m; i++) {
    if (i == target) {
      // Always produce `amount` new units; the target inventory is not
      // subtracted. Recipes may still consume the target, so cycles through
      // the output remain available.
      rhs[i] = (double) amount;
    } else {
      const NodeId source = sub.itemOrigin[i];
      Amount available = 0;
      if (source < inventoryBySourceNode.size())
        available = inventoryBySourceNode[source];
      rhs[i] = -(double) available;
    }
  }

  std::vector<double> objective(n, 1.0);

  const lp::Result solved = lp::solve(A, rhs, objective);
  result.status = solved.status;
  result.iterations = solved.iterations;
  if (solved.status == PlanStatus::OK)
    result.exec = solved.x;
  
  return result;
}

}  // namespace aw
