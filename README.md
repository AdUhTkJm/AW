# Applied Wheelchair — native crafting graph

The C++ half of [Applied Wheelchair](https://github.com/AdUhTkJm/AppliedWheelchair).
It turns the `.awr` recipe dump produced by
`io.aduhtkjm.appliedwheelchair.recipe.RecipeIO` into a compressed sparse row
(CSR) bipartite graph and installs it as a process-wide singleton that the
solver kernel can read.

The module lives in native WSL (not on the Windows mount) so it can be built and
tested here without Gradle.

## The graph

The dump is naturally "output item → list of recipes", and each recipe is a list
of "input item → amount". That is a bipartite graph with two edge kinds:

| direction      | row is    | target is  | weight       |
| ---------------- | ---------- | ----------- | --------------- |
| `itemToRecipe`  | item node  | recipe node | produced amount |
| `recipeToItem`  | recipe node | item node  | consumed amount |

Items and recipes share one flat node numbering, so a walk (`item → recipe →
input item → …`) never needs a translation step:

```
[0, itemCount)                item nodes    (handle h -> node h-1)
[itemCount, itemCount + recipeCount)  recipe nodes
```

`CraftingGraph::itemNode` / `itemHandle` / `recipeNode` convert. Every recipe
occupies exactly one edge in `itemToRecipe` — the row of the item it outputs —
so `recipeToItem.targetsOf(r)` is exactly the recipe's inputs, and
`recipeOutput[r]` / `recipeOutputAmount[r]` cache where it sits.

Pseudo-resources (EMI "ingredients" that stand for a set of alternatives) are
just regular item nodes with handles above `realResourceCount`; they are
"craftable" from their members by the dump itself, so no special casing is
needed. `isRealItem()` tells the two apart.

Workstations are parsed (so the stream stays in sync) and discarded. They get
their own structure later.

## Binary schema

Implemented from the comment on `RecipeIO`:

```
magic           4 bytes  'A' 'W' 'R' 0x01
realResourceCount   varint
entryCount        varint

per output, ascending:
   outputDelta    varint
   recipeCount    varint
   per recipe:
      outputAmount    varlong
      workstationCount varint
      workstation    varint  (see quirk below)
      inputCount     varint
      inputAmount    varlong
      inputDelta     varint
```

All variable-length integers are unsigned LEB128 (max 5 bytes for the 32-bit
kind, max 10 for the 64-bit kind).

## API

`include/aw/crafting_graph.h`:

```cpp
aw::CraftingGraph parseCraftingGraph(std::span<const std::byte> bytes);  // throws ParseError

void registerCraftingGraph(std::span<const std::byte> bytes);  // replaces, throws ParseError
const aw::CraftingGraph* currentCraftingGraph() noexcept;    // nullptr before the first call
std::shared_ptr<const aw::CraftingGraph> craftingGraphSnapshot();
```

`registerCraftingGraph` parses into a temporary first, so a malformed blob
leaves the existing graph untouched, then swaps it under a mutex. Use
`craftingGraphSnapshot()` when another thread may register a new graph while you
walk the current one; `currentCraftingGraph()` is the cheap raw pointer for
single-threaded hot paths.

## JNI

`src/jni_bridge.cpp` binds one static native method via `JNI_OnLoad` /
`RegisterNatives`, so the C++ symbol name does not depend on the Java package:

```java
package io.aduhtkjm.appliedwheelchair.natives;
public final class CraftingGraphKernel {
   public static native void registerCraftingGraph(byte[] blob);
}
```

A malformed blob surfaces as `IllegalArgumentException`. If the Java entry point
moves, change `AW_JNI_CLASS_NAME` (see the top of `jni_bridge.cpp`) rather than
the Java side. A network packet handler can simply do
`CraftingGraphKernel.registerCraftingGraph(payload)` on the server.

## Build and test (WSL)

```sh
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Decode a real dump and validate the CSR invariants:
./build/awr_inspect --check /mnt/d/IdeaProjects/AppliedWheelchair/runs/client/aw/recipes.awr
./build/awr_inspect --dump  /mnt/d/IdeaProjects/AppliedWheelchair/runs/client/aw/recipes.awr | less
```

`awr_inspect` output for the current dump:

```
real resources  : 4420
item nodes     : 4682  (2125 produced, 2557 leaf-only)
  pseudo-items  : 262
recipe nodes    : 6790
item -> recipe  : 6790 edges
recipe -> item  : 9478 edges
checks: OK
```
