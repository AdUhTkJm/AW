#ifndef AW_SOLVER_H
#define AW_SOLVER_H

// Integer solver, backed by Google OR-Tools CP-SAT.
//
// Solves
//
//   minimize    c^T x
//   subject to  A x >= b,  0 <= x
//
// over non-negative *integer* x. Sparse (CSC) integer matrix.
//
// This header deliberately keeps OR-Tools out of the include path: the
// hand-written code is compiled with -fno-exceptions -fno-rtti, while
// OR-Tools/Abseil/protobuf need both.

#include <cstdint>
#include <span>
#include <vector>

#include "aw/plan/Status.h"
#include "aw/utils/PodVector.h"

namespace aw::solver {

// Compressed sparse column integer matrix.
struct Matrix {
  uint32_t rows = 0;
  uint32_t cols = 0;
  aw::vector<uint32_t> colStart;  // cols + 1
  aw::vector<uint32_t> rowIndex;  // nnz
  aw::vector<int64_t> value;      // nnz

  Matrix() = default;
  Matrix(uint32_t rows, uint32_t cols): rows(rows), cols(cols) {}
};

struct Options {
  // The fraction by which the difference between returned and optimal solution is allowed.
  double relativeGap = 0.01;
  // Absolute gap floor. 0 leaves only the relative rule in force.
  int64_t absoluteGap = 0;

  // Wall-clock budget. <= 0 means no limit. On expiry, CP-SAT gives a feasible plan.
  double maxTimeSeconds = 2.0;

  // Search threads. 0 for auto.
  int numWorkers = 16;

  // Only observable with numWorkers == 1. CP-SAT's own default is 1.
  int randomSeed = 1;
  
  // Starting cap on the total objective, and therefore on every variable.
  // 0 derives one from a lower bound on the optimum. See implementation details.
  int64_t objectiveCap = 0;

  // Known-feasible objective value, from a heuristic pre-pass. 0 means none.
  // Unlike `objectiveCap` this does NOT replace the derived starting cap; it
  // only clamps it and bounds the cap-growth retries. A loose heuristic plan
  // can be orders of magnitude above the optimum, and seeding the variable
  // domains with it makes propagation blow up, so it is used purely as an
  // upper bound: a plan exists at this objective, hence no cap above it is
  // ever worth a retry.
  int64_t objectiveUpperBound = 0;

  // Reduced-cost fixing. The solver probes for any plan within this much of
  // the LP relaxation; if one exists, every column is bounded by
  // floor((incumbent - LP) / d_r), so columns whose bound is 0 are dropped and
  // the rest get a tightened domain before the final solve. 0 disables the
  // pass.
  //
  // Off by default. The probe is a second full CP-SAT solve (model load and
  // presolve, plus proving the tightened cap), which measured 0.5-1.0 s on the
  // recipe graphs. There the integer optimum sits far enough above the LP bound
  // that the probe is infeasible and fixes nothing, so the time is pure
  // overhead; enable it explicitly on instances where the optimum is known to
  // be close to the LP bound. The probe is also skipped while forbidden
  // assignments or startup entry cuts are present, because both are expressed
  // in the full column space and the probe solves a sub-model.
  double reducedCostGap = 0.0;

  // Optional warm start: one suggested value per column of the model handed to
  // `solve`. It is installed as a CP-SAT solution hint, so it guides the search
  // toward a known plan without constraining it. Values are clamped into the
  // column domain, and the hint is ignored entirely when its length does not
  // match the model being solved (for example after reduced-cost column
  // fixing). The planner fills this from its greedy DAG pre-pass.
  std::span<const int64_t> solutionHint;

  // Plans a post-solve check already rejected.
  //
  // Each entry is a full assignment, one value per column of the model handed
  // to `solve`, and is added as a CP-SAT forbidden-assignment table, so the
  // next solve cannot return that exact vector again. `planCrafting` appends
  // the plan its fireability check rejected and re-solves, up to
  // `maxCycleRetries` times. The entries are matched against the original
  // column space, so they stay valid across the cap retries inside one `solve`
  // call; a reduced-cost probe is skipped while they are present. See
  // docs/algorithm.typ, "启动切断".
  aw::vector<aw::vector<int64_t>> noGoods;

  // How many extra solves a caller may run after rejecting a plan. 0 turns the
  // retry off, which is the behaviour before no-goods existed.
  int maxCycleRetries = 3;

  // Return the first balance-feasible plan instead of a cheap one.
  //
  // This is the per-solve copy of the process-wide `aw::options.flash`;
  // `planCrafting` sets it from that switch and may clear it again. A first
  // incumbent is only useful while it is startable, so once the fireability
  // check rejects one, the retries run this flag off: the heuristic that put
  // the rejected plan there has failed, and an optimized incumbent is far more
  // likely to be realizable. Leaving it set is what made flash return
  // CYCLE_UNFULFILLED on instances the optimizing solve answers. See
  // docs/algorithm.typ, "启动可达性".
  bool flash = false;

