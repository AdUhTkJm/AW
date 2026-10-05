#!/usr/bin/env python3
"""Turn the benchmark JSONL files into per-cell and per-config tables.

Reads every `bench/results/*.jsonl` produced by `bench/run.py` (or by any of the
three harnesses run by hand) and writes:

    bench/results/summary.cells.csv   one row per (dataset, config, stage, target, amount, stock)
    bench/results/summary.csv         one row per (dataset, config, stage, stock)
    bench/results/summary.md          the same summary as a readable markdown table

`--group-inventory` does not change the rows; it makes the summary split into one
table per inventory setting (`none`, `leaves`, `random20`) instead of pooling them.

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

CONFIG_ORDER = ["aw-optimal", "aw-nonopt", "aw-flash", "tb-v2", "tb-cpsat", "ae2vm", "ae2vm-cold"]
STAGE_ORDER = ["optimal", "none", "dead_node", "seed", "direct", "recipe", "substitution", "tag",
               "pack", "satellite"]
STOCK_ORDER = ["none", "leaves", "random20"]

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


def aggregate(values, mode):
    """Central/worst statistic selected by --mean/--max (`median` by default)."""
    if not values:
        return None
    if mode == "mean":
        return statistics.fmean(values)
    if mode == "max":
        return max(values)
    return statistics.median(values)


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


def summary_sort_key(row):
    return (row["dataset"],
            CONFIG_ORDER.index(row["config"]) if row["config"] in CONFIG_ORDER else 99,
            STAGE_ORDER.index(row["stage"]) if row["stage"] in STAGE_ORDER else 99,
            STOCK_ORDER.index(row["stock"]) if row["stock"] in STOCK_ORDER else 99)


def stock_chunks(rows, group_inventory):
    """Split a dataset's rows into one chunk per inventory setting, or one chunk."""
    if not group_inventory:
        return [(None, rows)]
    by_stock = collections.defaultdict(list)
    for row in rows:
        by_stock[row["stock"]].append(row)
    return [(stock, by_stock[stock])
            for stock in sorted(by_stock,
                                key=lambda s: STOCK_ORDER.index(s) if s in STOCK_ORDER else 99)]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results-dir", default=os.path.join(ROOT, "bench", "results"))
    parser.add_argument("--out-prefix", default=None,
                        help="output prefix (default <results-dir>/summary)")
    parser.add_argument("--control", default="aw-optimal",
                        help="config used as ground truth (default aw-optimal)")
    parser.add_argument("--mean", action="store_true",
                        help="report the mean instead of the median for every aggregated "
                             "column (plan_ms, plan_ms_feasible, gap, items, recipes, "
                             "conflicts, branches, cost_ratio)")
    parser.add_argument("--max", action="store_true",
                        help="report the maximum instead of the median for every "
                             "aggregated column")
    parser.add_argument("--group-inventory", action="store_true",
                        help="split each dataset's summary into one table per inventory "
                             "setting (stock group: none, leaves, random20) instead of "
                             "pooling all three")
    args = parser.parse_args()

    if args.mean and args.max:
        raise SystemExit("--mean and --max are mutually exclusive")
    mode = "mean" if args.mean else ("max" if args.max else "median")
    tag = mode  # suffix for every aggregated summary column

    prefix = args.out_prefix or os.path.join(args.results_dir, "summary")
    rows, malformed = load_rows(args.results_dir)
    for path, number, error in malformed:
        print("malformed line: %s:%d: %s" % (path, number, error), file=sys.stderr)

    configs = [r for r in rows if r.get("type") == "config"]
    registrations = [r for r in rows if r.get("type") == "registration"]
    queries = [r for r in rows if r.get("type") == "query"]
    if not queries:
        raise SystemExit("no query rows found in %s" % args.results_dir)

    # ---- preprocessing index ----------------------------------------------
    # `preprocess_ms` is engine-only for anything written by the current harnesses: they mark
    # their headers with `preprocess_scope: "engine"` and report the harness' own input
    # preparation separately as `parse_ms`. A header without that marker predates the split,
    # so its `preprocess_ms` still contains the harness parse, and for `tb-cpsat` the native
    # runtime init was reported in its own field; fold that in so old and new cells agree on
    # what the column means. AW never had this problem: `register_ms` is `registerCraftingGraph`
    # alone, and the `.awr` read happens outside its timer.
    preprocess = {}
    parse = {}
    legacy_headers = set()
    for row in configs:
        key = (row["dataset"], row["config"])
        value = row.get("preprocess_ms")
        if row.get("preprocess_scope") != "engine":
            legacy_headers.add(key)
            if row["config"] == "tb-cpsat":
                value = (value or 0.0) + (row.get("cp_sat_init_ms") or 0.0)
        preprocess[key] = value
        if row.get("parse_ms") is not None:
            parse[key] = row["parse_ms"]
    for row in registrations:
        key = (row["dataset"], row["config"])
        preprocess[key] = row.get("register_ms")
        if row.get("parse_ms") is not None:
            parse[key] = row["parse_ms"]
    if legacy_headers:
        print("note: %d pre-split header(s) (no preprocess_scope=engine): their pre_ms still "
              "contains the harness parse. Re-run `bench/run.py --preprocess-only` on those "
              "cells to refresh it.%s"
              % (len(legacy_headers),
                 " tb-cpsat cp_sat_init_ms was folded in."
                 if any(c == "tb-cpsat" for _, c in legacy_headers) else ""),
              file=sys.stderr)

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
            "lp_ms", "probe_ms", "cap_attempts", "deterministic_time",
            "first_feasible_ms", "proven_ms", "solver_retries",
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
                row.get("lp_ms"), row.get("probe_ms"),
                row.get("cap_attempts"), row.get("deterministic_time"),
                row.get("first_feasible_ms"), row.get("proven_ms"),
                row.get("solver_retries"),
                truth_status, truth_feasible, reference.get("cost") if reference else None,
                truth_proven, agrees,
                "%.4f" % ratio if ratio is not None else ""])

    # ---- per-config/stage summary ----------------------------------------
    groups = collections.defaultdict(list)
    for row in queries:
        key = (row["dataset"], row["config"], stage_key(row["config"], row["stage"]))
        if args.group_inventory:
            key = key + (row["stock"],)
        groups[key].append(row)

    summary_rows = []
    for key, items in groups.items():
        dataset, config, stage = key[:3]
        stock = key[3] if args.group_inventory else "-"
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
        better = [(r, t) for (r, t) in disagreements if r.get("feasible") and r.get("balance_ok")]
        ratios = []
        for r in feasible:
            reference = truth.get((r["dataset"], r["target"], r["amount"], r["stock"],
                                   r.get("repeat", 0)))
            if (reference is not None and reference.get("proven_optimal")
                    and reference.get("feasible") and reference.get("cost")):
                ratios.append(r["cost"] / reference["cost"])
        plan_times = [plan_ms_of(r) for r in items]
        feasible_times = [plan_ms_of(r) for r in feasible]
        item_counts = [r.get("items", 0) for r in items]
        recipe_counts = [r.get("recipes", 0) for r in items]
        conflict_counts = [r.get("conflicts", 0) for r in items]
        branch_counts = [r.get("branches", 0) for r in items]
        # Solution-finding times. Only rows that reached the milestone have a
        # value, so the median is over the rows that did, and the complement is
        # reported as a count. See tools/Bench.cpp.
        first_feasible_times = [r["first_feasible_ms"] for r in items
                                if r.get("first_feasible_ms") is not None
                                and r["first_feasible_ms"] >= 0]
        proven_times = [r["proven_ms"] for r in items
                        if r.get("proven_ms") is not None and r["proven_ms"] >= 0]
        summary_rows.append({
            "dataset": dataset,
            "config": config,
            "stage": stage,
            "stock": stock,
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
            "gap_%s" % tag: aggregate(gaps, mode),
            "no_answer": len(items) - len(answered),
            "better": len(better),
            "vs_truth_compared": len(conclusive),
            "vs_truth_agree": len(conclusive) - len(disagreements),
            "false_positive": len(false_positives),
            "preprocess_ms": preprocess.get((dataset, config)),
            "parse_ms": parse.get((dataset, config)),
            "plan_ms_%s" % tag: aggregate(plan_times, mode),
            "plan_ms_feasible_%s" % tag: aggregate(feasible_times, mode),
            "items_%s" % tag: aggregate(item_counts, mode),
            "recipes_%s" % tag: aggregate(recipe_counts, mode),
            "conflicts_%s" % tag: aggregate(conflict_counts, mode),
            "branches_%s" % tag: aggregate(branch_counts, mode),
            "first_feasible_%s" % tag: aggregate(first_feasible_times, mode),
            "proven_ms_%s" % tag: aggregate(proven_times, mode),
            "never_feasible": sum(1 for r in items
                                  if r.get("first_feasible_ms") is not None
                                  and r["first_feasible_ms"] < 0),
            "never_proven": sum(1 for r in items
                                if r.get("proven_ms") is not None and r["proven_ms"] < 0),
            "cost_ratio_%s" % tag: aggregate(ratios, mode),
        })

    summary_rows.sort(key=summary_sort_key)

    fields = list(summary_rows[0].keys())
    with open(prefix + ".csv", "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in summary_rows:
            writer.writerow(row)

    render(summary_rows, tag, args.group_inventory)
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


def render(summary_rows, tag, group_inventory):
    gap_key = "gap_%s" % tag
    cost_key = "cost_ratio_%s" % tag
    plan_key = "plan_ms_%s" % tag
    items_key = "items_%s" % tag
    recipes_key = "recipes_%s" % tag
    by_dataset = collections.defaultdict(list)
    for row in summary_rows:
        by_dataset[row["dataset"]].append(row)
    for dataset in sorted(by_dataset):
        print()
        print("== %s == [%s]" % (dataset, tag))
        for stock, rows in stock_chunks(by_dataset[dataset], group_inventory):
            if stock is not None:
                print()
                print("  -- inventory: %s --" % stock)
            print("%-11s %-12s %4s %5s %6s %6s %6s %5s %4s %11s %5s %4s %8s %9s %7s %8s %7s %8s"
                  % ("config", "stage", "n", "feas", "prov", "unprov", "uncl", "noans", "tout",
                     "opt_agree", "fp", "more", "gap", "cost_rat", "pre_ms", "plan_ms", "items",
                     "recipes"))
            for row in sorted(rows, key=summary_sort_key):
                print("%-11s %-12s %4d %5d %6d %6d %6d %5d %4d %11s %5d %4d %8s %9s %7s %8s %7s %8s"
                      % (row["config"], row["stage"], row["queries"], row["feasible"],
                         row["proven"], row["unproven"], row["unclaimed"], row["no_answer"],
                         row["timeout"],
                         "%d/%d" % (row["vs_truth_agree"], row["vs_truth_compared"]),
                         row["false_positive"], row["better"],
                         fmt(row[gap_key], 4), fmt(row[cost_key], 3),
                         fmt(row["preprocess_ms"]), fmt(row[plan_key]),
                         fmt(row[items_key], 0), fmt(row[recipes_key], 0)))

if __name__ == "__main__":
    main()
