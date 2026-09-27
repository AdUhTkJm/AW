#!/usr/bin/env python3
"""Build a reproducible query plan (targets + inventory groups) for one dataset.

Everything the harness runs is derived from these files, so the plan is the
single source of truth shared by `aw_bench`, the Thunderbolt harness and the
AE2VM harness. No engine re-derives a random draw.

Outputs, all TSV next to `--out-dir`:

    <dataset>.meta.tsv            key/value facts about the dataset and the draw
    <dataset>.targets.tsv         handle  name  amounts  n_recipes  in_big_scc
    <dataset>.stock.none.tsv      (header only: group 1, nothing stocked)
    <dataset>.stock.leaves.tsv    handle  amount   (group 2, leaves = 1e6 by default)
    <dataset>.stock.random20.tsv  handle  amount   (group 3, 20% of real items)

Handles are 1-based, matching the `.awr` file and `awr_inspect --plan <handle>`.
Amounts are passed through verbatim, so the same plan drives both the `1` and
the `100000` request of every target.

Reproducibility: two independent RNG streams, `random.Random("<seed>:targets")`
and `random.Random("<seed>:random20")`, so adding a target later does not shift
the inventory draw. See bench/README.md for the exact recipe.
"""

import argparse
import hashlib
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from awrlib import Graph, load_names, name_of  # noqa: E402

GROUPS = ("none", "leaves", "random20")


def item_graph(graph):
    """Edge `i -> j` when some recipe consumes `i` and produces `j`."""
    n = graph.n_item
    out = [[] for _ in range(n)]
    for rec in graph.recipes:
        for (item, _amount) in rec.inputs:
            if item != rec.out:
                out[item].append(rec.out)
    return out


