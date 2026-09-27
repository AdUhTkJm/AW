#!/usr/bin/env python3
"""Turn the benchmark JSONL files into per-cell and per-config tables.

Reads every `bench/results/*.jsonl` produced by `bench/run.py` (or by any of the
three harnesses run by hand) and writes:

    bench/results/summary.cells.csv   one row per (dataset, config, stage, target, amount, stock)
    bench/results/summary.csv         one row per (dataset, config, stage)
    bench/results/summary.md          the same summary as a readable markdown table

Ground truth is `aw-optimal` (AW in optimal mode, every pruning on). Where that
row is `infeasible` or `ok` it is taken as authoritative; `iter_limit` and
`gave_up` rows are inconclusive and excluded from the agreement tallies. Cost
comparisons additionally require `proven_optimal`, so a row AW gave up on never
makes a baseline look better or worse than it can prove.

The `balance_ok` column is an independent recomputation of
`produced - consumed >= rhs` from the engine's own firing vector. `false_positive`
is `feasible and not balance_ok`: a plan the engine called complete that does not
actually balance. That is a hard error no timing analysis can reveal.
"""

import argparse
import collections
import csv
import glob
import json
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CONFIG_ORDER = ["aw-optimal", "aw-nonopt", "tb-v2", "tb-cpsat", "ae2vm", "ae2vm-cold"]
STAGE_ORDER = ["optimal", "none", "dead_node", "direct", "recipe", "substitution", "tag",
               "pack", "satellite", "-"]

# Statuses that mean "the engine produced a usable answer".
ANSWERED = {"ok", "missing", "infeasible"}

# `optimality` is the engine's own claim about a row:
#   proven    an optimal plan (AW `ok`+proven_optimal, TB CP-SAT solved without a budget cut)
#   unproven  a real plan with no proof -- AW reports the gap, TB reports budget_exhausted
#   unclaimed the engine has no optimality notion at all (CraftPlannerV2, AE2VM)
#   no_plan   nothing to be optimal about (infeasible, declined, no answer)
OPTIMALITY = ("proven", "unproven", "unclaimed", "no_plan")


def optimality_of(row):
    """New rows carry `optimality`; older ones are derived from status/proven_optimal."""
    if row.get("optimality"):
        return row["optimality"]
    if row.get("status") in ("ok", "missing"):
        return "proven" if row.get("proven_optimal") is True else "unclaimed"
    return "no_plan"


def no_answer_of(row):
    """`no_answer` replaced the old, misleadingly named `gave_up`."""
    if row.get("no_answer") is not None:
        return bool(row["no_answer"])
    if row.get("gave_up") is not None:
        return bool(row["gave_up"])
    return row.get("status") not in ANSWERED


def plan_ms_of(row):
    """Per-query planning time under one name across engines.

    The baselines emit `plan_ms` (their whole planning call). AW splits the
    same quantity into `reach_ms` + `solve_ms` and emits `query_ms` for the
    sum; see bench/README.md section 5.2. Normalising here keeps the `plan_ms`
    column comparable instead of silently reading 0.0 for every AW row.
    """
    if row.get("plan_ms") is not None:
        return row["plan_ms"]
    if row.get("query_ms") is not None:
        return row["query_ms"]
    return (row.get("reach_ms", 0.0) or 0.0) + (row.get("solve_ms", 0.0) or 0.0)


