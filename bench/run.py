#!/usr/bin/env python3
"""Run the cross-engine benchmark sweep and write one JSONL per (dataset, config).

Configs
-------
  aw-nonopt   AW in nonoptimal mode, the cumulative ablation stages (default all
              eight; `--stages` runs a subset and merges it into the existing JSONL)
  aw-optimal  AW in optimal mode with every pruning on: the ground truth
  aw-ablation AW in OPTIMAL mode, the same cumulative ablation stages. This is the
              ablation for the exact solver: how much each reduction helps prove
              an optimum, rather than how much plan quality it costs.
  aw-flash    AW in nonoptimal mode with every pruning on; the solver stops at
              the FIRST feasible plan instead of optimizing it
  tb-v2       Thunderbolt `CraftPlannerV2` (shipped default planner)
  tb-cpsat    Thunderbolt `CpSatRankedFlowSolver` (opt-in OR-Tools planner)
  ae2vm       AE2VM `CraftingVM` driven offline, warm (>=1 prior pass)
  ae2vm-cold  AE2VM with `--warmup 0`: the first execution of every query.

Sharding
--------
`--shards N` splits each cell's target list into N disjoint plan prefixes, all
written before any worker starts (the barrier that prevents a race). `--jobs J`
runs up to J cell/shard subprocesses concurrently. The canonical
`<dataset>.<config>.jsonl` is reassembled from the shards afterwards, so every
consumer sees exactly what a single-process run would have written. With one
CP-SAT worker per shard, `--shards 16 --jobs 16` fills 16 physical cores
without the poor utilisation of a sequential 16-worker sweep, and keeps each
row deterministic. Use it on the large cells: `aw-optimal` at 32 targets x 6
amounts x 3 groups is one ~7 h job otherwise.

AE2VM is split in two on purpose: its warm path replays a memoized plan and can
return a DIFFERENT ANSWER than the cold path (see bench/README.md section 7.3).
Its warm and cold rows are not two timings of one result.

Resuming an interrupted cell
----------------------------
`--resume` restarts a sweep that was cut short (Ctrl-C, a crash, a reboot) and
pays only for the points that are still missing. Each AW child is handed the
canonical cell file as its inventory of completed points, skips exactly those,
and writes the rest into a fresh file that is then appended to the canonical
one. The header and registration rows of the original run are carried over, so
`pre_ms` keeps the value that was actually measured; a cell whose file already
holds every point exits before registration, which is the expensive half of an
AW cell. Settings that differ from the ones the existing rows were produced
with (time limit, workers, gap, stages, ...) are reported on stderr rather than
silently mixed in. AW configs only: the Java baselines have no such support.

```
python3 bench/run.py --datasets recipes-nast --configs aw-optimal --resume
```

Each dataset/config pair is one child process, so a crash or a hang only loses
that cell. Everything is logged to `bench/results/<dataset>.<config>.log`.

Typical use
-----------
  # quick smoke test on the two small corpora
  python3 bench/run.py --datasets recipes-vanilla,recipes-small

  # refresh one point of the aw-nonopt ablation without re-running the rest
  python3 bench/run.py --datasets recipes-vanilla --configs aw-nonopt --stages satellite

  # one large cell across 16 cores, one CP-SAT worker each
  python3 bench/run.py --datasets recipes-atm --configs aw-optimal \\
        --targets 32 --amounts 1,10,100,1000,10000,100000 \\
        --shards 16 --jobs 16 --workers 1 --time-limit 300

  # overnight, the real thing
  nohup python3 bench/run.py --datasets recipes-nast,recipes-atm \\
        --time-limit 40 --warmup 1 --repeats 1 > bench/results/run.log 2>&1 &

  # refresh the preprocessing half of one cell without re-running its queries
  python3 bench/run.py --datasets recipes-atm --configs tb-v2 --preprocess-only

  # pick up where an interrupted sweep stopped, instead of starting it over
  python3 bench/run.py --datasets recipes-nast --configs aw-optimal --resume

Then `python3 bench/summarize.py`.
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DATASETS = {
    "recipes-vanilla": ("temp/recipes-vanilla.awr", "temp/recipes-vanilla.names.tsv"),
    "recipes-small": ("temp/recipes-small.awr", "temp/recipes-small.names.tsv"),
    "recipes-nast": ("temp/recipes-nast.awr", "temp/recipes-nast.names.tsv"),
    "recipes-atm": ("temp/recipes-atm.awr", "temp/recipes-atm.names.tsv"),
}

# The cumulative ablation order agreed for the paper. `satellite` is the last
# stage, so it is also "everything on" in nonoptimal mode.
AW_STAGES = ["none", "seed", "direct", "recipe", "substitution", "tag", "pack",
             "satellite"]

AW_CONFIGS = ("aw-nonopt", "aw-optimal", "aw-flash", "aw-ablation")

# How each AW config drives `aw_bench`:
#   nonoptimal  registration-time switch (True = the pruned-looking model, False = exact)
#   flash       stop at the first feasible plan
#   stages      None -> the --stages selection (cumulative ladder, stage names as
#               labels); a list -> fixed `profile[:label]` specs
#   gap         relative gap; "0" for the exact modes
#   by_stage    merge fresh rows into the existing JSONL per stage instead of
#               replacing the whole file, so one ablation point can be refreshed
AW_CONFIG_PLAN = {
    "aw-nonopt": dict(nonoptimal=True, flash=False, stages=None, gap=None, by_stage=True),
    "aw-optimal": dict(nonoptimal=False, flash=False, stages=["satellite:optimal"],
                       gap="0", by_stage=False),
    "aw-flash": dict(nonoptimal=True, flash=True, stages=["satellite"], gap=None,
                     by_stage=False),
    # The cumulative ladder run in OPTIMAL mode: measures how much each reduction
    # helps the exact solver prove optima, rather than plan quality.
    "aw-ablation": dict(nonoptimal=False, flash=False, stages=None, gap="0",
                        by_stage=True),
}
TB_CONFIGS = ("tb-v2", "tb-cpsat")
AE2VM_CONFIGS = ("ae2vm", "ae2vm-cold")
ALL_CONFIGS = AW_CONFIGS + TB_CONFIGS + AE2VM_CONFIGS


def rooted(path):
    """Resolve `path` against the repo root so cwd does not matter."""
    return path if os.path.isabs(path) else os.path.join(ROOT, path)


def ensure_plan(args, dataset, awr, names):
    prefix = rooted(os.path.join(args.plan_dir, dataset))
    meta = prefix + ".meta.tsv"
    if os.path.exists(meta) and not args.regen_plan:
        return prefix
    command = [
        sys.executable, os.path.join(ROOT, "bench", "make_plan.py"),
        "--dataset", dataset,
        "--awr", awr,
        "--names", names,
        "--out-dir", rooted(args.plan_dir),
        "--seed", args.seed,
        "--targets", str(args.targets),
        "--amounts", args.amounts,
    ]
    if args.big_scc:
        command.append("--big-scc")
    if int(args.big_scc_targets) or int(args.other_targets):
        command += ["--big-scc-targets", str(args.big_scc_targets),
                    "--other-targets", str(args.other_targets)]
    if args.groups:
        command += ["--groups", args.groups]
    print("[plan] " + " ".join(command), flush=True)
    subprocess.run(command, check=True, cwd=ROOT)
    return prefix


def run_logged(name, command, log_path, cwd, dry_run):
    print("[run ] %s: %s" % (name, " ".join(command)), flush=True)
    if dry_run:
        return 0
    started = time.time()
    with open(log_path, "w", encoding="utf-8") as log:
        log.write("$ %s\n(cwd %s)\n\n" % (" ".join(command), cwd))
        log.flush()
        result = subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
    elapsed = time.time() - started
    print("[done] %s exit=%d in %.1fs -> %s" % (name, result.returncode, elapsed, log_path),
          flush=True)
    return result.returncode


def merge_stage_output(out_path, fresh_path, stages):
    """Replace the rows of `stages` in `out_path` with the fresh ones.

    `fresh_path` is a complete `aw_bench` output (config header + registration +
    query rows for the selected stages). The config header and registration row
    of the fresh run win; every query row of a re-run stage is dropped from the
    old file and the fresh rows are appended. This is what lets `--stages tag`
    refresh one ablation point without discarding the other seven.
    """
    with open(fresh_path, encoding="utf-8") as handle:
        fresh = [json.loads(line) for line in handle if line.strip()]
    header = next((row for row in fresh if row.get("type") == "config"), None)
    registration = next((row for row in fresh if row.get("type") == "registration"), None)
    fresh_queries = [row for row in fresh if row.get("type") == "query"]

    old = []
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8") as handle:
            old = [json.loads(line) for line in handle if line.strip()]
    kept = [row for row in old
            if row.get("type") not in ("config", "registration")
            and not (row.get("type") == "query" and row.get("stage") in stages)]

    # Rewrite the header's stage list so it names every stage now in the file,
    # not just the subset this invocation refreshed.
    present = set(row.get("stage") for row in kept + fresh_queries if row.get("stage"))
    if header is not None:
        header = dict(header)
        header["stages"] = ",".join([stage for stage in AW_STAGES if stage in present]
                                     + sorted(present - set(AW_STAGES)))

    ordered = ([header] if header is not None else [])
    if registration is not None:
        ordered.append(registration)
    ordered += kept
    ordered += fresh_queries

    tmp_path = out_path + ".merge.tmp"
    with open(tmp_path, "w", encoding="utf-8") as handle:
        for row in ordered:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    os.replace(tmp_path, out_path)
    os.remove(fresh_path)


def read_jsonl(path):
    """Parse a JSONL file, dropping the lines that do not parse.

    The file a resume reads may end in a torn line when the process that wrote it
    was killed mid-write, and that must not abort the merge.
    """
    rows = []
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as error:
                print("[warn] %s:%d is not valid JSON (%s); dropped" % (path, number, error),
                      flush=True)
    return rows


def row_key(row):
    """The point a query row measures: (stage, target, amount, stock, repeat).

    Returns None for a row that does not carry all five, which is how every
    non-AW row looks; those are never deduplicated.
    """
    parts = tuple(row.get(field) for field in
                  ("stage", "target", "amount", "stock", "repeat"))
    return parts if all(part is not None for part in parts) else None


def discard(paths):
    """Remove a merge's staging files; a missing one is fine.

    A staging file is left behind when a merge is interrupted between the child
    exiting and the merge starting, and the next resume reads it again.
    """
    for path in paths:
        if os.path.exists(path):
            os.remove(path)


def merge_resume_output(out_path, fresh_paths):
    """Append the rows of a resumed run to the canonical cell file.

    `fresh_paths` are the files the children just wrote (one per shard, and some
    may not exist at all). A resumed child skips every point the canonical file
    already holds and copies its header and registration rows, so the two row sets
    are disjoint and this is a plain concatenation: the old query rows first, then
    the fresh ones. A child whose cell is already complete writes nothing, which is
    not an error.
    """
    old, old_header, old_registration = [], None, None
    if os.path.exists(out_path):
        for row in read_jsonl(out_path):
            kind = row.get("type")
            if kind == "config":
                old_header = row
            elif kind == "registration":
                old_registration = row
            else:
                old.append(row)

    fresh, fresh_header, fresh_registration = [], None, None
    for path in fresh_paths:
        if not os.path.exists(path):
            continue
        for row in read_jsonl(path):
            kind = row.get("type")
            if kind == "config":
                if fresh_header is None:
                    fresh_header = row
            elif kind == "registration":
                if fresh_registration is None:
                    fresh_registration = row
            else:
                fresh.append(row)

    # One row per point is what makes a resumed file readable by the summarizer and
    # resumable again. A leftover process from the run being resumed can still write
    # into the canonical file after the child read it, so the invariant is checked
    # rather than assumed; the older measurement wins, as it would have without a
    # resume at all.
    seen = set(key for key in map(row_key, old) if key is not None)
    kept = []
    for row in fresh:
        key = row_key(row)
        if key is not None and key in seen:
            print("[warn] %s already holds %s; the fresh duplicate is dropped"
                  % (out_path, key), flush=True)
            continue
        if key is not None:
            seen.add(key)
        kept.append(row)
    fresh = kept

    if not fresh:
        print("[skip] %s already holds every point of this cell" % out_path, flush=True)
        discard(fresh_paths)
        return

    header = fresh_header if fresh_header is not None else old_header
    registration = (fresh_registration if fresh_registration is not None else old_registration)
    # Keep the header's stage list describing the file, not the subset this invocation
    # happened to extend: a resumed `--stages tag` must not relabel a file that still
    # carries the other seven stages.
    if header is not None and "stages" in header:
        present = set(row.get("stage") for row in old + fresh if row.get("stage"))
        header = dict(header)
        header["stages"] = ",".join([stage for stage in AW_STAGES if stage in present]
                                     + sorted(present - set(AW_STAGES)))

    ordered = ([header] if header is not None else [])
    if registration is not None:
        ordered.append(registration)
    ordered += old
    ordered += fresh

    tmp_path = out_path + ".merge.tmp"
    with open(tmp_path, "w", encoding="utf-8") as handle:
        for row in ordered:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    os.replace(tmp_path, out_path)
    discard(fresh_paths)


def merge_preprocess_output(out_path, fresh_path):
    """Replace the header rows of `out_path` with the fresh ones, keeping every query row.

    `fresh_path` is a `--preprocess-only` output: a config header, plus a registration
    row for AW, and no query rows at all. The old file keeps its queries (and therefore
    every `plan_ms`) and gets the new preprocessing measurement, which is the whole point
    of the mode: refreshing the preprocessing half must not cost the query half again.
    """
    with open(fresh_path, encoding="utf-8") as handle:
        fresh = [json.loads(line) for line in handle if line.strip()]
    header = next((row for row in fresh if row.get("type") == "config"), None)
    registration = next((row for row in fresh if row.get("type") == "registration"), None)

    old = []
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8") as handle:
            old = [json.loads(line) for line in handle if line.strip()]
    kept = [row for row in old if row.get("type") not in ("config", "registration")]

    # Keep the header's stage list describing the file, not the subset this invocation
    # refreshed: a `--preprocess-only --stages satellite` run must not relabel a file that
    # still carries all eight ablation stages.
    if header is not None:
        header = dict(header)
        if "stages" in header:
            present = set(row.get("stage") for row in kept if row.get("stage"))
            present.update(part for part in (header.get("stages") or "").split(",") if part)
            header["stages"] = ",".join([stage for stage in AW_STAGES if stage in present]
                                         + sorted(present - set(AW_STAGES)))

    ordered = ([header] if header is not None else [])
    if registration is not None:
        ordered.append(registration)
    ordered += kept
    if not any(row.get("type") == "query" for row in kept):
        print("[warn] %s has no query rows; only the preprocessing half is recorded"
              % out_path, flush=True)

    tmp_path = out_path + ".merge.tmp"
    with open(tmp_path, "w", encoding="utf-8") as handle:
        for row in ordered:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    os.replace(tmp_path, out_path)
    os.remove(fresh_path)


def plan_groups(prefix):
    """Inventory groups recorded in a plan's meta file."""
    with open(prefix + ".meta.tsv", encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("groups\t"):
                return [g for g in line.rstrip("\n").split("\t")[1].split(",") if g]
    return ["none", "leaves", "random20"]


def shard_path(args, dataset, k):
    return os.path.join(rooted(args.plan_dir), "%s.s%d" % (dataset, k))


def ensure_shard_plans(args, dataset, prefix, groups):
    """Split a plan's target list into `--shards` disjoint plan prefixes.

    The full plan is generated first and every shard file is written here,
    before any worker starts: that is the barrier that keeps concurrent shards
    from racing on the plan files. Only `.targets.tsv` differs between shards;
    the meta and stock files are copied so every shard sees the same inventory.
    Targets are assigned round-robin, which balances a handle-sorted list.
    """
    if args.shards <= 1:
        return [prefix]
    with open(prefix + ".targets.tsv", encoding="utf-8") as handle:
        body = [line for line in handle if line.strip() and not line.startswith("#")]
    # More shards than targets would produce empty plan files that aw_bench
    # rejects, so cap it and say so.
    shards = min(args.shards, len(body))
    if shards < args.shards:
        print("[plan] %s has %d targets; using %d shards instead of %d"
              % (dataset, len(body), shards, args.shards), flush=True)
    header = "# handle\tname\tamounts\tn_recipes\tin_big_scc\n"
    result = []
    for k in range(shards):
        target_prefix = shard_path(args, dataset, k)
        for suffix in [".meta.tsv"] + [".stock.%s.tsv" % g for g in groups]:
            shutil.copyfile(prefix + suffix, target_prefix + suffix)
        with open(target_prefix + ".targets.tsv", "w", encoding="utf-8") as handle:
            handle.write(header)
            handle.writelines(body[k::shards])
        result.append(target_prefix)
    return result


def combine_shard_output(paths, out_path):
    """Concatenate per-shard JSONLs into one file with a single header.

    The config header and registration row come from the first shard; every
    shard's query rows are appended in shard order. This reconstructs exactly
    the file a single-process run would have written.
    """
    header = None
    registration = None
    queries = []
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                if not line.strip():
                    continue
                row = json.loads(line)
                kind = row.get("type")
                if kind == "config":
                    if header is None:
                        header = row
                elif kind == "registration":
                    if registration is None:
                        registration = row
                elif kind == "query":
                    queries.append(row)
    tmp_path = out_path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as handle:
        for row in ([header] if header else []) + ([registration] if registration else []) + queries:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    os.replace(tmp_path, out_path)
    for path in paths:
        os.remove(path)


def build_aw_command(args, cell, stages):
    """The `aw_bench` command for one cell.

    `cell` is one entry of the job list built below: the dataset, the config, the
    resolved input paths, the file this child writes (`target`) and the canonical
    `<dataset>.<config>.jsonl` it belongs to (`out`).
    """
    config = cell["config"]
    plan = AW_CONFIG_PLAN[config]
    specs = plan["stages"] if plan["stages"] is not None else stages
    command = [
        os.path.join(ROOT, "build", "aw_bench"),
        "--dataset", cell["dataset"], "--awr", cell["awr"], "--plan", cell["plan_prefix"],
        "--config", config, "--out", cell["target"], "--names", cell["names"],
        "--nonoptimal", "1" if plan["nonoptimal"] else "0",
        "--warmup", "0", "--repeats", args.repeats,
        "--time-limit", args.time_limit,
        "--gap", plan["gap"] if plan["gap"] is not None else args.gap,
        "--workers", args.workers,
        "--pack-seconds", args.pack_seconds,
        "--satellite-seconds", args.satellite_seconds,
    ]
    if plan["flash"]:
        command += ["--flash", "1"]
    for spec in specs:
        command += ["--stage", spec]
    if args.trace_attempts:
        command += ["--trace-attempts"]
    if args.preprocess_only:
        command += ["--preprocess-only"]
    if args.resume:
        # The canonical cell file is the inventory of completed points; the child
        # appends only what is missing to its own `--out`.
        command += ["--resume", "1", "--resume-from", cell["out"]]
    return command


def baseline_repo(config):
    """The Gradle project the baseline `config` belongs to.

    Gradle resolves its build from the working directory (or `-p`), not from the
    path the wrapper was invoked through, so a baseline child must be started in
    its own repository directory. `run.py` itself lives one level up, and the
    historical `cwd=ROOT` made every Gradle cell fail with "does not contain a
    Gradle build".
    """
    return os.path.join(ROOT, "compare",
                        "thunderboltcore" if config.startswith("tb-") else "ae2vm")


def build_baseline_command(args, cell):
    """The historical command for the two non-AW engine families."""
    config = cell["config"]
    repo = baseline_repo(config)
    warmup = "0" if config == "ae2vm-cold" else args.warmup
    common_limit_ms = str(int(float(args.time_limit) * 1000))
    bound_ms = (common_limit_ms if args.tb_deadline_ms == "same"
                else str(int(float(args.tb_deadline_ms))))
    child = ["--dataset", cell["dataset"], "--awr", cell["awr"], "--plan", cell["plan_prefix"],
             "--config", config, "--out", cell["target"], "--names", cell["names"],
             "--warmup", warmup, "--repeats", args.repeats,
             "--deadline-ms", common_limit_ms]
    if config == "tb-cpsat":
        child += ["--bind-deadline-ms", bound_ms]
    if args.preprocess_only:
        child += ["--preprocess-only"]
    return [os.path.join(repo, "gradlew"), "--no-daemon", "benchAwr",
            "--args=" + " ".join(child)]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--datasets", default=",".join(DATASETS),
                        help="comma separated dataset names (default: all four)")
    parser.add_argument("--configs", default=",".join(ALL_CONFIGS),
                        help="comma separated configs (default: all)")
    parser.add_argument("--stages", default=",".join(AW_STAGES),
                        help="comma separated aw-nonopt ablation stages to (re)run; the "
                             "other stages' rows are kept (default: all). Use this to "
                             "refresh a single point of the cumulative curve.")
    parser.add_argument("--plan-dir", default=os.path.join("bench", "plans"))
    parser.add_argument("--out-dir", default=os.path.join("bench", "results"))
    parser.add_argument("--seed", default="20260101")
    parser.add_argument("--targets", default="8")
    parser.add_argument("--amounts", default="1,100000")
    parser.add_argument("--groups", default="",
                        help="comma separated inventory groups to emit (default: make_plan's "
                             "none,leaves,random20). Forwarded to make_plan.")
    parser.add_argument("--big-scc", action="store_true",
                        help="sample targets from the largest SCC")
    parser.add_argument("--regen-plan", action="store_true",
                        help="regenerate the plan files even if they exist")
    parser.add_argument("--preprocess-only", action="store_true",
                        help="run only each engine's one-time preprocessing (AW registration, "
                             "AE2VM pattern compilation, Thunderbolt's CP-SAT runtime init) "
                             "and merge the fresh timing into the existing JSONL, keeping "
                             "every query row. Use this to refresh `pre_ms` after a harness "
                             "change without paying for `plan_ms` again.")
    parser.add_argument("--time-limit", default="40", help="per-query cutoff in seconds")
    parser.add_argument("--warmup", default="1", help="unmeasured passes before the measured one")
    parser.add_argument("--repeats", default="1", help="measured passes")
    parser.add_argument("--workers", default="16", help="CP-SAT search threads (AW and tb-cpsat)")
    parser.add_argument("--gap", default="0.01", help="AW solver relative gap")
    parser.add_argument("--pack-seconds", default="60")
    parser.add_argument("--satellite-seconds", default="2.0",
                        help="satellite pass budget; the library default is 0.05, which makes "
                             "the ablation stage timing-dependent")
    parser.add_argument("--tb-deadline-ms", default="same",
                        help="planning deadline bound for Thunderbolt's CP-SAT config. "
                             "'same' binds the common --time-limit so all engines share one "
                             "wall clock; '0' leaves Thunderbolt's own bounds in place (3 s "
                             "per native solve call, which is what a direct caller gets)")
    parser.add_argument("--shards", type=int, default=1,
                        help="split each cell's target list into this many disjoint shards. "
                             "The full plan is written first, so shards never race on it. "
                             "Use with --jobs to parallelize one large cell across cores. "
                             "AW configs only.")
    parser.add_argument("--jobs", type=int, default=1,
                        help="run up to this many cell/shard subprocesses concurrently. "
                             "Pairs with --shards: total workers = cells x shards.")
    parser.add_argument("--trace-attempts", action="store_true",
                        help="record the per-attempt objective-cap trajectory (cap, budget, "
                             "status, objective, elapsed) on every query row. Use it on the "
                             "ablation subset only; it grows the JSONL.")
    parser.add_argument("--big-scc-targets", default="0",
                        help="draw this many targets from the largest SCC; combined with "
                             "--other-targets this is the mixed adversarial/typical panel")
    parser.add_argument("--other-targets", default="0",
                        help="draw this many targets from outside the largest SCC")
    parser.add_argument("--resume", action="store_true",
                        help="restart an interrupted sweep: skip the query points already "
                             "recorded in each cell's JSONL, keep its header and "
                             "registration row, and append only what is missing. AW configs "
                             "only. A cell whose file is already complete is left untouched "
                             "instead of paying for its registration again.")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    datasets = [d for d in args.datasets.split(",") if d]
    configs = set(c for c in args.configs.split(",") if c)
    unknown = configs - set(ALL_CONFIGS)
    if unknown:
        raise SystemExit("unknown config(s): %s" % ", ".join(sorted(unknown)))

    stages = [s for s in args.stages.split(",") if s]
    unknown_stages = set(stages) - set(AW_STAGES)
    if unknown_stages:
        raise SystemExit("unknown stage(s): %s" % ", ".join(sorted(unknown_stages)))
    if not stages:
        raise SystemExit("--stages must list at least one stage")

    if args.shards < 1:
        raise SystemExit("--shards must be >= 1")
    if args.jobs < 1:
        raise SystemExit("--jobs must be >= 1")
    if args.preprocess_only and args.shards > 1:
        raise SystemExit("--preprocess-only cannot be combined with --shards > 1")
    if args.resume:
        unsupported = sorted(configs - set(AW_CONFIG_PLAN))
        if unsupported:
            raise SystemExit("--resume is only implemented for the AW configs (%s), not %s"
                             % (", ".join(sorted(AW_CONFIG_PLAN)), ", ".join(unsupported)))
        if args.preprocess_only:
            raise SystemExit("--resume cannot be combined with --preprocess-only")

    os.makedirs(rooted(args.out_dir), exist_ok=True)
    os.makedirs(rooted(args.plan_dir), exist_ok=True)

    # ---- plan barrier ---------------------------------------------------
    # Generate every full plan and every shard file before any worker starts.
    # That is what keeps concurrent shards from racing on the plan directory.
    plans = {}
    for dataset in datasets:
        awr, names = DATASETS[dataset]
        full = ensure_plan(args, dataset, awr, names)
        plans[dataset] = ensure_shard_plans(args, dataset, full, plan_groups(full))

    # ---- job list -------------------------------------------------------
    jobs = []
    for dataset in datasets:
        awr, names = DATASETS[dataset]
        awr = rooted(awr)
        names = rooted(names)
        for config in ALL_CONFIGS:
            if config not in configs:
                continue
            shard_prefixes = plans[dataset]
            if config not in AW_CONFIG_PLAN and len(shard_prefixes) > 1:
                raise SystemExit("--shards > 1 is only supported for AW configs, not %s"
                                 % config)
            out = os.path.join(ROOT, args.out_dir, "%s.%s.jsonl" % (dataset, config))
            single = len(shard_prefixes) == 1
            for k, plan_prefix in enumerate(shard_prefixes):
                if args.preprocess_only:
                    log = out + ".preprocess.log"
                    target = out + ".preprocess"
                elif single:
                    log = out + ".log"
                    # A resumed or per-stage run must not truncate the canonical file
                    # before it has read what is already in it.
                    if args.resume or (config in AW_CONFIG_PLAN
                                       and AW_CONFIG_PLAN[config]["by_stage"]):
                        target = out + ".fresh"
                    else:
                        target = out
                else:
                    log = out + ".shard%d.log" % k
                    target = out + ".shard%d.fresh" % k
                cell = dict(dataset=dataset, config=config, awr=awr, names=names,
                            plan_prefix=plan_prefix, target=target, out=out, shard=k,
                            name="%s/%s#%d" % (dataset, config, k), log=log,
                            # AW's native tool does not care about the cwd (every input
                            # is absolute); a Gradle cell does, and only works in its
                            # own repository. See baseline_repo.
                            cwd=(ROOT if config in AW_CONFIG_PLAN
                                 else baseline_repo(config)))
                cell["command"] = (build_aw_command(args, cell, stages)
                                   if config in AW_CONFIG_PLAN
                                   else build_baseline_command(args, cell))
                jobs.append(cell)

    # ---- run ------------------------------------------------------------
    if args.dry_run:
        for job in jobs:
            run_logged(job["name"], job["command"], job["log"], job["cwd"], True)
        return 0

    if args.jobs <= 1:
        for job in jobs:
            job["code"] = run_logged(job["name"], job["command"], job["log"], job["cwd"], False)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(run_logged, job["name"], job["command"], job["log"],
                                   job["cwd"], False): job for job in jobs}
            for future in concurrent.futures.as_completed(futures):
                futures[future]["code"] = future.result()

    # ---- merge shards into one file per (dataset, config) ---------------
    failures = []
    cells = {}
    for job in jobs:
        cells.setdefault((job["dataset"], job["config"]), []).append(job)

    for (dataset, config), group in cells.items():
        group.sort(key=lambda job: job["shard"])
        out = group[0]["out"]
        failed = [job for job in group if job["code"] != 0]
        if failed:
            failures += ["%s/%s#%d" % (job["dataset"], job["config"], job["shard"])
                         for job in failed]
            continue
        plan = AW_CONFIG_PLAN.get(config)
        if args.preprocess_only:
            merge_preprocess_output(out, group[0]["target"])
        elif args.resume:
            merge_resume_output(out, [job["target"] for job in group])
        elif len(group) == 1:
            if plan is not None and plan["by_stage"]:
                merge_stage_output(out, group[0]["target"], stages)
        else:
            combined = out + ".combined"
            combine_shard_output([job["target"] for job in group], combined)
            if plan is not None and plan["by_stage"]:
                merge_stage_output(out, combined, stages)
            else:
                os.replace(combined, out)

    print()
    if failures:
        print("FAILED cells: %s" % ", ".join(failures))
    else:
        print("all cells finished")
    print("next: python3 bench/summarize.py")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