def strongly_connected(graph):
    """Iterative Tarjan over the item graph. Returns a list of components.

    Recursion is not an option: the large corpora have ~13k items.
    """
    out = item_graph(graph)
    n = len(out)
    index = [0] * n
    low = [0] * n
    on_stack = bytearray(n)
    stack = []
    counter = 0
    components = []

    for root in range(n):
        if index[root]:
            continue
        work = [(root, 0)]
        while work:
            node, k = work[-1]
            if k == 0:
                counter += 1
                index[node] = low[node] = counter
                stack.append(node)
                on_stack[node] = 1
            adjacency = out[node]
            descended = False
            while k < len(adjacency):
                nxt = adjacency[k]
                k += 1
                if not index[nxt]:
                    work[-1] = (node, k)
                    work.append((nxt, 0))
                    descended = True
                    break
                if on_stack[nxt] and index[nxt] < low[node]:
                    low[node] = index[nxt]
            if descended:
                continue
            work[-1] = (node, k)
            work.pop()
            if work:
                parent = work[-1][0]
                if low[node] < low[parent]:
                    low[parent] = low[node]
            if low[node] == index[node]:
                component = []
                while True:
                    top = stack.pop()
                    on_stack[top] = 0
                    component.append(top)
                    if top == node:
                        break
                components.append(component)
    return components


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", required=True,
                        help="short dataset name, e.g. recipes-vanilla")
    parser.add_argument("--awr", required=True, help="path to the .awr recipe graph")
    parser.add_argument("--names", default="", help="path to the .names.tsv table")
    parser.add_argument("--out-dir", default="bench/plans")
    parser.add_argument("--seed", default="20260101", help="master seed (string)")
    parser.add_argument("--targets", default="8",
                        help="number of random targets, or 'all' for every produced item")
    parser.add_argument("--amounts", default="1,100000", help="requested amounts, comma separated")
    parser.add_argument("--groups", default=",".join(GROUPS), help="inventory groups to emit")
    parser.add_argument("--leaf-stock", type=int, default=1_000_000,
                        help="stock given to every leaf in group 2")
    parser.add_argument("--random-frac", type=float, default=0.2,
                        help="fraction of real items stocked in group 3")
    parser.add_argument("--random-min", type=int, default=1)
    parser.add_argument("--random-max", type=int, default=1_000_000,
                        help="log-uniform upper bound for group 3 amounts")
    parser.add_argument("--big-scc", action="store_true",
                        help="sample targets from the largest SCC instead of uniformly")
    args = parser.parse_args()

    graph = Graph(args.awr).prepare()
    counts = graph.counts()
    names = load_names(args.names) if args.names else {}
    amounts = [int(a) for a in args.amounts.split(",") if a]

    components = strongly_connected(graph)
    biggest = max(components, key=len)
    biggest_members = frozenset(biggest)

    rng_targets = random.Random("%s:targets" % args.seed)
    produced = graph.produced_items()
    pool = [i for i in produced if i in biggest_members] if args.big_scc else produced
    if not pool:
        raise SystemExit("no eligible targets")

    if args.targets == "all":
        chosen = sorted(pool)
    else:
        count = min(int(args.targets), len(pool))
        chosen = sorted(rng_targets.sample(sorted(pool), count))

    rng_stock = random.Random("%s:random20" % args.seed)
    real_items = list(range(graph.n_real))
    stocked = sorted(rng_stock.sample(real_items, int(round(args.random_frac * len(real_items)))))
    random_amounts = {}
    for item in stocked:
        if args.random_max <= args.random_min:
            random_amounts[item] = args.random_min
        else:
            ratio = args.random_max / args.random_min
            value = args.random_min * ratio ** rng_stock.random()
            random_amounts[item] = max(args.random_min, int(round(value)))

    os.makedirs(args.out_dir, exist_ok=True)
    prefix = os.path.join(args.out_dir, args.dataset)

    digest = hashlib.sha256(open(args.awr, "rb").read()).hexdigest()

    with open(prefix + ".meta.tsv", "w", encoding="utf-8") as out:
        out.write("# key\tvalue\n")
        out.write("dataset\t%s\n" % args.dataset)
        out.write("awr\t%s\n" % os.path.abspath(args.awr))
        out.write("awr_sha256\t%s\n" % digest)
        out.write("names\t%s\n" % ("" if not args.names else os.path.abspath(args.names)))
        out.write("seed\t%s\n" % args.seed)
        out.write("targets\t%s\n" % args.targets)
        out.write("big_scc_only\t%d\n" % (1 if args.big_scc else 0))
        out.write("amounts\t%s\n" % ",".join(str(a) for a in amounts))
        out.write("groups\t%s\n" % args.groups)
        out.write("leaf_stock\t%d\n" % args.leaf_stock)
        out.write("random_frac\t%g\n" % args.random_frac)
        out.write("random_min\t%d\n" % args.random_min)
        out.write("random_max\t%d\n" % args.random_max)
        for key, value in sorted(counts.items()):
            out.write("%s\t%d\n" % (key, value))
        out.write("big_scc_items\t%d\n" % len(biggest))
        out.write("targets_drawn\t%d\n" % len(chosen))
        out.write("random20_stocked\t%d\n" % len(stocked))

    with open(prefix + ".targets.tsv", "w", encoding="utf-8") as out:
        out.write("# handle\tname\tamounts\tn_recipes\tin_big_scc\n")
        for item in chosen:
            out.write("%d\t%s\t%s\t%d\t%d\n" % (
                item + 1, name_of(names, item, graph.n_real),
                ",".join(str(a) for a in amounts),
                len(graph.i2r.get(item, ())), 1 if item in biggest_members else 0))

    requested = set(args.groups.split(","))
    if "none" in requested:
        with open(prefix + ".stock.none.tsv", "w", encoding="utf-8") as out:
            out.write("# handle\tamount\n")
    if "leaves" in requested:
        with open(prefix + ".stock.leaves.tsv", "w", encoding="utf-8") as out:
            out.write("# handle\tamount\n")
            for item in graph.leaf_items():
                out.write("%d\t%d\n" % (item + 1, args.leaf_stock))
    if "random20" in requested:
        with open(prefix + ".stock.random20.tsv", "w", encoding="utf-8") as out:
            out.write("# handle\tamount\n")
            for item in stocked:
                out.write("%d\t%d\n" % (item + 1, random_amounts[item]))

    print("dataset      : %s" % args.dataset)
    for key, value in sorted(counts.items()):
        print("%-13s: %s" % (key, value))
    print("big_scc_items: %d" % len(biggest))
    print("targets      : %d  (%s)" % (len(chosen), ", ".join(
        name_of(names, i, graph.n_real) for i in chosen[:5])))
    print("random20     : %d items stocked" % len(stocked))
    print("wrote        : %s.{meta,targets,stock.*}.tsv" % prefix)


if __name__ == "__main__":
    main()
