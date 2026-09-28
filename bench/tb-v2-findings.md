# Thunderbolt `CraftPlannerV2` on NAST and ATM — investigation

Scope: why `tb-v2` finds no plan at all on `recipes-atm`, and what its success on
`recipes-nast` actually consists of. All TB claims below are checked against the
external repo at `compare/thunderboltcore` (snapshot `42e0e7d`) and against the
benchmark JSONL in `bench/results/`.

## TL;DR

`tb-v2`'s ATM result is **by design**: a whole-graph safety cap fires before any
planning, so all 48 queries return a trivial "the target itself is missing" plan.
It is not a heuristic that tried and failed.

```
core 可达 item/pattern/input/output <= 65536   (thunderbolt.maxReachablePlanningWork)
```

ATM's reachable graph is ~1.1M work units, **16.9× over** the cap; NAST is ~62.8K,
**95.9% of** the cap. That 4% headroom is the entire difference between "runs the
planner" and "short-circuits".

On NAST, `tb-v2` gets 32/48 feasible because of a fast cap-bounded heuristic, but
the 16 misses are **structural** (cycles needing seeds / order-sensitive plans),
not a budget that is merely too small — raising every TB tunable 10–30× changes
nothing.

## 1. ATM: the whole-graph safety cap

### 1.1 Evidence

The cap is `CraftPlannerV2.MAX_REACHABLE_PLANNING_WORK`, default `65536`
(overridable by `-Dthunderbolt.maxReachablePlanningWork`). `planCore` checks it
before doing anything else and returns a bounded-missing plan:

```java
if (reachableWork > MAX_REACHABLE_PLANNING_WORK) {
    CraftPlan<K> boundedMissing = new CraftPlan<>(
            true, false, Map.of(), Map.of(), Map.of(),
            Map.of(target, amount), Map.of(target, amount), 0, true /* budgetExhausted */);
    return new PlanningResult<>(boundedMissing, diagnostics.finish(started, null));
}
```

The flag is documented as an intentional production boundary in
`docs/crafting-conflict-kernel-missing-design.zh-CN.md` §7 ("外层安全上限"):

> 第二项在任何图归一化前做一次可达扫描 … 一旦超过上限便停止扫描并返回
> `budgetExhausted=true` 的目标 Missing，`planRuns=0`。它避免一个数万节点/输出的
> 刻意图在多轮线性预处理中仍占用数秒。
>
> *(the reachable scan stops at the cap and returns target-Missing with
> `budgetExhausted=true`, `planRuns=0`; it keeps a deliberately huge graph from
> burning seconds in repeated linear preprocessing.)*

The doc calls this **Policy A**: false-negative Missing is allowed on extreme
graphs, a false-positive `feasible` never is.

Measured reachable work (same BFS TB uses: item + pattern + input + byproduct):

| corpus | reachable work | vs 65536 cap |
|---|---|---|
| `recipes-nast` | 62,787–62,925 | 95.8–96.0% (headroom ~2.6K) |
| `recipes-atm`  | **1,106,844** | **16.9× over** |

Every ATM target lives in the 61,517-item SCC (`meta.tsv` `big_scc_items`), so all
8 targets see essentially the whole graph and all 48 rows short-circuit.

### 1.2 Why the summary table looks misleading

The JSONL for default `tb-v2` ATM is not 48 "no answer" rows — it is 48
partial-plan rows:

```
status=missing, has_plan=true, missing_count=1,
missing=[[<the target itself>, <amount>]],
items_processed=0, gross_demand=<amount>,
budget_exhausted=true, plan_ms median 3.88 ms
```

`summarize.py` classifies `missing` as *answered* and `CraftPlannerV2` as
*unclaimed*, which is why the table shows `uncl=48, noans=0` instead of "did not
run". The `budget_exhausted=true` + `items_processed=0` pair is the real signal.

### 1.3 Confirmation — lift the cap and ATM plans again

Single query, target 4520 amount 1 stock none, all other caps default:

```
control  (default 65536):  missing, items_processed=0,  budget_exhausted=true,  4 ms
raised   (2,000,000):      missing, items_processed=51, budget_exhausted=true, 2312 ms
```

So the cap is the first wall. Lifting **every** tunable simultaneously
(`maxReachablePlanningWork`, `maxByproductScheduleWork`, `maxGraphExportWork`,
`maxCraftSearchWork`, `maxLowWidthTableauCells`, `maxLowWidthCellWork`,
`maxReusableMatchPairs`, `maxCraftDepth`) and re-running the full 48-query sweep:

| ATM `tb-v2` | feasible | median query |
|---|---|---|
| default caps | **0/48** | 3.9 ms |
| all caps raised | **25/48** | 2601 ms |

So ~half of ATM is explained purely by the cap. The remaining 23 misses are the
second, structural limitation below.

