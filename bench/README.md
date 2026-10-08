> **Stale (paper workspace, 2026-10):** the dataset table, the target-sample design (§3.2), the budgets (§3.6) and the configuration list (§3.4) below predate the controlled two-panel collection the paper reports.  The current sampling frame and the exact command that regenerates the paper's tables are in `paper/scripts/collect_data.py`, `paper/tables/instances.tex` and `paper/Makefile`; the numbers in §3.1 no longer match the `.awr` files on disk.

# Cross-engine crafting-planner benchmark

Everything needed to compare **AW** against the two external baselines (**Thunderbolt
Core** and **AE2VM**) on the real recipe corpora, and to ablate AW's own prunings.

The harness never starts Minecraft. Each engine is driven through its own offline
entry point:

| Engine | Entry point | Minecraft needed? |
|---|---|---|
| AW | `aw_bench` → `reachableSubgraph` + `planCrafting` | no, native |
| Thunderbolt (`tb-v2`) | `CraftPlannerV2.plan(CraftGraph<Integer>, …)` | no (compile-time deps only) |
| Thunderbolt (`tb-cpsat`) | `CpSatRankedFlowSolver.solve(…)` | no |
| AE2VM | `CraftingVM` + the in-repo `BenchAEKey` / `BenchPatternDetails` / `BenchSimulationState` / `FakeBenchGrid` fixtures | no |

---

## 1. Files

```
bench/
  awrlib.py       canonical .awr reader + C++-equivalent canonicalization
  make_plan.py    seeded target sample + the three inventory groups -> bench/plans/
  run.py          driver: runs every (dataset, config) cell, one child process each
  summarize.py    JSONL -> results/summary.cells.csv + results/summary.{csv,md}
  plans/          generated instance manifests (committed: this is the audit trail)
  repro/          minimal, self-contained reproductions of engine defects
  results/        generated output (gitignored)
tools/
  Bench.cpp       the `aw_bench` tool (built by CMake next to `awr_inspect`)
  awr_dump.py     quick eyeball dump of a .awr, using bench/awrlib.py
compare/thunderboltcore/
  src/test/java/com/moakiee/thunderbolt/core/crafting/planner/AwrBench.java
  build.gradle    -> `./gradlew benchAwr`
compare/ae2vm/
  src/test/java/com/ae2vm/addon/bench/AwrVmBench.java
  build.gradle    -> `./gradlew benchAwr`
```

`bench/repro/ae2vm-orange-wool/` is a one-query instance with AW's ground-truth answer and
the exact commands that make AE2VM return an unbalanced plan. See §7.3.

---

## 2. One-time setup

```bash
# AW side: build the library, awr_inspect and aw_bench
./test.sh                      # or: cmake --build build --target aw_bench
ls build/aw_bench              # GCC/Clang only; the MSVC/DLL build skips it
                               # because its balance check uses __int128

# Thunderbolt: the OR-Tools native jar for the Linux test runtime.
# Only needed if the NeoForged mirror 502s on it; Maven Central always has it.
D=~/.m2/repository/com/google/ortools/ortools-linux-x86-64/9.15.6755
mkdir -p "$D"
[ -f "$D"/ortools-linux-x86-64-9.15.6755.jar ] || {
  wget -O "$D"/ortools-linux-x86-64-9.15.6755.jar \
    https://repo1.maven.org/maven2/com/google/ortools/ortools-linux-x86-64/9.15.6755/ortools-linux-x86-64-9.15.6755.jar
  wget -O "$D"/ortools-linux-x86-64-9.15.6755.pom \
    https://repo1.maven.org/maven2/com/google/ortools/ortools-linux-x86-64/9.15.6755/ortools-linux-x86-64-9.15.6755.pom
}

# Thunderbolt sanity check (should be BUILD SUCCESSFUL)
cd compare/thunderboltcore && ./gradlew --no-daemon referenceCapabilityTest && cd ../..
```

**AE2VM needs four config fixes to build off the author's Windows box.** They are already
applied in this tree; keep them if you re-clone:

| File | Change | Windows value |
|---|---|---|
| `gradle/wrapper/gradle-wrapper.jar` | was missing (gitignored); copied from `compare/thunderboltcore` | — |
| `gradle/wrapper/gradle-wrapper.properties` | `gradle-8.10-bin.zip` → `gradle-8.14-bin.zip` (only 8.14 is cached here) | either works |
| `gradle.properties` | commented out `org.gradle.java.home=C:\Users\Administrator\.jdks\corretto-22.0.2`, commented out the four `systemProp.*.proxy*` lines, `org.gradle.configuration-cache` `true`→`false` | restore as needed |
| `build.gradle` | commented out `compileOnly files('E:/TB ThirdParty/…/thunderbolt-2.0-alpha.jar')` (no source references it since v1.13.17) | restore on Windows |

---

## 3. Experiments this harness implements

### 3.1 Datasets

