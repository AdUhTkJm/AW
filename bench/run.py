#!/usr/bin/env python3
"""Run the cross-engine benchmark sweep and write one JSONL per (dataset, config).

Configs
-------
  aw-nonopt   AW in nonoptimal mode, all eight cumulative ablation stages
  aw-optimal  AW in optimal mode with every pruning on: the ground truth
  tb-v2       Thunderbolt `CraftPlannerV2` (shipped default planner)
  tb-cpsat    Thunderbolt `CpSatRankedFlowSolver` (opt-in OR-Tools planner)
  ae2vm       AE2VM `CraftingVM` driven offline, warm (>=1 prior pass)
  ae2vm-cold  AE2VM with `--warmup 0`: the first execution of every query.

AE2VM is split in two on purpose: its warm path replays a memoized plan and can
return a DIFFERENT ANSWER than the cold path (see bench/README.md section 7.3).
Its warm and cold rows are not two timings of one result.

Each dataset/config pair is one child process, so a crash or a hang only loses
that cell. Everything is logged to `bench/results/<dataset>.<config>.log`.

Typical use
-----------
  # quick smoke test on the two small corpora
  python3 bench/run.py --datasets recipes-vanilla,recipes-small

  # overnight, the real thing
  nohup python3 bench/run.py --datasets recipes-nast,recipes-atm \\
        --time-limit 40 --warmup 1 --repeats 1 > bench/results/run.log 2>&1 &

Then `python3 bench/summarize.py`.
"""

import argparse
import os
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
AW_STAGES = ["none", "dead_node", "direct", "recipe", "substitution", "tag", "pack",
             "satellite"]

AW_CONFIGS = ("aw-nonopt", "aw-optimal")
TB_CONFIGS = ("tb-v2", "tb-cpsat")
AE2VM_CONFIGS = ("ae2vm", "ae2vm-cold")
ALL_CONFIGS = AW_CONFIGS + TB_CONFIGS + AE2VM_CONFIGS


def ensure_plan(args, dataset, awr, names):
    prefix = os.path.join(args.plan_dir, dataset)
    meta = prefix + ".meta.tsv"
    if os.path.exists(meta) and not args.regen_plan:
        return prefix
    command = [
        sys.executable, os.path.join(ROOT, "bench", "make_plan.py"),
        "--dataset", dataset,
        "--awr", awr,
        "--names", names,
        "--out-dir", args.plan_dir,
        "--seed", args.seed,
        "--targets", str(args.targets),
        "--amounts", args.amounts,
    ]
    if args.big_scc:
        command.append("--big-scc")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--datasets", default=",".join(DATASETS),
                        help="comma separated dataset names (default: all four)")
    parser.add_argument("--configs", default=",".join(ALL_CONFIGS),
                        help="comma separated configs (default: all)")
    parser.add_argument("--plan-dir", default=os.path.join("bench", "plans"))
    parser.add_argument("--out-dir", default=os.path.join("bench", "results"))
    parser.add_argument("--seed", default="20260101")
    parser.add_argument("--targets", default="8")
    parser.add_argument("--amounts", default="1,100000")
    parser.add_argument("--big-scc", action="store_true",
                        help="sample targets from the largest SCC")
    parser.add_argument("--regen-plan", action="store_true",
                        help="regenerate the plan files even if they exist")
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
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    datasets = [d for d in args.datasets.split(",") if d]
    configs = set(c for c in args.configs.split(",") if c)
    unknown = configs - set(ALL_CONFIGS)
    if unknown:
        raise SystemExit("unknown config(s): %s" % ", ".join(sorted(unknown)))

    os.makedirs(args.out_dir, exist_ok=True)
    os.makedirs(args.plan_dir, exist_ok=True)

    if args.dry_run:
        for dataset in datasets:
            awr, names = DATASETS[dataset]
            ensure_plan(args, dataset, awr, names)
    else:
        for dataset in datasets:
            awr, names = DATASETS[dataset]
            ensure_plan(args, dataset, awr, names)

    common_limit_ms = str(int(float(args.time_limit) * 1000))
    bound_ms = (common_limit_ms if args.tb_deadline_ms == "same"
                else str(int(float(args.tb_deadline_ms))))

    failures = []
    for dataset in datasets:
        awr, names = DATASETS[dataset]
        awr = os.path.join(ROOT, awr) if not os.path.isabs(awr) else awr
        names = os.path.join(ROOT, names) if not os.path.isabs(names) else names
        prefix = os.path.join(ROOT, args.plan_dir, dataset)

        for config in ALL_CONFIGS:
            if config not in configs:
                continue
            out = os.path.join(ROOT, args.out_dir, "%s.%s.jsonl" % (dataset, config))
            log = os.path.join(ROOT, args.out_dir, "%s.%s.log" % (dataset, config))

            if config == "aw-nonopt":
                command = [os.path.join(ROOT, "build", "aw_bench"),
                           "--dataset", dataset, "--awr", awr, "--plan", prefix,
                           "--config", config, "--out", out, "--names", names,
                           "--nonoptimal", "1",
                           "--warmup", args.warmup, "--repeats", args.repeats,
                           "--time-limit", args.time_limit, "--gap", args.gap,
                           "--workers", args.workers, "--pack-seconds", args.pack_seconds,
                           "--satellite-seconds", args.satellite_seconds]
                for stage in AW_STAGES:
                    command += ["--stage", stage]
                code = run_logged(config, command, log, ROOT, args.dry_run)

            elif config == "aw-optimal":
                command = [os.path.join(ROOT, "build", "aw_bench"),
                           "--dataset", dataset, "--awr", awr, "--plan", prefix,
                           "--config", config, "--out", out, "--names", names,
                           "--nonoptimal", "0", "--stage", "satellite:optimal",
                           "--warmup", args.warmup, "--repeats", args.repeats,
                           "--time-limit", args.time_limit, "--gap", args.gap,
                           "--workers", args.workers, "--pack-seconds", args.pack_seconds,
                           "--satellite-seconds", args.satellite_seconds]
                code = run_logged(config, command, log, ROOT, args.dry_run)

            else:
                repo = os.path.join(ROOT, "compare",
                                    "thunderboltcore" if config.startswith("tb-") else "ae2vm")
                # `ae2vm-cold` is the same harness with no warm-up pass at all.
                warmup = "0" if config == "ae2vm-cold" else args.warmup
                child = ["--dataset", dataset, "--awr", awr, "--plan", prefix,
                         "--config", config, "--out", out, "--names", names,
                         "--warmup", warmup, "--repeats", args.repeats,
                         "--deadline-ms", common_limit_ms]
                if config == "tb-cpsat":
                    child += ["--bind-deadline-ms", bound_ms]
                command = [os.path.join(repo, "gradlew"), "--no-daemon", "benchAwr",
                           "--args=" + " ".join(child)]
                code = run_logged(config, command, log, repo, args.dry_run)

            if code != 0:
                failures.append("%s/%s" % (dataset, config))

    print()
    if failures:
        print("FAILED cells: %s" % ", ".join(failures))
    else:
        print("all cells finished")
    print("next: python3 bench/summarize.py")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
