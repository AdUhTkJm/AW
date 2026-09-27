#!/usr/bin/env python3
"""Quick eyeball dump of a .awr recipe graph, and an optional JSON export.

The format itself is parsed by `bench/awrlib.py`, which is the canonical reader
shared by the benchmark harness, so this tool cannot drift from it.

    python3 tools/awr_dump.py temp/recipes-vanilla.awr temp/recipes-vanilla.names.tsv
    python3 tools/awr_dump.py temp/recipes-vanilla.awr "" 5 out.json   # 5 recipes + export
"""

import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "bench"))

from awrlib import Graph, load_names, name_of  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    names_path = sys.argv[2] if len(sys.argv) > 2 else ""
    shown = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    export = sys.argv[4] if len(sys.argv) > 4 else ""

    graph = Graph(path).prepare()
    names = load_names(names_path) if names_path else {}

    for key, value in sorted(graph.counts().items()):
        print("%-14s: %s" % (key, value))
    print()
    for recipe in graph.recipes[:shown]:
        inputs = ", ".join("%s x%d" % (name_of(names, item, graph.n_real), amount)
                           for (item, amount) in recipe.inputs)
        print("  %d x %s  <- %s   (%d workstation(s))"
              % (recipe.out_amt, name_of(names, recipe.out, graph.n_real), inputs or "(nothing)",
                 len(recipe.ws)))

    if export:
        payload = {
            "nReal": graph.n_real,
            "nItem": graph.n_item,
            "recipes": [
                {
                    "out": recipe.out + 1,
                    "outAmt": recipe.out_amt,
                    "inputs": [[item + 1, amount] for (item, amount) in recipe.inputs],
                    "workstations": [station + 1 for station in recipe.ws],
                }
                for recipe in graph.recipes
            ],
        }
        with open(export, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)
        print("\nwrote %s" % export)
    return 0


if __name__ == "__main__":
    sys.exit(main())