def load_rows(results_dir):
    rows = []
    malformed = []
    for path in sorted(glob.glob(os.path.join(results_dir, "*.jsonl"))):
        with open(path, encoding="utf-8") as handle:
            for number, line in enumerate(handle, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as error:
                    malformed.append((path, number, str(error)))
    return rows, malformed


def stage_key(config, stage):
    if config == "aw-optimal":
        return "optimal"
    return stage


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results-dir", default=os.path.join(ROOT, "bench", "results"))
    parser.add_argument("--out-prefix", default=None,
                        help="output prefix (default <results-dir>/summary)")
    parser.add_argument("--control", default="aw-optimal",
                        help="config used as ground truth (default aw-optimal)")
    args = parser.parse_args()

    prefix = args.out_prefix or os.path.join(args.results_dir, "summary")
    rows, malformed = load_rows(args.results_dir)
    for path, number, error in malformed:
        print("malformed line: %s:%d: %s" % (path, number, error), file=sys.stderr)

    configs = [r for r in rows if r.get("type") == "config"]
    registrations = [r for r in rows if r.get("type") == "registration"]
    queries = [r for r in rows if r.get("type") == "query"]
    if not queries:
        raise SystemExit("no query rows found in %s" % args.results_dir)

    preprocess = {}
    for row in configs:
        preprocess[(row["dataset"], row["config"])] = row.get("preprocess_ms")
    for row in registrations:
        preprocess[(row["dataset"], row["config"])] = row.get("register_ms")

    # ---- ground truth index ------------------------------------------------
    truth = {}
    for row in queries:
        if row["config"] != args.control:
            continue
        stage = stage_key(row["config"], row["stage"])
        if stage != "optimal":
            continue
        truth[(row["dataset"], row["target"], row["amount"], row["stock"], row.get("repeat", 0))] = row

    # ---- per-cell table ---------------------------------------------------
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    with open(prefix + ".cells.csv", "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "dataset", "config", "stage", "target", "target_name", "amount", "stock", "repeat",
            "cold", "status", "optimality", "has_plan", "no_answer", "feasible", "balance_ok",
            "false_positive", "cost", "missing_count", "gap", "bound",
            "plan_ms", "items", "recipes", "items_processed", "conflicts", "branches",
            "truth_status", "truth_feasible", "truth_cost", "truth_proven",
            "agrees_with_truth", "cost_ratio_vs_truth"])
        for row in sorted(queries, key=lambda r: (r["dataset"], r["config"],
                                                  stage_key(r["config"], r["stage"]),
                                                  r["target"], r["amount"], r["stock"],
                                                  r.get("repeat", 0))):
            key = (row["dataset"], row["target"], row["amount"], row["stock"], row.get("repeat", 0))
            reference = truth.get(key)
            truth_feasible = reference.get("feasible") if reference else None
            truth_status = reference.get("status") if reference else None
            conclusive = reference is not None and reference.get("status") in ("ok", "infeasible")
            agrees = None
            if conclusive and row.get("status") in ("ok", "missing", "infeasible"):
                agrees = bool(row.get("feasible")) == bool(truth_feasible)
            ratio = None
            truth_proven = bool(reference.get("proven_optimal")) if reference else False
            if (ratio is None and truth_proven and truth_feasible and row.get("feasible")
                    and reference.get("cost") and row.get("cost") is not None):
                ratio = row["cost"] / reference["cost"]
            writer.writerow([
                row["dataset"], row["config"], stage_key(row["config"], row["stage"]),
                row["target"], row.get("target_name", ""), row["amount"], row["stock"],
                row.get("repeat", 0), row.get("cold"), row.get("status"),
                optimality_of(row), row.get("has_plan"), no_answer_of(row),
                row.get("feasible"),
                row.get("balance_ok"), row.get("false_positive"), row.get("cost"),
                row.get("missing_count"), row.get("gap"), row.get("bound"),
                "%.3f" % plan_ms_of(row),
                row.get("items", 0), row.get("recipes", 0), row.get("items_processed", 0),
                row.get("conflicts", 0), row.get("branches", 0),
                truth_status, truth_feasible, reference.get("cost") if reference else None,
                truth_proven, agrees,
                "%.4f" % ratio if ratio is not None else ""])

    # ---- per-config/stage summary ----------------------------------------
    groups = collections.defaultdict(list)
    for row in queries:
        groups[(row["dataset"], row["config"], stage_key(row["config"], row["stage"]))].append(row)

    summary_rows = []
    for (dataset, config, stage), items in sorted(groups.items()):
        repeats = collections.Counter(r.get("repeat", 0) for r in items)
        answered = [r for r in items if r.get("status") in ANSWERED]
        feasible = [r for r in items if r.get("feasible")]
        claims = collections.Counter(optimality_of(r) for r in items)
        statuses = collections.Counter(r.get("status") for r in items)
        gaps = [r["gap"] for r in items
                if r.get("gap") is not None and optimality_of(r) == "unproven"]
        false_positives = [r for r in items
                           if r.get("feasible") and r.get("balance_ok") is False]
        conclusive = [(r, truth[(r["dataset"], r["target"], r["amount"], r["stock"],
                                 r.get("repeat", 0))])
                      for r in items
                      if (r["dataset"], r["target"], r["amount"], r["stock"], r.get("repeat", 0))
                      in truth
                      and truth[(r["dataset"], r["target"], r["amount"], r["stock"],
                                 r.get("repeat", 0))].get("status") in ("ok", "infeasible")
                      and r.get("status") in ANSWERED]
        disagreements = [(r, t) for (r, t) in conclusive
                         if bool(r.get("feasible")) != bool(t.get("feasible"))]
        false_negative = [(r, t) for (r, t) in disagreements if not r.get("feasible")]
        false_positive_vs_truth = [(r, t) for (r, t) in disagreements if r.get("feasible")]
        ratios = []
        for r in feasible:
            reference = truth.get((r["dataset"], r["target"], r["amount"], r["stock"],
                                   r.get("repeat", 0)))
            if (reference is not None and reference.get("proven_optimal")
                    and reference.get("feasible") and reference.get("cost")):
                ratios.append(r["cost"] / reference["cost"])
        plan_times = [plan_ms_of(r) for r in items]
        feasible_times = [plan_ms_of(r) for r in feasible]
        summary_rows.append({
            "dataset": dataset,
            "config": config,
            "stage": stage,
            "queries": len(items),
            "repeats": max(repeats) + 1 if repeats else 1,
            "feasible": len(feasible),
            "infeasible": statuses.get("infeasible", 0),
            "missing": statuses.get("missing", 0),
            "plans": sum(1 for r in items if r.get("has_plan") or r.get("status") in ("ok", "missing")),
            "proven": claims.get("proven", 0),
            "unproven": claims.get("unproven", 0),
            "unclaimed": claims.get("unclaimed", 0),
            "no_answer": sum(1 for r in items if no_answer_of(r)),
            "timeout": sum(1 for r in items if r.get("timeout")),
            "gap_p50": statistics.median(gaps) if gaps else None,
            "gap_max": max(gaps) if gaps else None,
            "no_answer": len(items) - len(answered),
            "false_positive": len(false_positives),
            "vs_truth_compared": len(conclusive),
            "vs_truth_agree": len(conclusive) - len(disagreements),
            "vs_truth_false_positive": len(false_positive_vs_truth),
            "vs_truth_false_negative": len(false_negative),
            "preprocess_ms": preprocess.get((dataset, config)),
            "plan_ms_median": statistics.median(plan_times) if plan_times else None,
            "plan_ms_mean": statistics.fmean(plan_times) if plan_times else None,
            "plan_ms_feasible_median": (statistics.median(feasible_times)
                                        if feasible_times else None),
            "items_median": (statistics.median([r.get("items", 0) for r in items])
                             if items else None),
            "recipes_median": (statistics.median([r.get("recipes", 0) for r in items])
                               if items else None),
            "conflicts_median": (statistics.median([r.get("conflicts", 0) for r in items])
                                 if items else None),
            "branches_median": (statistics.median([r.get("branches", 0) for r in items])
                                if items else None),
            "cost_ratio_median": statistics.median(ratios) if ratios else None,
            "cost_ratio_worst": max(ratios) if ratios else None,
        })

    fields = list(summary_rows[0].keys())
    with open(prefix + ".csv", "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in summary_rows:
            writer.writerow(row)

    with open(prefix + ".md", "w", encoding="utf-8") as handle:
        write_markdown(handle, summary_rows)

    render(summary_rows)
    print()
    print("wrote %s.cells.csv, %s.csv, %s.md" % (prefix, prefix, prefix))
    for path, number, error in malformed:
        print("WARNING malformed: %s:%d %s" % (path, number, error), file=sys.stderr)


def fmt(value, digits=1):
    if value is None:
        return "-"
    if isinstance(value, float):
        return ("%%.%df" % digits) % value
    return str(value)


def render(summary_rows):
    by_dataset = collections.defaultdict(list)
    for row in summary_rows:
        by_dataset[row["dataset"]].append(row)
    for dataset in sorted(by_dataset):
        print()
        print("== %s ==" % dataset)
        print("  optimality: prov=proven optimal, unprov=plan without a proof (AW gap / TB"
              " budget cut),")
        print("              uncl=engine has no optimality notion, noans=no answer at all"
              " (not merely unproven)")
        print("%-11s %-12s %4s %5s %6s %6s %6s %5s %4s %7s %7s %8s %9s %7s %8s %7s %8s"
              % ("config", "stage", "n", "feas", "prov", "unprov", "uncl", "noans", "tout",
                 "vs_ok", "fp_vs", "gapmax", "cost_rat", "pre_ms", "plan_ms", "items",
                 "recipes"))
        for row in sorted(by_dataset[dataset],
                          key=lambda r: (CONFIG_ORDER.index(r["config"])
                                         if r["config"] in CONFIG_ORDER else 99,
                                         STAGE_ORDER.index(r["stage"])
                                         if r["stage"] in STAGE_ORDER else 99)):
            print("%-11s %-12s %4d %5d %6d %6d %6d %5d %4d %7s %7d %8s %9s %7s %8s %7s %8s"
                  % (row["config"], row["stage"], row["queries"], row["feasible"],
                     row["proven"], row["unproven"], row["unclaimed"], row["no_answer"],
                     row["timeout"],
                     "%d/%d" % (row["vs_truth_agree"], row["vs_truth_compared"]),
                     row["vs_truth_false_positive"],
                     fmt(row["gap_max"], 4), fmt(row["cost_ratio_median"], 3),
                     fmt(row["preprocess_ms"]), fmt(row["plan_ms_median"]),
                     fmt(row["items_median"], 0), fmt(row["recipes_median"], 0)))


def write_markdown(handle, summary_rows):
    by_dataset = collections.defaultdict(list)
    for row in summary_rows:
        by_dataset[row["dataset"]].append(row)
    for dataset in sorted(by_dataset):
        handle.write("## %s\n\n" % dataset)
        handle.write("`prov` = proven optimal, `unprov` = real plan without a proof (AW gap, "
                     "TB budget cut), "
                     "`uncl` = engine has no optimality notion, `noans` = no answer at all.\n\n")
        handle.write("| config | stage | n | feasible | prov | unprov | uncl | noans | timeout | "
                     "false-pos | vs truth | FP vs truth | FN vs truth | gap max | cost ratio | "
                     "preprocess ms | plan ms | items | recipes |\n")
        handle.write("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
                     "---:|---:|---:|---:|\n")
        for row in sorted(by_dataset[dataset],
                          key=lambda r: (CONFIG_ORDER.index(r["config"])
                                         if r["config"] in CONFIG_ORDER else 99,
                                         STAGE_ORDER.index(r["stage"])
                                         if r["stage"] in STAGE_ORDER else 99)):
            handle.write("| %s | %s | %d | %d | %d | %d | %d | %d | %d | %d | %d/%d | %d | %d | %s | %s | %s | %s | %s | %s |\n"
                         % (row["config"], row["stage"], row["queries"], row["feasible"],
                            row["proven"], row["unproven"], row["unclaimed"], row["no_answer"],
                            row["timeout"], row["false_positive"],
                            row["vs_truth_agree"], row["vs_truth_compared"],
                            row["vs_truth_false_positive"], row["vs_truth_false_negative"],
                            fmt(row["gap_max"], 4), fmt(row["cost_ratio_median"], 3),
                            fmt(row["preprocess_ms"]), fmt(row["plan_ms_median"]),
                            fmt(row["items_median"], 0), fmt(row["recipes_median"], 0)))
        handle.write("\n")


if __name__ == "__main__":
    main()
