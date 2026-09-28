# The mod / planner interface

`aw_jni` is the only thing the Minecraft mod links against. It exposes five
static native methods, one thread's worth of work, and two payload formats: a
binary blob for the graph and the plan, and JSON for the options. It also
carries a small item-name search index; see `search.md` for that endpoint.

The Java counterpart lives in
`src/main/java/io/aduhtkjm/appliedwheelchair/natives/`. The untracked
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
                                 3 iteration limit, 4 invalid input
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
cheapest.

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
deadNodePruning            bool
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
objectiveCap, objectiveUpperBound, reducedCostGap, flash
```

`solutionHint` is deliberately absent: it is an internal warm start that
`planCrafting` fills from the greedy DAG pre-pass, not a knob.

`plannerOptionsJson()` / `solverOptionsJson()` dump the current values as a
document the setters accept verbatim.

The registration-time switches (`tagInlining` and the dominance / pack passes)
are read once by `registerCraftingGraph`, so they take effect on the next
registration. The query-time ones (`satellite`, `deadNodePruning` and the
query-time half of tag inlining) can be changed at any time. The solver budget is
re-read on every `plan` call, so it applies to the next query without
re-registering.

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

The Windows DLL is a separate path, `scripts/build-windows.sh`, which stages the
OR-Tools runtime DLLs next to `aw_jni.dll` in the build directory.

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

`aw_jni` must sit **next to** the OR-Tools runtime in both cases. On Windows the
JVM loads an absolute DLL path with `LOAD_WITH_ALTERED_SEARCH_PATH`, so a DLL's
own directory is searched for its dependencies first; that is what makes a
co-located OR-Tools work at all, and it is why `scripts/build-windows.sh` copies
the runtime DLLs next to the shim.

On Linux the equivalent is `DT_RUNPATH`, which for a build tree points at the
OR-Tools library directory by absolute path, so a development run works as-is. A
*bundled* Linux library has no such path baked in and needs the extracted
directory on `LD_LIBRARY_PATH`. Windows is the shipping target; the Linux
bundled path is a convenience, not a supported install.