### 1.4 The second limitation (still fails after the cap is lifted)

The 23 remaining ATM misses are not "the graph is big" — they are specific items
that the planner cannot close, e.g.:

| missing leaf | why |
|---|---|
| `biomeswevegone:cika_wood`, `aspen_wood` | `wood ⇄ log` is a 2-cycle (sawmill 1:1 each way); the only acyclic seed is a `productivebees:/lumber_bee` recipe |
| `chipped:arched_leaded_glass_pillar` | self-referential recipe: the input tag whose members include the output item |
| `productivebees:configurable_comb`, `minecraft:milk_bucket` | world/entity-only sources, no in-graph production |

These are exactly the classes the TB docs admit V2 does not solve:
`small-conservative-recovery.zh-CN.md` ("V2 仍以 DAG 候选为主；需要保留完整守恒环的
请求可以继续漏解" — V2 is DAG-candidate-first; requests needing a full
conservation cycle can still be missed) and `non-growing-cycle-solving.zh-CN.md`.

That doc also reports the caps are not the issue: expanding the recovery search
to 65,536 states / 200 ms / 64 firings recovered **0** additional cases.

### 1.5 Production note

In the real mod there is a *second*, earlier 65536 cap —
`FastCraftingPlanner.MAX_GRAPH_EXPORT_WORK` (also `-D…maxGraphExportWork`) — that
runs before the immutable `CraftGraph` exists. The AW benchmark harness builds
`CraftGraph` directly, so only `maxReachablePlanningWork` applies in these
results; a real ATM-sized network would hit the export cap first.

## 2. NAST: what "good at feasible" really is

### 2.1 The numbers

`tb-v2` on NAST (default caps): 32/48 feasible, preprocess 221 ms, median query
121 ms. `aw-nonopt --stage satellite` on the same corpus: **48/48** feasible,
preprocess 2106 ms, median query 736 ms. So V2 is the *fast* feasibility
heuristic, not the *complete* one; AW already answers all 48.

The 16 V2 misses are concentrated in the zero/near-zero-stock cases:

| target | stock=none | missing leaf |
|---|---|---|
| `sophisticatedstorage:blasting_upgrade` | missing | `upgrade_base` |
| `alltheores:dirty_osmium_dust` | missing | `mekanism:oxygen_bucket` |
| `appliedsoul:ender_star` | missing | `the_bumblezone:royal_jelly_block` |
| `cookingforblockheads:light_gray_kitchen_floor` | missing | `black/white_concrete_powder` |

Each failing row reports exactly one small leaf missing with
`budget_exhausted=true`, i.e. the bounded search gave up with a partial plan.

### 2.2 Raising caps does not help

Re-running NAST with `maxCraftSearchWork=8,000,000` (30× default),
`maxReachablePlanningWork=2,000,000`, `maxLowWidth*` raised, `maxCraftDepth=2048`:
still **32/48**. Query times grow (e.g. 418 ms → 1513 ms for `blasting_upgrade`)
but the same leaves stay missing. This matches the docs: the failure is a
structural cutoff (cycle orientation / seed / order proof), not a work budget.

### 2.3 What actually produces the 32 feasible plans

V2's fast path is a stack of cap-bounded stages (`docs/v2-*.zh-CN.md`, source):

1. **Linear backbone** (`linearPass`): one O(V+E) aggregated material-flow pass
   with `ceilDiv`; quantity-independent, no integer matrix. Resolves ordinary
   DAG requests immediately. This is where most NAST feasible rows come from.
2. **Cycle breaking ("去头尾")**: DFS from the target drops back-edges; a
   conversion SCC is retried with up to 16 alternative orientations
   (`MAX_CONVERSION_ORIENTATION_RETRIES`).
3. **Bounded exact/local integer flow** over small conflict components:
   separator width ≤ 12, ≤ 96 vars, ≤ 128 constraints, ≤ 64 branch nodes,
   ≤ 1024 pivots, tableau ≤ 16,384 cells, shared cell-work ≤ 2,097,152.
4. **Bounded local fallback** / small conservative search: ≤ 12 items, ≤ 16
   recipes, ≤ 32 firings, ≤ 4096 states, ≤ 20 ms.
5. Every stage, on exhaustion, returns **the best partial plan so far** with
   `budgetExhausted=true` — never a wrong `feasible`.

The invariant that makes this usable as a heuristic is: *caps only ever lose
completeness, never soundness*. V2 accepts `feasible` only after a balance +
replay check against its own pool, so `feasible=true` is trustworthy **under
V2's own semantics** — with one benchmark-convention caveat, see §5.

## 3. The "cap helps CP-SAT" link

AW's `tools/CpSatSolver.cpp` needs an objective upper bound `cap`:

