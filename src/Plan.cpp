#include "aw/Plan.h"
#include "aw/Solver.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace aw {
namespace {

struct ColumnEntry {
  uint32_t row;
  int64_t value;
};

}  // namespace

// The problem is
//
//   min   sum_{real r} x_r
//   s.t.  produced(i) - consumed(i) >= b_i        for every item i
//         x_r >= 0, x_r integer
//
// Here x_r is the number of times recipe `r` is executed. A tag edge (a recipe
// whose output is a synthetic tag node) costs nothing: choosing which member
// fills a tag slot is free in-game. It is given a natural upper bound by the
// solver instead -- it can never usefully exceed what the plan consumes of its
// tag -- so the free column stays finite under the `>=` row above.
//
// b_target = amount and b_i = -inventory(i) for every other item.
PlanResult planCrafting(const Subgraph &sub, NodeId target, Amount amount,
                        std::span<const Amount> invSrc,
                        const solver::Options &options) {
  PlanResult result;

  const BaseCraftingGraph &g = sub.graph;
  if (target >= g.nItem)
    return result;

  const uint32_t n = g.nRecipe;
  const uint32_t m = g.nItem;

  // One balance row per item:
  //   produced(i) - consumed(i) >= b_i
  // A recipe r adds +outputAmt at its output row and -inputAmt at each input
  // row. Inputs are ascending in the subgraph, but the output row may collide
  // with one of them, so merge equal rows.
  solver::Matrix A;
  A.rows = m;
  A.cols = n;
  A.colStart.reserve((size_t) n + 1);
  A.colStart.push_back(0);
  std::vector<ColumnEntry> column;
  // Note that our matrix is column-major, so we're processing column-by-column here.
  for (uint32_t r = 0; r < n; r++) {
    // Push "produced(i) - consumed(i)".
    column.clear();
    column.push_back({g.output[r], g.outputAmt[r]});
    const auto inputs = g.r2i.targetsOf(r);
    const auto weights = g.r2i.weightsOf(r);
    for (size_t k = 0; k < inputs.size(); k++)
      column.push_back({inputs[k], -weights[k]});

    std::sort(column.begin(), column.end(),
              [](const ColumnEntry &lhs, const ColumnEntry &rhs) {
                return lhs.row < rhs.row;
              });

    // Merge entries with the same row.
    size_t kept = 0;
    for (size_t k = 0; k < column.size();) {
      const uint32_t row = column[k].row;
      int64_t value = 0;
      while (k < column.size() && column[k].row == row) {
        value += column[k].value;
        k++;
      }
      if (value != 0) {
        column[kept].row = row;
        column[kept].value = value;
        kept++;
      }
    }

    // Push the column to A.
    for (size_t k = 0; k < kept; k++) {
      A.rowIndex.push_back(column[k].row);
      A.value.push_back(column[k].value);
    }
    A.colStart.push_back((uint32_t) A.rowIndex.size());
  }

  // Push b[i].
  std::vector<int64_t> rhs(m, 0);
  for (uint32_t i = 0; i < m; i++) {
    if (i == target) {
      // Always produce `amount` new units; the target inventory is not
      // subtracted. Recipes may still consume the target, so cycles through
      // the output remain available.
      rhs[i] = amount;
    } else {
      const NodeId source = sub.itemOrigin[i];
      const Amount available = source < invSrc.size() ? invSrc[source] : 0;
      rhs[i] = -available;
    }
  }

  // Tag edges are free. A recipe is a tag edge when it produces a non-real
  // node; those are exactly the synthetic member edges. Everything else costs
  // one crafting step. See include/aw/Solver.h for how the solver keeps the
  // free columns bounded without an equality.
  std::vector<int64_t> objective(n);
  for (uint32_t r = 0; r < n; r++)
    objective[r] = g.output[r] < g.nReal ? 1 : 0;

  const solver::Result solved = solver::solve(A, rhs, objective, options);
  result.status = solved.status;
  result.provenOptimal = solved.provenOptimal;
  result.gap = solved.gap;
  result.bestBound = solved.bestBound;
  result.numConflicts = solved.numConflicts;
  result.numBranches = solved.numBranches;
  result.fixedColumns = solved.fixedColumns;
  if (solved.status == PlanStatus::OK)
    result.exec = solved.x;

  return result;
}

}  // namespace aw