  // Startup entry cuts.
  //
  // The balance model can return a plan whose cycle has no seed: an
  // "amplifier" that eats the item it makes is net positive, so the balance
  // accepts it without ever having a unit to start from. The post-solve
  // fireability check rejects that plan, but the solver then has to be told
  // what it missed, or it returns another plan built on the same unstartable
  // cycle.
  //
  // A *group* is a set of columns whose first firing is cut over. Exactly one
  // member is designated the entry (`start_c`), forced whenever the group is
  // used at all, and the entry has to pay the group's *gross* inputs out of the
  // stock plus the net output of everything outside the group:
  //
  //   x_c >= start_c                                        (used => entry)
  //   x_c <= domain_c * sum_{g in group} start_g           (used => an entry)
  //   gross(c,i) * start_c <= stock[i] + sum_{k not in group, net_k > 0} net_k * x_k
  //
  // Every fireable plan obeys this for *any* group: the first member of the
  // group to fire is used, draws on the stock and on firings outside the group,
  // and never on the group itself. The `x_c >= start_c` row is what keeps the
  // solver from naming an unused member the entry and escaping the cut.
  // The sum over the complement ignores the other recipes' consumption, so it
  // only over-estimates what could be on the shelf by then, which keeps the
  // cut sound. Two details matter: the balance matrix stores net coefficients,
  // so `gross` cannot be read back out of it (an amplifier's row entry is
  // positive) and has to come from the graph; and the group's own columns are
  // left out of the funding sum, because a cycle cannot pay for its own start.
  //
  // `planCrafting` posts the 1- and 2-cycles of the subgraph eagerly, before
  // the first solve, so the unstartable plan is excluded while it is still
  // being searched for (see buildStartupCuts); after a rejection it adds one
  // more group for the component the fireability check found deadlocked. See
  // docs/algorithm.typ, "启动可达性".
  struct EntryNeed {
    uint32_t row = 0;
    uint32_t column = 0;
    // Gross input per execution. Not the net coefficient, which the balance
    // matrix stores and which is 0 or positive for a self-consuming recipe.
    int64_t amount = 0;
  };

  struct EntryGroup {
    // Members. Some member that is actually used has to be the entry, so a
    // group is also a valid place to record a single blocked recipe.
    aw::vector<uint32_t> columns;
    // One entry per (column, row) the group's entry has to afford.
    aw::vector<EntryNeed> needs;
  };
  aw::vector<EntryGroup> entryGroups;

  // Startup *seed* cuts: the joint form of the same idea, for a two-recipe
  // cycle whose start needs two items at once.
  //
  // The entry cut prices every input against its own row, one firing at a time,
  // and a cycle can pass it and still not start. The shape is an amplifier
  // pair: `a` eats a batch of `e` that it does not make, `b` makes `e` and eats
  // `f`, and `a` makes `f`. Nothing can fire at first, so the balance asks `a`
  // to be paid out of the shelf alone, which it cannot be; the real start is a
  // few firings of `b`, each of which is itself paid out of the shelf in `f`.
  // The two rows are therefore not separately affordable, they are *jointly*
  // affordable, and only a cut that mixes them sees the difference:
  //
  //   A x64 <- B x16      a: e = B (16 per firing), f = A (64 per firing)
  //   B x1  <- A x1       b: e = B (1 per firing),  f = A (1 per firing)
  //
  // starting from eight A and eight B. The entry cut accepts it (`b` needs one
  // A, and the shelf has eight), the plan is unstartable, and the joint form
  // rejects it because one round of the cycle needs sixteen units of A or B in
  // total. See `buildStartupCuts` for the derivation of the weights.
  //
  // A cut weighs several rows against a single demand and is triggered by one
  // member being used at all:
  //
  //   x_trigger >= t,  x_trigger <= domain_trigger * t
  //   sum_k weight_k * (stock[k] + sum_{j not in cycle, net_kj > 0} net_kj x_j)
  //     >= demand * t
  //
  // The trigger is the member the derivation is about, coupled to a fresh
  // Boolean, and it cannot reuse the entry group's `start_c`: that one only
  // says "some member fires first", so a plan that starts the cycle through
  // the other member would escape this cut, and the plan that does so is
  // exactly the unstartable one the cut is for.
  //
  // The cut is a *necessary* condition of fireability, like the entry cut, so
  // the INFEASIBLE it can produce is still a proof.
  struct SeedTerm {
    uint32_t row = 0;
    // Coefficient on that row's stock plus positive outside net. Positive; a
    // term with a weight of 0 would be dropped by the solver.
    int64_t weight = 0;
  };