```cpp
constexpr int64_t CAP_GROWTH = 8;
constexpr int MAX_ATTEMPTS = 16;
```

It starts from a deliberately weak lower bound (`lowerBoundOnTotal`) and grows
`cap *= 8` until a model is feasible — because, per the comment, the graphs have
amplifying cycles with no raw base item, so no Bellman-Ford-style cost seed
exists. `docs/algorithm.typ` §"CP-SAT 求解" describes the same growth, and
§"基于价值的剪枝" (disabled) needs a known feasible `u` to fix columns by reduced
cost.

A fast heuristic feasible plan is exactly a valid `u`. V2 provides one, but note:

- On NAST its cost is `cost_rat = 2.850` (2.85× the AW optimum) — a valid but
  loose bound.
- On ATM (cap lifted) some plans are up to ~7× the AW optimum (e.g.
  `chipped:smoothed_double_inlayed_red_sandstone` amount 100000: V2 712,899 vs
  AW 223,392).
- AW already has a strictly more complete feasibility heuristic:
  `aw-nonopt --stage satellite` is 48/48 on both corpora, only slower
  (preprocess 2.1–4.3 s, median query 0.7–9.7 s).

So V2 is interesting *as a cap source* because it is cheap (NAST 121 ms median
query, 221 ms preprocess), not because it is more complete than AW's non-optimal
mode.

## 4. Does the O(V+E) backbone help when the reachable subgraph is one giant SCC?

Yes, and the reason is **not** graph shrinkage. Measured with `PlanningDiagnostics`
(temporarily wired into `AwrBench`; `reachableItems`/`reachablePatterns` are the
post-back-edge-cut DAG, per `DiagnosticsCollector.recordCompilation`):

| corpus | corpus items | DAG items (median) | DAG patterns | back-edges cut | linear pass |
|---|---|---|---|---|---|
| NAST (default) | 12,853 | 4,855 | 10,019 | 3,154 | 0.7 ms |
| ATM (cap raised) | 68,345 | **66,527** | 258,623 | 44,252 | **13.6 ms** |

On ATM the "linear" pass traverses ~97% of all items — the big SCC is not avoided
at all. It is still 13.6 ms, because `linearPass` is literally one topological
aggregation sweep: `buildDag` DFS-cuts back-edges (AE2's "去头尾", `frameFor`
cuts any input that is a GRAY ancestor), then a reverse post-order pass resolves
each item exactly once with `ceilDiv`. Cost is linear: 13.7× the DAG items → 19×
the pass time.

What the backbone buys is *success without a solver*, not traversal speed:

- NAST: 31 of the 32 feasible rows have `planRuns=1` and `lowWidthAttempts=0` —
  the first `run` answered them and no integer matrix was ever built. Median
  total 34 ms (21 ms of that is DAG compilation, 0.7 ms the sweep).
- ATM raised: **all 25** feasible rows have `planRuns=1, lowWidthAttempts=0`;
  median total 1037 ms (405 ms compile, 13.6 ms sweep).
- The failures are the other story: `planRuns` median 3–5 (orientation retries)
  and `searchCutoff=true`. Those queries never get a feasible answer at all, so
  the backbone did not help them — the 44K cut back-edges removed the cycle
  routes they need.

Honest framing: the backbone is a *cheap sufficient* constructor, not a complete
one. It answers whenever the retained acyclic edge set already supports the
request — 31/48 on NAST, 25/48 on ATM — and it is essentially free (tens of ms
even on the full ATM graph). The seconds are spent only on the queries where it
fails.

### 4.1 What this means for AW's first-feasible time

AW has no equivalent stage: `aw-flash` still builds the full model and solves
for a first incumbent. But compare carefully — the benchmark's AW `reach_ms` is
inflated by the deliberately-raised satellite budget:

| | registration | per-query reach | per-query solve |
|---|---|---|---|
| `aw-flash` ATM, benchmark (`--satellite-seconds 2.0`) | 4360 ms | 4045 ms med | 2195 ms med |
| `aw-flash` ATM, production (`--satellite-seconds 0.05`) | 4360 ms | **142 ms med** | 2654 ms med |
| V2 ATM (cap raised) | 860 ms header | ~420 ms DAG compile+sweep | — |

The benchmark `reach_ms` clusters at ~0 / ~4045 / ~8060 ms, i.e. 2 s × 2–4
satellite rounds. Re-running with the library default `0.05` cuts the median
reach to 142 ms (mean 4711 → 126 ms) with no change in feasibility. So AW's real
first-feasible bottleneck on ATM is the CP-SAT solve (~2.6 s median), not graph
size. That is where a greedy DAG-projection constructor could pay off: ~14 ms to
a feasible plan on the ~half of ATM it covers, plus a valid `u`/`cap` for the
rest. Note also that `cost_rat` and the ablation table's `pre_ms`/`plan_ms` are
all measured with the 2 s satellite budget, so any speed comparison against
the baselines should quote the production setting alongside it.