| name | items | recipes (raw → canonical) | biggest SCC | leaf items |
|---|---:|---:|---:|---:|
| `recipes-vanilla` | 1 417 | 2 282 → 2 236 | 32 | 494 |
| `recipes-small` | 2 521 | 6 790 → 6 523 | 1 027 | 398 |
| `recipes-nast` | 12 853 | 63 491 → 35 306 | 5 356 | 1 276 |
| `recipes-atm` | 68 345 | 677 507 → **413 688** | **61 517** | 5 780 |

All four readers normalize the file identically before anything is built, so the canonical
column is what every engine sees: `dropNonPositiveInputs` (an input that consumes nothing per
firing is not a flow — AE2's entropy recipes spell a fluid ingredient with no amount at all,
and Thunderbolt's `CraftInput` rejects a non-positive amount outright), then
`dropUselessRecipes`, then `canonicalizeRecipes`. Dropping such an edge can make two recipes
identical, which is why `recipes-nast`'s canonical count moves by one and not three.
A `.awr` written before this rule existed therefore graphs exactly like one written after it:
`bench/plans/*` is unchanged, byte for byte.

`recipes-atm` is the headline case: 90 % of its 68 345 items sit in one strongly connected
component, 61 517 of them. Any method that assumes the recipe graph is a DAG has to cut that
component down to a tree before it can start, which is exactly the regime the AW docs call
out.

`make_plan.py` re-derives the same numbers `awr_inspect` prints, because it runs the same
two-pass parse, then `dropUselessRecipes` (a real recipe that is a net loss on its own
output, or that no workstation can run) and `canonicalizeRecipes` (dedup by output, amount
and inputs, uniting workstations). Item handles are never renumbered, so a plan file works
for every engine.

### 3.2 Target sample (fixed seed)

* Pool: real items (handle ≤ `nReal`) with at least one producing recipe. Tag/pseudo items
  and leaf-only raw materials are excluded.
* `rng = random.Random("<seed>:targets")`; seed defaults to `20260101`.
* `rng.sample(sorted(pool), 8)`. The drawn handles, their names, their recipe count and
  whether they sit in the biggest SCC are written to `<dataset>.targets.tsv`.
* Each target is requested at **amount 1 and amount 100000**.
* `--big-scc` restricts the pool to the largest strongly connected component, which is the
  adversarial sample for DAG-based planners (`recipes-nast`'s biggest SCC is 5 356 items).

### 3.3 Inventory groups

All three come from the same manifest, so every engine sees byte-identical stock.
Workstations are **all available** everywhere by default (AW gets handles
`1..nReal`; the baselines have no workstation notion, so all recipes are usable).

| group | contents |
|---|---|
| `none` | nothing stocked. Not automatically infeasible: some recipes consume nothing. |
| `leaves` | every recipe-free leaf gets 1 000 000 — "raw materials are unconstrained". |
| `random20` | 20 % of real items, drawn by `random.Random("<seed>:random20")`, each with a log-uniform amount in `[1, 10^6]`. |

#### Workstation availability (`--ws-percent`)

Every AW tool (`aw_bench`, `aw_scc`, `awr_inspect`) can run on a realistic
subset instead of all workstations. With `--ws-percent <0..100>`:

1. A station whose resource location (second column of the `.names.tsv`, the
   `--names` table, default the one next to the `.awr`) starts with
   `minecraft:` is always on.
2. Let `M` be the number of remaining, non-vanilla items that appear as a
   workstation at least once. A uniformly drawn subset of exactly
   `round(p * M)` of them is kept.

The total is `V + round(p * M)` where `V` is the always-on vanilla count, which
exceeds `p` of all workstation items by about `(1 - p) * V`. The draw is a fixed
splitmix64 stream seeded by `--ws-seed` (default `20260101`), so a dataset plus
percentage pins down one subset across every tool and run. `--ws-percent 100`
(the default) is the historical all-on setting, and `awr_inspect`'s explicit
`--ws <handle,...>` overrides the percentage.

### 3.4 Configurations

| config | what it is | rows per (target, amount, group) |
|---|---|---|
| `aw-optimal` | AW, `nonoptimal=false`, every pruning on. **Ground truth.** | 1 |
| `aw-nonopt` | AW, `nonoptimal=true`, 8 cumulative ablation stages | 8 |
| `aw-flash` | AW, `nonoptimal=true`, every pruning on, but `solver::Options::flash` stops CP-SAT at the first feasible plan | 1 |
| `tb-v2` | Thunderbolt `CraftPlannerV2` (shipped default) | 1 |
| `tb-cpsat` | Thunderbolt `CpSatRankedFlowSolver` (opt-in OR-Tools) | 1 |
| `ae2vm` | AE2VM `CraftingVM`, production resolver policy, `--warmup` passes first | 1 |
| `ae2vm-cold` | the same, with `--warmup 0`: every query's first execution | 1 |

`ae2vm` and `ae2vm-cold` are separate configs on purpose. AE2VM's warm path replays a
memoized plan and can return a **different answer**, not merely a faster one; §7.3 has a
one-query reproduction. Report both or you are reporting an unspecified configuration.

`aw-flash` is AW's latency-oriented end: the same registration and the same all-on pruning
as the `aw-nonopt` `satellite` row, but `solver::Options::flash` returns the first feasible
plan instead of a cheap one. It is **not** ground truth and certifies optimality only when
that first incumbent happens to be provably optimal, so `prov` collapses to whatever it
finds. Its point is the shape of the trade: how much latency the proof costs and what the
first feasible plan's cost is relative to `aw-optimal` (`cost_ratio_median` by default;
run `summarize.py --mean` or `--max` for the mean/worst ratio).

### 3.5 Ablation stages (cumulative, `nonoptimal=true`)

```
none → seed → direct → recipe → substitution → tag → pack → satellite
```

`direct` is column dominance, `recipe` is the composite (inline) pass, `substitution` is
dominated-input substitution, `tag` is tag-edge dominance, `pack` is the wasteful-pack
certificates, `satellite` is satellite/island elimination. Tag inlining stays **off**
throughout. Each stage additionally enables the gates of the stages before it, so the last
row of the table is "everything on in nonoptimal mode" and the `aw-optimal` row differs
from it only by the `nonoptimal` flag.

**One registration serves all eight stages.** The four dominance passes always compute
their marks at registration; `setTagPruningEnabled` and friends are read by
`reachableSubgraph` at query time, so `aw_bench` flips them per stage. `pack` is the
exception — `computePackPruning` early-returns when disabled — so the tool always enables
it before registering and only uses the flag as a query-time gate afterwards. Only
`options.nonoptimal` genuinely cannot be flipped, which is why `aw-optimal` is a second
process. `solver::Options::flash` is read at query time, so `aw-flash` could ride the
`aw-nonopt` registration, but `run.py` still gives it its own process so its JSONL is a
self-contained single-row config. Net cost: **three registrations per dataset**, not ten.

Because the gates are flipped per query, a stage is self-contained: `run.py --configs
aw-nonopt --stages tag` re-measures only the `tag` point and merges it back into the
existing `aw-nonopt` JSONL, leaving the other seven rows untouched.

#### `probe`: the pass set flash mode actually uses

Flash mode does not run the cumulative curve. It asks the greedy pre-pass for a plan on a
subgraph built from everything up to and including `pack`, plus `exclusive` and nothing
else, and only falls back to the full query when that has no plan. The `probe` stage is
that pass set: `satellite`, `variant` and `fold` off, `exclusive` on. It exists so the
latency this buys can be measured against the cost in plan quality, and it is **not** part
of the cumulative curve -- `fold` is on in every other stage, because the pass was added
afterwards and the published ablation was collected without it.

`exclusive` is deliberately the one pass the probe keeps. It is the cheapest of the three
by an order of magnitude, and skipping it is what makes the greedy pre-pass pick routes
that trade an exclusive item for tag capacity: on `recipes-nast` a single query's plan goes
from cost 6 to cost 2056, and another from 852 112 to 201 513 636. With it kept, the worst
observed quality ratio over all four corpora is 1.2x and the median is exact.

`--flash-probe 0` measures the same queries with the probe off, which is the A/B that the
`from_greedy` and `greedy_ms` query columns support: `from_greedy` distinguishes a greedy
answer (probe hit or solver-free) from a solved one without inferring it from
`conflicts`.

### 3.6 Budgets

| knob | default | note |
|---|---|---|
| per-query cutoff | 40 s | AW: `solver::Options::maxTimeSeconds` (one shared deadline across the whole `planCrafting` call). Java: a watchdog thread that abandons the query and records `timeout`. |
| warm-up | 1 pass | unmeasured full pass over the query list, to let HotSpot and the engine caches settle |
| repeats | 1 | measured passes; `repeat` is recorded per row so you can take the min or median |
| CP-SAT gap / threads | 0.01 / 16 | library defaults |
| AW pack budget | 60 s | library default |
| AW satellite budget | **2.0 s** (library default 0.05) | deliberately raised: the pass is wall-clock bounded, and at 0.05 s the last ablation stage would be timing-dependent rather than attributable to the pass. `--satellite-seconds 0.05` restores production behaviour. |

---

## 4. Running it

### 4.1 Smoke test (the two small corpora; ~10 minutes with a 5 s cutoff)

```bash
cmake --build build --target aw_bench
python3 bench/run.py --datasets recipes-vanilla,recipes-small --time-limit 5
python3 bench/summarize.py
```

Expected shape of the output (vanilla, 5 s cutoff, 5 s Java watchdog). The
`dead_node` row below is from before that stage was renamed to `seed`; rerun
`run.py` to refresh it:

```
== recipes-vanilla ==
  optimality: prov=proven optimal, unprov=plan without a proof (AW gap / TB budget cut),
              uncl=engine has no optimality notion, noans=no answer at all (not merely unproven)
  aggregation: median
config      stage           n  feas   prov unprov   uncl noans tout   vs_ok   fp_vs      gap  cost_rat  pre_ms  plan_ms   items  recipes
aw-optimal  optimal        48    20     19      1      0     1    0   47/47       0   0.0000     1.000     2.2      1.4       2        1
aw-nonopt   none           48    20     18      2      0    16    0   32/32       0   0.0000     1.000     3.9      2.0      68       88
aw-nonopt   dead_node      48    20     18      2      0    16    0   32/32       0   0.0000     1.000     3.9      2.0      68       88
aw-nonopt   direct         48    20     18      2      0    16    0   32/32       0   0.0000     1.000     3.9      2.5      68       88
aw-nonopt   recipe         48    20     18      2      0    16    0   32/32       0   0.0000     1.000     3.9      2.2      62       87
aw-nonopt   substitution   48    20     18      2      0    16    0   32/32       0   0.0000     1.000     3.9      2.5      62       87
aw-nonopt   tag            48    20     18      2      0    12    0   36/36       0   0.0000     1.000     3.9      1.8      48       48
aw-nonopt   pack           48    20     19      1      0     9    0   39/39       0   0.0000     1.000     3.9      0.4       8        7
aw-nonopt   satellite      48    20     19      1      0     1    0   47/47       0   0.0000     1.000     3.9      1.1       2        1
aw-flash    satellite      48    20     15      5      0     1    0   47/47       0   0.5714     1.000     4.2      1.2       2        1
tb-v2       -              48    20      0      0     48     0    0   47/47       0        -     1.000     0.0      3.8       0        0
tb-cpsat    -              48    20     47      1      0     0    0   47/47       0        -     1.000   458.1     19.5       0        0
ae2vm       -              48    12      0      0     48     0    0   37/47       1        -     1.250     9.9      0.5       0        0
ae2vm-cold  -              48    12      0      0     48     0    0   37/47       1        -     1.250     9.7      1.9       0        0
```

Read it as: the ablation shrinks the median query subgraph from 68 items / 88 recipes to
2 / 1 and removes 15 of the 16 instances that fail to give an answer. Both Thunderbolt planners agree with
the ground truth on every conclusive instance; AE2VM disagrees on 10, reports 1 instance as
feasible that AW proves infeasible, and emits 2 firing vectors that do not balance at all
(the `fp` column — see §7.2). Note that in this sweep `ae2vm` and `ae2vm-cold` happen to
agree: the false positive is history-dependent, which is why §7.3 uses a one-query instance
instead of the sweep to expose it.

`aw-flash` answers the same 47 conclusive instances as `aw-optimal` here, but certifies
only 15 of them: it stops at the first feasible plan, so `prov` falls and `unprov` absorbs
everything it declined to prove. `gap` is the median gap of those incumbents (pass
`summarize.py --max` for the worst one). The counts
wobble between runs (16 workers), so read the columns, not the exact integers.

The `pre_ms` column is engine-only (§5.2): `aw-*` is `registerCraftingGraph`, AE2VM is the
`PatternCompiler` pass over 2 236 patterns, `tb-cpsat` is the OR-Tools native runtime, and
`tb-v2` is 0 because it has no one-time phase at all. The harness' own parse is reported as
`parse_ms` in `summary.csv` (on this corpus: AW ~0.7 ms, the two Java harnesses ~60 ms and
~560 ms).

For AW the `plan_ms` column is `query_ms` (`reach_ms` + `solve_ms`), the same whole-query
quantity the baselines report as `plan_ms`; see §5.2.

### 4.2 Full run (overnight)

```bash
mkdir -p bench/results
nohup python3 bench/run.py --datasets recipes-nast,recipes-atm \
      --time-limit 40 --warmup 1 --repeats 1 \
      > bench/results/run.log 2>&1 &
tail -f bench/results/run.log
python3 bench/summarize.py
```

Cells are child processes, so an interrupt or a crash only loses that cell; rerun with
`--datasets X --configs Y` to fill a gap, or with `--resume` to fill it without paying for
the points that already ran (§4.4). Progress and the exact command line of every cell
are in `bench/results/<dataset>.<config>.log`.

Rough cost model for the full run, per dataset: AW is 3 registrations (dominated by the
dominance passes and the 60 s pack pass, which on `recipes-atm` is minutes, not seconds)
plus 480 queries; `tb-v2` and `ae2vm` are one JVM each plus 48 queries; `tb-cpsat` is one
JVM plus up to 48 × 40 s of native solving, which is the expensive cell.

`recipes-atm` needs a bigger JVM heap than the default: the Java harnesses hold one object
per pattern, and it has 413 688 of them. Both Gradle tasks set `-Xmx8g`; raise it with
`-PawrBenchHeap=16g` if an `ae2vm` or `tb-v2` cell dies with `OutOfMemoryError` (check that
cell's `.log`). On `recipes-atm` the AE2VM cell spends ~1.5 s in `parse_ms` (reading and
canonicalizing the `.awr`) and ~0.5 s in `preprocess_ms` (compiling all 413 688 patterns),
so the parse dominates the one-time half there.

### 4.3 Useful variations

```bash
# A/B the integrality relaxation alone (optimal vs all-on nonoptimal), no baselines
python3 bench/run.py --datasets recipes-small --configs aw-optimal,aw-nonopt

# AE2VM cold only: no warm-up pass at all (see 7.3, the answer can differ)
python3 bench/run.py --datasets recipes-small --configs ae2vm-cold

# adversarial sample: targets from the biggest SCC
python3 bench/run.py --datasets recipes-nast --big-scc

# refresh one point of the aw-nonopt ablation (keeps the other seven stages)
python3 bench/run.py --datasets recipes-nast --configs aw-nonopt --stages tag

# report mean / worst-case instead of the default median
python3 bench/summarize.py --mean
python3 bench/summarize.py --max

# one table per inventory setting (none / leaves / random20) instead of pooling them
python3 bench/summarize.py --group-inventory

# a corpus you exported yourself
python3 bench/make_plan.py --dataset my-pack --awr /path/recipes.awr --names /path/recipes.names.tsv
python3 bench/run.py --datasets recipes-vanilla          # edit DATASETS in run.py for a new path

# Thunderbolt CP-SAT with its own internal bounds instead of the shared 40 s
python3 bench/run.py --datasets recipes-small --configs tb-cpsat --tb-deadline-ms 0

# refresh only the preprocessing half of a cell; every query row (and its plan_ms) is kept
python3 bench/run.py --datasets recipes-atm --configs tb-cpsat --preprocess-only
```

### 4.4 Resuming an interrupted cell

A long sweep is usually killed by something outside the harness: an SSH drop, a reboot, a
Ctrl-C to free the machine. `--resume` picks it up where it stopped instead of starting the
cell over.

```bash
# an overnight run died at query ~800 of 1536
python3 bench/run.py --datasets recipes-nast,recipes-atm --time-limit 40 --warmup 1 --resume
python3 bench/summarize.py
```

How it works, and what a resumed file is worth:

* The unit of resume is the **point**: `(stage, target, amount, stock, repeat)`. A point that
  completed is never re-measured, and a query that was interrupted mid-solve is not recorded
  at all, so `--resume` re-runs it. There is nothing to clean up by hand: a torn last line
  left by a hard kill is dropped when the file is read, and the point it belonged to is
  measured again.
* The `config` header and the `registration` row of the file being continued are copied
  into the new one, so `pre_ms`/`register_ms` keep the value that was actually measured and
  the old `plan_ms` rows stay comparable. The child re-registers in memory (it cannot query
  without a graph) but does not record the new timing.
* A cell whose file already holds every point of the requested sweep exits **before**
  registering, which is the expensive half of an AW cell (the pack pass alone is minutes on
  `recipes-atm`). Re-running a complete sweep with `--resume` is therefore nearly free.
* Settings that changed since the file was started are reported on stderr rather than
  silently mixed in:
  `resume: settings differ from the rows already in the file: time_limit_s 40.000000 -> 20.000000`.
  A file that mixes a 40 s and a 20 s cutoff is not a data set, so pass the same flags as
  the original run. Timings (`parse_ms`) are exempt: they are measurements, not settings.
* Ordering inside the file is not canonical after a resume (old rows first, then the new
  ones, in shard order). `summarize.py` does not care, and neither does another resume.
* Works with `--shards`: each shard reads the same canonical file as its inventory of
  completed points, so a shard with nothing left to do exits without writing anything.
* AW configs only. `tb-v2`, `tb-cpsat`, `ae2vm` and `ae2vm-cold` are rejected with a clear
  message; their harnesses have no equivalent mode. `--resume` cannot be combined with
  `--preprocess-only` (that mode already keeps every query row and refreshes only the
  header).

`aw_bench --resume 1 --resume-from <file>` is the same mechanism one level down, for a
hand-run cell. The default `--resume-from` is `--out`, which appends in place; passing a
fresh `--out` keeps the old rows untouched until `run.py` merges them, which is what makes
an interrupted merge recoverable.

---

## 5. What the numbers mean

### 5.1 Solution cost

`cost` is the shared objective, and it is the same one `src/Plan.cpp` optimises:

```
min  Σ_{real recipes} c_r · x_r        # tag/pseudo-item recipes cost c = 0
s.t. produced(i) - consumed(i) >= b_i,  x_r ∈ ℤ≥0
     b_target = amount,  b_i = -stock(i) otherwise
```

`c_r` is the recipe's cost in *real Minecraft executions*: 1 for an ordinary recipe, 0 for
the free member edge of a pseudo-resource, and the batch size for a level of a chanced
recipe, which the mod folds into one deterministic column. `cost` therefore counts
executions of the underlying game recipe, not planner steps: one firing of a batch-64 level
costs 64. Schema v3 of the `.awr` blob carries the number; see `BaseCraftingGraph::cost` in
`include/aw/plan/CraftingGraph.h` and the doc comment on the format in `src/plan/CraftingGraph.cpp`.

AW reports it as the solver objective. The baselines' `cost` is recomputed by the harness
from the firing vector each engine returned, so a "cheap" plan and a "wrong" plan are
distinguishable. `amount` is the contract amount; the Java harnesses also record
`request_amount`, the amount actually handed to the engine after the per-engine translation
of §7.2. **Only compare `cost` on rows where `feasible` is true and the ground truth is
`proven_optimal`; otherwise the ratio is meaningless.** `summarize.py` already
enforces this and leaves `cost_ratio_median` (or `cost_ratio_mean` / `cost_ratio_max` under
`--mean` / `--max`) blank when no instance qualifies.

`real_exec` is the same plan counted the other way: one per executed real recipe, ignoring
`c_r`, plus `tag_exec` for the free tag edges. The two agree on a dataset with no chanced
recipe (vanilla) and diverge exactly by where a batch level was used.

> **The baselines are still unweighted.** The reference engines have no notion of a chanced
> batch: they see the batched column as an ordinary recipe, so their own `cost` adds 1 per
> real execution where AW adds `c_r`. On a dataset that contains batched chanced recipes
> (anything but the vanilla pack) a `cost` comparison against them is therefore *not* a
> like-for-like comparison, and only the `proven_optimal` AW rows say anything. Compare
> `real_exec` across engines instead, or weight the reference harnesses too: the number is
> in the firing vector they already return, matched against `Recipe.cost` in `awrlib.py`.

### 5.2 Time

`pre_ms` is the engine's **one-time** work, and nothing else. `parse_ms` is the harness' own
input preparation (read the `.awr`, read the plan manifests, build the objects the engine is
handed) and is deliberately kept out of `pre_ms`, because it is not something an engine does.

| field | AW | baselines |
|---|---|---|
| harness input preparation | `parse_ms` (file read + name table; the `.awr` *parse* is inside `registerCraftingGraph`) | `parse_ms` (parse + canonicalize + build the harness' graph/pattern objects) |
| preprocessing | `register_ms` (`registerCraftingGraph`: parse + canonicalize + dominance + pack certificates) | `preprocess_ms`: AE2VM compiles every pattern; `tb-v2` is **0** (it has no one-time phase); `tb-cpsat` is the OR-Tools native runtime init |
| query | `reach_ms` + `solve_ms` = `query_ms` | `plan_ms` |

`preprocess_ms` is what `summarize.py` reports, and it means different amounts of work per
engine by construction:

* **AW** — one `registerCraftingGraph` per graph version: canonicalization, the tag/recipe/
  direct/substitution dominance passes and the pack certificates. The satellite pass is *not*
  here; it is per query (`reach_ms`), because it needs the target and the inventory.
* **AE2VM** — `PatternCompiler` compilation of every pattern. The pattern-table construction
  is harness work and lives in `parse_ms`. AE2VM has no other one-time phase.
* **`tb-v2`** — **0**. `CraftPlannerV2` compiles its DAG, feedback components and capacity
  index lazily *inside* `plan()`, so in this harness that cost is per query and lands in
  `plan_ms`; `diag_graph_compile_ns` on each row isolates it. In production the same work is
  shared by the amount probes of one crafting calculation (`PlanningSession`), not by the game.
* **`tb-cpsat`** — the OR-Tools native runtime load (`cp_sat_init_ms`). Its `compile()` is per
  `solve()` call, so it too is inside `plan_ms`.

A header written before this split has no `preprocess_scope` field: its `preprocess_ms` still
contains the harness parse, and `tb-cpsat` reported the native init separately. `summarize.py`
folds `cp_sat_init_ms` back in for those rows and prints a note. `--preprocess-only` refreshes
such a cell in place:

```bash
python3 bench/run.py --datasets <dataset> --configs <config> --preprocess-only
```

It runs the harness with `--preprocess-only`, which writes the config header (and AW's
registration row) and exits **without running a query**, then merges that header into the
existing JSONL so every query row — and every `plan_ms` — is preserved byte for byte. The
child process is one gradle/`aw_bench` launch, so it costs seconds, not hours. If the JSONL
does not exist yet, only the preprocessing half is recorded and `run.py` says so. Pass the
same `--time-limit`, `--pack-seconds` and `--satellite-seconds` the original sweep used: the
merged header describes the run that produced the *new* preprocessing number, and the AW
header records those settings (they do not affect the query rows, which are kept as they
were).

After a full sweep, every legacy header can be brought over in one cheap pass:

```bash
python3 bench/run.py --satellite-seconds 0.05 --preprocess-only
python3 bench/summarize.py
```

AW's `reach_ms` is the reachability walk including every query-time pruning pass; `solve_ms`
is `planCrafting` alone. The Java numbers include the watchdog thread hop, so they are
slightly pessimistic — irrelevant, since the intent of the Java runs is "does this help in
a real client", not a paper speed ranking.

Subgraph size (`items`, `recipes`, `r2i_edges`, `items_multi_recipe`) is **AW-only**; it is
the state after pruning, which is exactly what "remaining nodes and edges" means for the
ablation. The baselines have no equivalent boundary, so those columns are 0 for them;
`items_processed` and `gross_demand` are Thunderbolt's own "work done" counters.

### 5.3 Status vocabulary

| status | meaning |
|---|---|
| `ok` | a complete plan was found |
| `missing` | a usable but partial plan; `missing` lists what fell short |
| `infeasible` | proven no plan exists (AW) / the CP-SAT model is infeasible |
| `iter_limit` | no answer. Split by `timeout` (the cutoff really elapsed) vs `gave_up` |
| `timeout` | the Java watchdog abandoned the query |
| `no_pattern` | AE2VM found no pattern producing the target |
| `unsupported` / `invalid` | the CP-SAT runtime was unavailable / a malformed model |

`cold` is `true` only for the first execution of a query in the process, so a
`--warmup 0 --repeats 1` run is all-cold and a `--warmup 1` run is all-warm.

**`gave_up` is not `timeout`.** On the unpruned subgraph CP-SAT returns `UNKNOWN` in a few
milliseconds: zero-cost tag columns fall back to `absoluteCap`'s ~10^16 domain, and the LP
search gives up immediately. The harness separates the two so a fast "no answer" is never
read as "spent the whole cutoff". It is also the clearest single result of the ablation:
without the later prunings AW stops answering rather than answering slowly.

---

## 6. Output files

* `bench/results/<dataset>.<config>.jsonl` — one `config` header, then either one
  `registration` row (AW) or nothing, then one `query` row per measured query. Exactly one
  row per point, which is what a resume relies on. The header
  carries `parse_ms`, `preprocess_ms`, `preprocess_scope` (`engine`) and `preprocess_only`;
  a header without `preprocess_scope` predates the split (see §5.2).
* `bench/results/<dataset>.<config>.log` — the child's stdout/stderr.
* `bench/results/<dataset>.<config>.preprocess.log` — the same, for a `--preprocess-only`
  run. It gets its own name so refreshing the preprocessing half never overwrites the full
  sweep's log.
* `bench/results/summary.cells.csv` — one row per (dataset, config, stage, target, amount,
  stock, repeat), joined with the ground-truth row for that instance.
* `bench/results/summary.csv`, `summary.md` — one row per (dataset, config, stage, stock):
  counts, aggregates, agreement tallies, cost ratios. Every aggregate is the median by
  default; `python3 bench/summarize.py --mean` (or `--max`) switches them all to the mean
  (or the worst case) and suffixes the columns `_mean` / `_max` accordingly. Without
  `--group-inventory` the three inventory settings are pooled and `stock` is `-`;
  `--group-inventory` gives each setting its own row and renders one table per setting.
* `bench/results/summary.csv` also carries `preprocess_ms`, `parse_ms`, `items_<agg>`,
  `recipes_<agg>`, `conflicts_<agg>` and `branches_<agg>`, so the CP-SAT effort counters
  you asked for are in the same table. `parse_ms` is CSV-only; the printed table has no
  column for it.

---

## 7. Methodology notes and traps

1. **Ground truth is `aw-optimal`, and only where it is conclusive.** `infeasible` and
   `ok` are authoritative; `iter_limit`/`gave_up` rows are dropped from the agreement
   tallies, and cost ratios additionally require `proven_optimal`.
2. **`balance_ok` is the false-positive detector.** Each Java harness recomputes
   `produced - consumed >= rhs` in `BigInteger` from the engine's own firing vector, using
   the same `rhs` AW's solver gets: `rhs(target) = amount`, `rhs(i) = -stock(i)` otherwise.
   `feasible && !balance_ok` is a plan that claims to be complete and does not balance; no
   timing analysis can find it, and `summarize.py` counts it as `false_positive`.
   **The request each engine is handed differs, because the engines disagree on what a
   stocked target means.** AW and AE2's production calculation always craft `amount` new
   units and ignore the target's own stock (`ignore(output)`). Thunderbolt's planners
   instead let the target's stock satisfy the request, so the harness asks them for
   `amount + stock(target)`; the net deliverable is still `amount`, and the target-in-pool
   path stays exercised (it shows up as an inflated `cost`, not as a false positive).
   AE2VM never draws the target from stock, so it is handed `amount` with the target zeroed
   in its inventory. Every row records the engine-facing `request_amount` next to the
   contract `amount`.
   The smoke test yields 2 such rows per small corpus from AE2VM while Thunderbolt and AW
   stay at 0, which is the positive control for the AE2VM defects of §7.3.
3. **AE2VM is a heuristic, in two distinct ways.**
   *One recipe per output item*: its resolver is `Function<AEKey, IPatternDetails>` and the
   production policy is `AE2VMCrafting.pickBestPattern` (smallest per-craft output amount),
   which the harness uses, plus `wouldCauseCycle` for dead rings (`--dead-ring off` disables
   it). On a corpus where the right route is not the smallest-output one, AE2VM cannot find
   it, so its `cost` is a heuristic's cost and its `missing` is not a proof of
   infeasibility.
   *History-dependent answers*: the warm path replays a memoized plan, and it can return a
   different answer than the cold path — including an unbalanced one. `bench/repro/ae2vm-orange-wool/`
   pins this down on a single query: `minecraft:orange_wool x1` with the `random20` stock is
   feasible with cost 2 (AW proves it), AE2VM reports `missing` cold, `ok` with
   `balance_ok=false` warm. The full 48-query sweep is *not* a controlled way to see the
   difference — it shows the false positive in both the warm and the cold cell — so quote the
   one-query reproduction, not the sweep, when you make this claim.
4. **Thunderbolt's CP-SAT has two internal bounds that are tighter than 40 s.** For a
   direct caller, `CpSatRankedFlowSolver` caps each native solve at 3 s and its default
   `PlanningSession` at 250 ms. `run.py` therefore binds the common cutoff by default
   (`--tb-deadline-ms same` → `--bind-deadline-ms 40000`), which installs a planning
   deadline so CP-SAT can use the full wall clock like AW does. `--tb-deadline-ms 0` measures the shipped behaviour instead; report which one
   you used.
5. **Workstation symmetry.** AW is run with every handle in `1..nReal` as an available
   workstation, and the real recipes in these corpora all have at least one workstation, so
   nothing is lost by the baselines' lack of the concept. `dropUselessRecipes` is applied
   identically on both sides, so a recipe no workstation can run is never handed to a
   baseline as a free route.
6. **Degenerate inputs are normalized away, identically on both sides.** AW's own solver
   already treats an input of amount 0 as a no-op (`Plan.cpp` drops zero entries from the
   balance column), but a baseline planner may not be able to represent it at all —
   Thunderbolt's `CraftInput` throws on a non-positive amount. Rather than hand three engines
   two different graphs, every reader drops the edge and keeps the recipe (§3.1), so the free
   route an amount-0 input implies survives for all of them, in the same shape.
7. **`items`/`recipes`/`conflicts`/`branches` are not comparable across engines.** AW has a
   pruned-subgraph boundary; Thunderbolt has `items_processed`; AE2VM has neither. Only the
   AW columns form a curve.
8. **Timing hygiene.** `--warmup 1` runs a full unmeasured pass. Use `--repeats 3` and take
   the minimum when you care about AW's `solve_ms`: CP-SAT with 16 workers is not
   deterministic, so a single run of a hard instance is a sample, not a measurement. The
   `repeat` column is in every row for exactly this reason. For AE2VM the warm-up is not
   only a timing decision — §7.3 — so the driver exposes `ae2vm-cold` as its own config
   rather than leaving it to a flag.
9. **`satellite` is wall-clock bounded** (see §3.6). If you lower
   `--satellite-seconds` back to 0.05, expect the last ablation stage to flap between runs
   and say so in the paper.
10. **Plan reuse.** `bench/plans/*.tsv` is committed. Do not regenerate it between engines:
   `make_plan.py` is seeded and deterministic, but the whole point of committing it is that
   a reviewer can check that every config was fed the same 48 instances.

---

## 8. Extending it

* **More targets / amounts**: `--targets 16 --amounts 1,100` on `run.py` (or
  `make_plan.py` directly). Adding targets does not shift the `random20` draw, because the
  two RNG streams are seeded separately.
* **A real save file's inventory**: replace `bench/plans/<dataset>.stock.random20.tsv` with
  `handle<TAB>amount` lines. AW's `awr_inspect --plan` and the Java loaders read the same
  file.
* **A new corpus**: add an entry to `DATASETS` in `bench/run.py`, then
  `python3 bench/make_plan.py --dataset <name> --awr <path> --names <path>`.
* **Extra ablation stages**: the profiles live in `kStages` in `tools/Bench.cpp`. Adding
  one is a single table row; the driver needs no change if you also add it to `AW_STAGES`
  in `bench/run.py`.
* **AW's optimal-mode ablation**: `aw_bench --nonoptimal 0 --stage <profile>` works for any
  profile, so `aw-optimal` can be turned into a second full curve by passing several
  `--stage` flags.
* **Flash ablation**: `aw_bench --flash 1 --stage <profile>` (single stage) or all eight
  `--stage` flags works the same way, if you want flash's own curve rather than the
  single `aw-flash` row. `--flash` is a per-query solver option, so it does not need a
  separate registration.
