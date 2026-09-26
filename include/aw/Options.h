#ifndef AW_OPTIONS_H
#define AW_OPTIONS_H

namespace aw {

// Process-wide planner options. Minecraft is serial and the crafting graph is
// a singleton, so there is no thread safety to worry about. The registration
// time passes read these at registerCraftingGraph, so they must be set before
// it, exactly like the per-pass options in CraftingGraph.h.
struct Options {
  // Nonoptimal mode. On by default. Read at registration time by every
  // dominance pass.
  //
  // The pruning passes are sound (they never make the plan infeasible) but
  // they are only optimal because they reason about whole recipe executions: a
  // plan runs a recipe an integer number of times. This flag drops that
  // assumption, which lets the passes mark more edges and recipes dominated,
  // so the query-time subgraph is smaller and the solver has less to do. The
  // price is that a plan can be feasible but suboptimal (docs/algorithm.typ,
  // the black glass example). Disable this when the optimum has to be exact,
  // e.g. in the unit tests.
  //
  // Concretely, it drops three integrality guards and generalizes the tag
  // relation:
  //
  //   * the tag pass keeps a tag edge `T <- m` whenever some recipe of m emits
  //     more than one unit at a time, because the batch surplus is a free way
  //     to satisfy T ("Batching guard" in Prune.cpp). Nonoptimal mode skips
  //     that guard;
  //   * the tag pass computes the domination relation one step at a time when
  //     the flag is off: a real input j only satisfies `dominate(m, w)` when
  //     j == w, and a tag input only when w is one of its members. Nonoptimal
  //     mode takes the transitive closure instead, chaining through real
  //     inputs (`dominate(j, w)`) and through every member of a consumed tag
  //     (`dominate(z, w)` for all z). This is what captures families such as
  //     EnderIO's fused quartz, where `fused_quartz_a` is only reachable from
  //     `fused_quartz` through a tag of its own variants;
  //   * the composite pass inlines a producer of the witness Y with
  //     `alpha = ceil(q / p)`, because running it once may overshoot the q
  //     units of Y that R actually consumes. Nonoptimal mode floors it;
  //   * the direct pass compares raw columns, so one of its executions must be
  //     replaceable by one of the dominator. Nonoptimal mode compares columns
  //     after normalizing the output amount to 1, which can drop a slow but
  //     material-cheap recipe in favor of a fast but costly one.
  bool nonoptimal = true;
};

// The singleton option block.
Options &options() noexcept;

}  // namespace aw

#endif