## 5. Recommendations

1. **Do not read TB-V2's ATM `0/48` as a heuristic failure.** If the paper needs
   to compare planners on ATM, either raise
   `-Dthunderbolt.maxReachablePlanningWork` (documenting it as "shipped vs.
   raised") or state explicitly that the shipped default declines the whole ATM
   corpus by design at the reachable-work safety cap. The `budget_exhausted=true`
   + `items_processed=0` fingerprint should be called out; the default summary
   table hides it under `uncl=48`.
2. **Judge V2 on the cap-lifted ATM run (25/48) and on NAST (32/48)**, plus the
   plan quality (`cost_rat`), not the shipped-default ATM cell.
3. **For AW, the transferable idea is the invariant, not the specific caps:**
   bounded work everywhere, cap exhaustion returns a conservative partial, never
   a false feasible. AW's per-stage budgets in `include/aw/Options.h` already
   follow this shape.
4. **If AW wants V2-style plans as a CP-SAT `cap` seed:** wire a fast
   feasibility pass and feed `cap = cost(feasible)` into `solveWithCap`, instead
   of `lowerBoundOnTotal` grown ×8. AW's own `aw-nonopt`/`aw-flash` already
   produce a valid `u` at 48/48, so benchmark whether the extra V2 pass is worth
   it versus just using flash mode's first incumbent.
5. **For first-feasible speed, the cheapest experiment is a greedy DAG pass
   before CP-SAT.** `buildDag` + one aggregated sweep is ~14 ms on ATM's 66K-item
   graph; it returns a feasible plan (and a `u`/`cap`) whenever the retained
   acyclic edges suffice, and fails fast otherwise. AW's `aw-flash` first
   incumbent costs ~2.6 s of CP-SAT on the queries a sweep would cover. Wire it
   as a pre-pass and benchmark coverage + `cap` tightness against `aw-flash`.
6. **Do not expect V2 to cover ATM even with more budget.** Its documented and
   observed gaps (full conservation cycles, seed ordering, world-only leaves)
   are structural. If ATM coverage matters, it has to come from AW's pruning +
   CP-SAT path, not from the V2 heuristic.

## 6. Caveat: target-stock semantics differ from AW's

One `tb-v2` row on NAST (`cookingforblockheads:light_gray_kitchen_floor`, amount
1, `random20`) has `feasible=true, balance_ok=false, false_positive=true`:

```
cost=0, distinct_recipes=0, items_processed=1, missing=[], status=ok
```

The target is in the `random20` stock (`6187\t2`). V2 satisfies the request from
stock and returns zero firings; the harness's independent check rejects that.

The reason is a deliberate AW convention, documented in `src/Plan.cpp`:

```
// Always produce `amount` new units; the target inventory is not
// subtracted. Recipes may still consume the target, so cycles through
// the output remain available.
rhs[i] = amount;
```

so `rhs(target) = amount` even when the target is in stock, and AW crafts it
anyway (`aw-nonopt` on that row: cost 1, 39 recipes, `balance_ok=true`).
`AwrBench.balanceOk` copies that convention, so an engine that draws the target
from stock is flagged. This is a benchmark-convention mismatch, not a V2
soundness bug: using stock for the requested item is a legitimate plan.

Consequences for the tables: `summarize.csv` has `false_positive=1` for
`recipes-nast,tb-v2`, but the printed table's `fp_vs` column is 0 because AW also
calls that row feasible, so a reader of the table never sees it. If the paper
quotes TB's false-positive count, either exclude "target in stock" rows or use a
`rhs` that credits target stock.

## 7. Reproducing

```bash
# ATM default (shipped) — 0/48, all short-circuited
python3 bench/run.py --datasets recipes-atm --configs tb-v2

# ATM with the whole-graph cap lifted — 25/48
cd compare/thunderboltcore
JAVA_TOOL_OPTIONS="-Dthunderbolt.maxReachablePlanningWork=2000000 \
  -Dthunderbolt.maxByproductScheduleWork=2000000 -Dthunderbolt.maxCraftSearchWork=2000000 \
  -Dthunderbolt.maxLowWidthTableauCells=2000000 -Dthunderbolt.maxLowWidthCellWork=2000000 \
  -Dthunderbolt.maxCraftDepth=2048" \
  ./gradlew --no-daemon benchAwr --args="--dataset recipes-atm \
    --awr ../../temp/recipes-atm.awr --plan ../../bench/plans/recipes-atm \
    --config tb-v2 --out /tmp/atm.raised.jsonl \
    --names ../../temp/recipes-atm.names.tsv --warmup 0 --repeats 1 --deadline-ms 40000"

# NAST, default vs raised budgets: both 32/48
```
