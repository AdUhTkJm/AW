# The mod / planner interface

`aw_jni` is the only thing the Minecraft mod links against. It exposes five
static native methods, one thread's worth of work, and two payload formats: a
binary blob for the graph and the plan, and JSON for the options. It also
carries a small item-name search index; see `search.md` for that endpoint.

The Java counterpart lives in
`src/main/java/io/aduhtkjm/appliedworktable/natives/`. The untracked
`java-ref/CraftingGraphKernel.java` here is the reference copy of the method
signatures.

## Why the split is shaped this way

* **The recipe blob stays binary.** It is megabytes and cold, and the AWR format
  and its reader already exist. Nothing about it is human-edited.
* **The options are JSON.** They are a few dozen scalars that a person edits in
  a config file, so a named-key patch is the clearest shape. A patch is a
  partial update: absent keys keep their value, and an unknown key is an error
  rather than a silent no-op.
* **The plan is one blob-in / blob-out call.** The whole query is therefore
  atomic with respect to the status flag, and the JNI surface stays at five
  methods.
* **The response is self-describing.** Every executed recipe carries its output
  handle, output amount and inputs. It has to: registration canonicalization
  renumbers and merges recipes, so a subgraph recipe id is *not* an index into
  the blob the mod sent, and query-time tag inlining can synthesize recipes that
  were never in the blob at all. `Subgraph::itemOrigin` / `recipeOrigin` cannot
  survive the trip, so the translation happens inside `runPlan`.

## Threading and status

The mod owns exactly one worker thread (`PlannerService`). It builds the recipe
graph, calls `registerCraftingGraph`, and then blocks on its queue until a plan
is requested. Because only that thread enters the native code and the queue is
FIFO, **concurrent queries are impossible by construction** rather than merely
forbidden; the native side needs no locks.

The cooperation point is `status()`, which reads one atomic and touches no
planner state. It is the only method that is safe to call from another thread
while one of the others is running, and it is what a UI thread polls:

| `aw::KernelStatus` | value | set while |
| --- | --- | --- |
| `OK` | 0 | nothing is running; a call may start |
| `PREPROCESSING` | 1 | inside `registerCraftingGraph`: decoding, the parse-time cleanup and every registration-time pruning pass |
| `PLANNING` | 2 | inside `plan`: the reachability walk and the solve |

The flag is raised and lowered with `KernelStatusGuard`, an RAII type, so an
early return cannot strand the kernel in a busy state. A caller that sees `OK`
has exclusive access.

Registration is the long one. The pack-certificate pass alone has a 60 s budget
by default, so a poller should expect `PREPROCESSING` to sit there for seconds
in a large pack.

## Native methods

```java
public static native void   registerCraftingGraph(byte[] blob);
public static native int    status();
public static native void   setPlannerOptions(String json);
public static native void   setSolverOptions(String json);
public static native byte[] plan(byte[] request);
```

Methods are bound by name in `JNI_OnLoad` (`RegisterNatives`), so no symbol
depends on the package; only the compile-time macro `AW_JNI_CLASS_NAME` does.

**The class named by `AW_JNI_CLASS_NAME` must not have a static initializer that
loads the library.** `JNI_OnLoad` runs inside the load and calls `FindClass` on
that class, which would re-enter the initializer. The mod keeps the loading in a
separate `NativeLoader` class for exactly this reason.

`registerCraftingGraph` throws `IllegalArgumentException` on a malformed blob and
leaves the previous graph untouched. `plan` throws `IllegalArgumentException` on
a malformed request. Both option setters throw `IllegalArgumentException` on a
bad document and change nothing at all.

## Plan request

LEB128, matching the recipe blob's conventions. Handles are 1-based and are
written as ascending deltas, because both a stock map and a workstation set are
small ascending sets.

```
magic        'A' 'W' 'P' 0x01
varlong      amount              must be > 0
varint       targetHandle        must be >= 1
varint       nWorkstations
  varint     workstationDelta    ascending; the first delta is absolute, later ones must be non-zero
varint       nStock
  varint     stockHandleDelta    ascending, non-zero
  varlong    stockAmount
```