  struct SeedCut {
    // The cycle's members. Left out of the funding sum: a cycle cannot pay for
    // its own start, and counting the amplifier's own output would make the
    // cut vacuous.
    aw::vector<uint32_t> members;
    // The member whose use triggers the cut.
    uint32_t trigger = 0;
    // Right-hand side, in the units the weights make of the rows.
    int64_t demand = 0;
    aw::vector<SeedTerm> terms;
  };
  aw::vector<SeedCut> seedCuts;

  // Budget for the eager cycle enumeration: how many groups and how many seed
  // cuts it may add, and how many columns one group may hold. 0 for either
  // disables the pass, which leaves only the groups added after a rejection.
  uint32_t maxStartupGroups = 1024;

  // A cycle with more members than this is skipped. The mutual-consumption
  // components of a large modpack merge into one huge blob, and cutting that
  // would cost more than it is worth; the tight little cycles this pass is for
  // are well under the bound.
  uint32_t maxStartupGroupMembers = 32;

  // Physical stock per row, in the solver's row space. The startup cuts use it
  // as the seed that stock contributes; an empty span means "derive from b"
  // (max(0, -b[i])), which understates the target row and would make the cuts
  // unsound there. `planCrafting` always fills it.
  std::span<const int64_t> stock;
};

// One CP-SAT solve of the objective-cap search. A trivially copyable aggregate
// so it can live in an `aw::vector`; see `Result::attemptTrace`.
struct CapAttempt {
  int64_t cap;             // objective cap this attempt was solved under
  double budgetSeconds;    // <= 0 means unlimited
  int32_t status;          // PlanStatus as an int
  int64_t objective;       // 0 when the attempt returned no feasible plan
  double elapsedMs;        // wall-clock since `solve()` started
};

struct Result {
  PlanStatus status = PlanStatus::INVALID_INPUT;

  // Executions per recipe. Empty unless a solution was found.
  aw::vector<int64_t> x;
  int64_t objective = 0;

  bool provenOptimal = false;
  double bestBound = 0.0;
  double gap = 0.0;

  // Profiling counters.
  int64_t numConflicts = 0;
  int64_t numBranches = 0;

  // Columns removed by reduced-cost fixing before the final solve. 0 when the
  // pass did not run or removed nothing. The returned `x` is always full
  // length, with zeros at the fixed columns.
  uint32_t fixedColumns = 0;

  // --- solution-finding diagnostics -------------------------------------
  //
  // Wall-clock milliseconds spent in the LP relaxation and in the reduced-cost
  // probe. Both run *outside* `Options::maxTimeSeconds`, so a query's true cost
  // is `lpMs + probeMs + solveMs`, and on an unpruned graph the LP can dominate.
  double lpMs = 0.0;
  double probeMs = 0.0;

  // Number of CP-SAT solves summarized here: the cap-growth attempts plus the
  // probe and the reduced solve. > 1 means the objective cap bound and grew.
  int64_t capAttempts = 0;

  // CP-SAT's own deterministic-work counter, summed over attempts. Stable
  // across machines (unlike wall-clock), so a capped row stays comparable.
  double deterministicTime = 0.0;

  // Wall-clock milliseconds from the start of `solve()` to the first feasible
  // plan and to the incumbent that was returned. -1 when never reached. On a
  // capped row these are the interesting numbers: they say how long the solver
  // worked *before* the cutoff, which `solveMs` alone does not.
  double firstFeasibleMs = -1.0;
  double bestMs = -1.0;

  // Starting objective cap (after the LP/lower-bound derivation).
  int64_t initialCap = 0;

  // Per-attempt trajectory: cap, budget, status, objective and the elapsed
  // wall-clock at which each attempt finished. Serialized by the harness.
  aw::vector<CapAttempt> attemptTrace;
};

// `b` has `A.rows` entries, `c` has `A.cols` entries. Negative entries in `b`
// are free starting stock, matching the balance form used by the planner.
//
// Every entry of `c` must be at least 0. A costed column is bounded by the
// objective cap (c_r x_r <= c^T x <= cap). A *zero-cost* column is not, so the
// solver caps it with the model instead: it bounds the column by the demand
// plus the consumption capacity of the rows it produces into, under the same
// cap. Those rows may be `>=` and so allow overproduction; capping anyway is
// safe because producing more than the plan consumes is pure waste, so an
// optimum always exists inside the cap. `ceiling` (see `absoluteCap`) is the
// int64-range fallback when a zero-cost column produces into nothing.
Result solve(const Matrix& A, std::span<const int64_t> b,
             std::span<const int64_t> c, const Options& options = {});

}  // namespace aw::solver

#endif
