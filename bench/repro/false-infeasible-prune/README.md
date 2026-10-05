# Minimal reproduction: query-time pruning proves a feasible query infeasible

Two queries from the `recipes-nast` ablation panel come out **`infeasible`**
(proven, with `branches = 0`) in a more-pruned stage while a less-pruned stage
proves a finite **optimum** for the same query. A feasibility claim cannot be
monotone-decreasing under *sound* dominance pruning, so at least one stage is
wrong.

| target | stock | stage | status | cost | branches |
|---|---|---|---|---|---|
| 3914 Energetic Alloy Nugget | `none` | `none` / `seed` / `direct` | `ok`, proven | 29107 | 2084–8447 |
| 3914 Energetic Alloy Nugget | `none` | `recipe` | **`infeasible`** | — | 0 |
| 11968 Gold | `random20` | `none` / `seed` | `ok`, proven | 3646 | 7726–8469 |
| 11968 Gold | `random20` | `direct` | **`infeasible`** | — | 0 |

The ablation ladder is cumulative, so the flip identifies the rule: `recipe`
(composite dominance) drops a producer of Energetic Alloy Nugget, and `direct`
(column dominance) drops a producer of Gold. In both cases the reduced subgraph
has no balance-feasible solution at all, so CP-SAT proves it infeasible without
searching.

## Reproduce

```bash
cd ../../..                      # repository root
./build/aw_bench --dataset recipes-nast --awr temp/recipes-nast.awr \
    --plan bench/repro/false-infeasible-prune/p --config aw-ablation \
    --out /tmp/false-infeasible.jsonl --names temp/recipes-nast.names.tsv \
    --nonoptimal 0 --gap 0 --amounts 100000 --groups none,random20 \
    --warmup 0 --repeats 1 --time-limit 30 --workers 1 \
    --stage none --stage seed --stage direct --stage recipe

python3 - <<'PY'
import json
rows = [json.loads(l) for l in open('/tmp/false-infeasible.jsonl') if l.strip()]
q = {(r['target'], r['stock'], r['stage']): r for r in rows if r.get('type') == 'query'}
for tgt, stk in [(3914, 'none'), (11968, 'random20')]:
    print('target', tgt, stk)
    for st in ['none', 'seed', 'direct', 'recipe']:
        r = q[(tgt, stk, st)]
        print('  %-8s status=%-11s proven=%-8s cost=%-6s branches=%s'
              % (st, r['status'], r['optimality'], r['cost'], r['branches']))
PY
```

Output **before** the fix (the bug):

```
target 3914 none
  none     status=ok          proven=proven   cost=29107  branches=8447
  seed     status=ok          proven=proven   cost=29107  branches=2600
  direct   status=ok          proven=proven   cost=29107  branches=2084
  recipe   status=infeasible  proven=no_plan  cost=0      branches=0
target 11968 random20
  none     status=ok          proven=proven   cost=3646   branches=7727
  seed     status=ok          proven=proven   cost=3646   branches=8469
  direct   status=infeasible  proven=no_plan  cost=0      branches=0
  recipe   status=infeasible  proven=no_plan  cost=0      branches=0
```

The correct output has `status=ok` in every row with the unpruned optimum, which
is what the current code produces after the `Greedy.cpp` fix below:

```
target 3914 none
  none     status=ok  proven=proven  cost=29107  branches=8447
  seed     status=ok  proven=proven  cost=29107  branches=2600
  direct   status=ok  proven=proven  cost=29107  branches=2084
  recipe   status=ok  proven=proven  cost=29107  branches=1735
target 11968 random20
  none     status=ok  proven=proven  cost=3646   branches=7727
  seed     status=ok  proven=proven  cost=3646   branches=8469
  direct   status=ok  proven=proven  cost=3646   branches=6866
  recipe   status=ok  proven=proven  cost=3646   branches=7245
```

## Diagnosis

The flip is a **false `INFEASIBLE` produced by the greedy DAG pre-pass**, not by
a dominance pass. The chain is:

1. Pruning changes the query subgraph.
2. `greedyDagPlan` returns a vector that does **not** satisfy the exact balance
   `A x >= b`. The sweeps rank routes with the optimistic capacity
   `cap(x) = stock(x) + max_r producible(r)`, which is only an estimate, so the
   propagation can leave an item's consumption above its production. Both
   flipping queries fail exactly this way:

   | target | stage | greedy cost | balance violations | true optimum |
   |---|---|---|---|---|
   | 3914 | `recipe` | 27331 | 1 | 29107 |
   | 11968 | `direct` | 3283 | 1 | 3646 |

   An unbalanced vector can be *cheaper* than the true optimum.
3. `planCrafting` trusts it as `objectiveUpperBound` (it only checked
   `greedyCost > 0`), so the CP-SAT objective cap is set **below the optimum**.
4. Every capped attempt is infeasible, so the solver reports `INFEASIBLE` for a
   query whose true subgraph is feasible.

The check used to live in `BestPlan::consider` (`satisfiesBalance`) and was
removed in `d1e8596` ("Experiments on Greedy") together with the doc sentence
that claimed the verification. The comment at the top of `Greedy.cpp` still says
"The result is always re-checked against the exact balance `planCrafting` hands
the solver, so it can never be a false positive" — that re-check was simply
gone.

The earlier reading (dominance + `pruneSeedUnreachable` stranding the
replacement) came from `awr_inspect`, which leaves `options.reprune.enabled = true`
while `aw_bench` sets it to false. The two therefore run **different** subgraphs,
which is what made the dominance story look correct there. On the bench's
subgraph, removing seed pruning does not fix these rows; fixing the greedy
does, at every stage.

## Fix

Verification is restored in `Greedy.cpp`: every candidate is checked against the
exact matrix (anchor and byproduct rows alike) inside `BestPlan::consider`, and
an unbalanced candidate is rejected so a later cycle-cut view gets a chance to
supply a balanced one. This removes the false proofs **without touching any
pruning pass** — the ablation's dominance counts and costs are unchanged, and
because a rejected candidate no longer shadows a valid one, the surviving
greedy plan is often better and the query needs *fewer* CP-SAT branches than the
bogus cap did (`direct` for 11968: 6866 instead of 13223).

## What this does not settle

The `FIXME` in `docs/algorithm.typ` ("column dominance guarantees balance but
not that the plan can start") is a real soundness question about the pruning
passes and remains open. It is **not** what these five rows demonstrate: with the
greedy gap closed, all 32 (target, stock, stage) rows of the cumulative ladder
come back `ok, proven` at the unpruned optimum, including `tag` and `pack`.

The one-step `selfConsuming` guards in `RecipePrune.cpp` and `Prune.cpp` still
only catch a sibling that consumes the anchor directly, not a longer cycle back
to it. A grounding fixpoint as described in the FIXME would still be the
principled hardening if a dominance-induced false `INFEASIBLE` is ever found
again.

## Impact

`coverage.tex` and `baselines.tex` treat `aw-optimal` infeasibility as ground
truth. Before this fix, any query whose greedy candidate happened to be
unbalanced could be reported `infeasible` with zero branches; after it, that
class of false claim is gone.
