# CPU profiling `awr_inspect`

A development-only CPU profiler for the native planner, built on
[gperftools](https://github.com/gperftools/gperftools). It exists to answer
"where does a plan spend its time", and it produces a flamegraph from a single
run.

Profiling is a host-side tool concern only:

* It is compiled into `awr_inspect` when gperftools is installed on a
  non-Windows host, and is a no-op otherwise.
* It never runs unless you pass `--profile`, so a normal run is unaffected.
* `aw_crafting`, `aw_jni`, the Java side and the Windows build do not include
  `aw/Profiler.h` and never link gperftools.

## Install

The profiler needs the library and its `pprof` analysis script:

```sh
sudo apt install libgoogle-perftools-dev google-perftools   # Debian/Ubuntu
```

Then reconfigure and rebuild. CMake reports what it found:

```
-- awr_inspect: CPU profiling enabled (/usr/lib/x86_64-linux-gnu/libprofiler.so)
```

Without gperftools the build still succeeds; `--profile` then exits with an
error instead of silently doing nothing.

## Profile a run

`--profile <file>` wraps the **whole run**: decode and canonicalize, the
dominance-pruning precompute, the reachability pass, the LP relaxation and
CP-SAT solve, and the report. A profile can be taken with any mode, but the
interesting case is a plan:

```sh
build/awr_inspect --plan minecraft:oak_planks --amount 64 \
    --time-limit 10 --profile temp/oak.prof temp/recipes-small.awr
```

The tool prints the number of samples it gathered on the way out:

```
profile: 536 sample(s) -> temp/oak.prof
```

Sampling is a statistical process, so a short run gives a noisy graph. Three
things thicken it:

* `--time-limit <s>` — a bigger budget means more samples. The default is 2s;
  a single run typically yields only a few hundred samples.
* `CPUPROFILE_FREQUENCY=1000` — raises the sampling rate from the default
  100 Hz. Values up to 4000 are reasonable on a development machine.
* `--workers 1` — CP-SAT's search threads are all sampled into one profile, so
  a single worker gives a far more legible graph.

Aim for at least a few thousand samples before reading much into the shape.

## Flamegraph

```sh
scripts/fetch-flamegraph.sh                      # once: interactive renderer
scripts/flamegraph.sh temp/oak.prof build/awr_inspect
```

This runs gperftools' `pprof --collapsed` and renders the folded stacks into
`temp/oak.svg`. Open it in a browser and it is interactive, exactly like any
other FlameGraph:

* **click a frame** to zoom into that subtree; click *unzoom* (or the frame's
  ancestors) to come back,
* **Ctrl-F** to search for a frame, highlighting every match,
* **Ctrl-I** to toggle case-sensitive search.

That interaction is the JavaScript embedded in Brendan Gregg's `flamegraph.pl`;
there is no pure-Python package that reproduces it for arbitrary folded stacks
(the PyPI `flamegraph` package only *profiles* Python and still needs
`flamegraph.pl` to render). So the renderer is fetched rather than rewritten.

`scripts/fetch-flamegraph.sh` downloads a pinned FlameGraph release into
`third_party/flamegraph/` (gitignored), following the same pattern as
`fetch-ortools.sh`. FlameGraph is CDDL-1.0; fetching it keeps the repository
from redistributing it.

`scripts/flamegraph.sh` picks the first renderer it finds, in this order:

1. `$FLAMEGRAPH_PL`, if set — point it at `inferno-flamegraph`, a drop-in Rust
   port, or any other `flamegraph.pl`-compatible program,
2. `third_party/flamegraph/flamegraph.pl`,
3. `flamegraph.pl` or `inferno-flamegraph` on `PATH`,
4. otherwise `scripts/flamegraph.py`, a dependency-free static renderer, with
   a note on stderr.

Use `--static` to force the Python fallback. It produces the same layout and
tooltips as the interactive SVG, but no click-to-zoom; it also accepts
`--width` and `--frame-height`, and can read folded stacks directly:

```sh
pprof --collapsed build/awr_inspect temp/oak.prof \
  | python3 scripts/flamegraph.py --width 1600 > temp/oak.svg
```

Both renderers take `--title`; it defaults to the profile's file name.

* The binary argument must be the one that produced the profile; pprof reads
  its symbols to name the frames.
* `pprof` must be on `PATH`, or point `PPROF=` at it.
* The profile is also readable with pprof itself, for example `pprof --text`
  for a flat list or `pprof --svg` for a call graph rather than a flamegraph.

## What to expect

The prebuilt OR-Tools libraries ship with symbols, so frames such as
`operations_research::sat::LinearPropagator::Propagate` and
`operations_research::glop::RevisedSimplex::...` resolve by name. On a typical
plan the CP-SAT / GLOP internals dominate, which is usually the point: it shows
how much of the run is OR-Tools search versus the hand-written graph and matrix
work. The frames for our own code are `aw::solver::*`, `aw::planCrafting`,
`aw::reachableSubgraph` and the `CraftingGraph` loaders.

Caveats worth remembering when reading a graph:

* **Inlining.** The planner is built with `-O2`, so small functions are folded
  into their callers and are attributed there. Unwinding itself is reliable —
  Ubuntu's libprofiler uses libunwind, not frame pointers. Add
  `-fno-omit-frame-pointer` to `CMAKE_CXX_FLAGS` if you want the most literal
  stacks at some runtime cost.
* **Threads.** Every search thread is sampled, so a flamegraph of a
  multi-worker run mixes them. `--workers 1` removes that noise.
* **Startup.** Because the whole run is wrapped, one-off costs (loading
  OR-Tools, building the graph) appear at the left of the graph. That is by
  design; profile a single phase by narrowing the run instead.

## Where it lives

| Piece | File |
| --- | --- |
| No-op-by-default RAII wrapper | `include/aw/Profiler.h` |
| gperftools detection and linking | `CMakeLists.txt` (`AW_BUILD_TOOLS`) |
| `--profile` argument handling | `tools/Inspect.cpp` |
| Interactive renderer (fetched) | `third_party/flamegraph/flamegraph.pl` |
| Fetch script for the renderer | `scripts/fetch-flamegraph.sh` |
| Static folded-stack to SVG renderer | `scripts/flamegraph.py` |
| `pprof` + renderer wrapper | `scripts/flamegraph.sh` |