Trailing bytes are an error. `Workstations` and `stock` are rejected when they
name a handle outside the registered graph: `reachableSubgraph` would ignore an
unknown workstation and `planCrafting` would ignore unknown stock, which would
quietly turn a Java-side bug into a plausible-looking plan.

Stock is what the player already holds, indexed by handle. The solver subtracts
it from the balances, so a stocked input does not have to be crafted. The target
is the exception: `amount` units are produced *in addition* to any stock of the
target the player has.

## Plan response

```
magic        'A' 'W' 'Q' 0x01
varint       status              aw::PlanStatus: 0 ok, 1 infeasible, 2 numerical fail,
                                 3 iteration limit, 4 invalid input,
                                 5 cycle unfulfilled
byte         provenOptimal       0 or 1
8 bytes      gap                 IEEE-754 double, little-endian, raw bits
8 bytes      bestBound           IEEE-754 double, little-endian, raw bits
varlong      numConflicts
varlong      numBranches
varint       fixedColumns
varint       nUses
  varlong    count               recipe executions, always >= 1
  varint     outputHandle
  varlong    outputAmount        units produced per execution
  varint     nInputs
    varint   inputHandleDelta    ascending
    varlong  inputAmount         units consumed per execution
```

Only recipes the plan actually executes appear: the solver returns one value per
subgraph recipe and most are zero. Entries come in subgraph recipe order.
`provenOptimal` is false when the solver stopped on its gap or time budget rather
than proving optimality; the plan is still a real plan, just not certified
cheapest. It is also false for `cycle unfulfilled`.

`cycle unfulfilled` (5) is the post-solve fireability check: the solver found a
balance-feasible plan, but some cycle in it cannot be started from the stock
plus the upstream output, so it is not a runnable plan. It is deliberately
*unproven*: that plan is unrealizable, but a different plan may still exist.
`planCrafting` already spends up to `maxCycleRetries` extra solves looking for
one, each time forbidding the plan it just rejected; this status is what comes
back when that budget runs out. For this status `uses` still describes the last
rejected plan, so a caller can fall back to the acyclic plan or give up. See
`docs/algorithm.typ`, "启动可达性" and "no-good 重试".

The doubles are raw bits rather than formatted text, so no precision is lost and
no locale is involved. The Java side reads them with a little-endian
`ByteBuffer`.

## Options JSON

`setPlannerOptions` patches `aw::Options`; `setSolverOptions` patches
`aw::solver::Options`. Field names are the C++ member names, exactly. A patch is
validated into a copy and committed only when every key has parsed, so a failure
leaves the running configuration untouched.

`aw::Options`:

```
nonoptimal, tagPruning, recipePruning, directPruning, substitutionPruning,
seedPruning                bool
outputPruningProfile       bool   (accepted on every platform; only meaningful
                                   in a build with AW_PROFILE_PRUNING)
tagInlining                string "off" | "prePrune" | "queryTime" | "both",
                           or the ordinal 0..3
pack                       object { enabled, maxPackRecipes, maxPackValue,
                                  maxBranchDepth, maxBranchNodes,
                                  maxZeroStockItems, maxSeconds }
satellite                  object { enabled, maxComponentNodes, maxIslandNodes,
                                  maxSeconds }
maxTagMembers, maxSiblingRecipes, maxWitnessProducers,
maxCostMemo, maxSubstitutionGuardItems,
maxSubstitutionGuardTotal  non-negative integer
maxTagPairs, maxTagCoverWork, maxWitnessPairs, maxPrunePairs,
maxSubstitutionWork, maxSubstitutionCostWork   non-negative integer (64-bit)
maxSubstitutionDepth, maxCostDepth             integer (32-bit)
maxNeed                                        integer (64-bit)
maxPropIterations                              integer (32-bit)
```

`aw::solver::Options`:

```
relativeGap, absoluteGap, maxTimeSeconds, numWorkers, randomSeed,
objectiveCap, objectiveUpperBound, reducedCostGap, maxCycleRetries, flash
```

`solutionHint` is deliberately absent: it is an internal warm start that
`planCrafting` fills from the greedy DAG pre-pass, not a knob. `noGoods` is
also absent: `planCrafting` owns that list internally.

`maxCycleRetries` (default 3, 0 disables) bounds how many extra solves
`planCrafting` runs after its post-solve fireability check rejects a plan. Each
rejected vector is added as a no-good, and the retries share `maxTimeSeconds`,
so the total latency stays inside the configured budget. See
`docs/algorithm.typ`, "no-good 重试", for the soundness caveat.

`plannerOptionsJson()` / `solverOptionsJson()` dump the current values as a
document the setters accept verbatim.

`deadNodePruning` is a retired knob: `seedPruning` now subsumes the dead-node
cleanup, so the separate pass is gone. The key is still accepted on input and
ignored, so an older client that sends it is not rejected; it is not written
back by `plannerOptionsJson()`.

The registration-time switches (`tagInlining` and the dominance / pack passes)
are read once by `registerCraftingGraph`, so they take effect on the next
registration. The query-time ones (`satellite`, `seedPruning` and the query-time
half of tag inlining) can be changed at any time. The solver budget is re-read on
every `plan` call, so it applies to the next query without re-registering.

## Building the library

The option loader needs nlohmann/json, vendored rather than fetched at configure
time:

```sh
scripts/fetch-nlohmann.sh          # -> third_party/nlohmann/nlohmann/json.hpp
```

Then the Linux build (and the tests) pick it up automatically:

```sh
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 cmake -S . -B build -G Ninja
cmake --build build
build/aw_tests
```

The Windows DLL is a separate path, `scripts/build-windows.sh`. MSVC and Ninja
cannot build inside the WSL filesystem: Ninja runs each command through cmd.exe
with the build directory as its working directory, and cmd.exe cannot chdir to a
UNC path, so the tree lives in `%LOCALAPPDATA%\Temp\aw-build-windows` and the
script copies `aw_jni.dll` plus the OR-Tools runtime DLLs it stages beside it
into `build/windows/` afterwards. That directory is a build cache: deleting
`build/windows/` does not force a recompile, deleting the Windows tree does.

`src/OptionsJson.cpp` is the only translation unit that is *not* compiled with
`-fno-exceptions`: nlohmann uses them, and its entry points catch everything
before returning.

## Loading the library from the mod

`NativeLoader` looks in two places, in order:

1. the directory named by `-Daw.native.dir`, which a development run sets from
   the `awNativeDir` Gradle property;
2. `/natives/<platform>/` inside the jar, extracted to a temporary directory.
   The Gradle `stageNatives` task writes the libraries there together with an
   `index.txt`, because a jar directory cannot be listed at runtime and the
   loader therefore has to be told what to unpack.

`aw_jni` must sit **next to** the OR-Tools runtime in both cases, but on Windows
that alone is not enough. Measured on JDK 21, `System.load` of an absolute path
does not put the DLL's own directory on the dependency search path: a
co-located `ortools.dll` fails with "Can't find dependent libraries" (Windows
error 126). `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)` resolves the
same file and its co-located dependencies, so the limitation is on the JVM's
side of the call, not in the DLL. `NativeLoader` therefore retries: it loads the
sibling libraries first, in dependency order, and then loads `aw_jni`, which
leaves `JNI_OnLoad` to the JVM. `scripts/build-windows.sh` copies the runtime
DLLs next to the shim either way, so only the directory has to be handed to the
JVM.

On Linux a build tree needs none of that: `DT_RUNPATH` points at the OR-Tools
library directory by absolute path, so the direct load succeeds. A *bundled*
Linux library has no such path baked in, and the same sibling preload covers it,
so it does not need `LD_LIBRARY_PATH` either. Windows is the shipping target;
the Linux bundled path is a convenience, not a supported install.
