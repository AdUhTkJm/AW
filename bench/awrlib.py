"""Independent .awr reader + canonicalization for the cross-engine bench harness.

This mirrors the C++ side (`src/CraftingGraph.cpp`) so that every engine in the
comparison sees the same graph:

  * `scan`      -> the raw recipe list in file order,
  * `drop_useless_recipes` -> the parse-time cleanup `dropUselessRecipes`,
  * `canonicalize`         -> `canonicalizeRecipes` (dedup by output, amount and
                              inputs; workstations of merged recipes are united).

Item handles are 1-based in the file and are never renumbered by either step, so
a handle drawn here is valid for `awr_inspect`, `aw_bench` and the Java harnesses
alike. Only recipe ids shift.

Format (see src/CraftingGraph.cpp, `ByteReader` / `scan`):

    magic "AWR\\x01"
    varint nReal
    varint nOutput
    repeat nOutput:
        varint outputHandleDelta          # ascending, so handles are 1-based
        varint nRecipe
        repeat nRecipe:
            varlong outputAmount
            varint nWorkstation; if >0: varint absolute, then nWorkstation-1 deltas
            varint nInput
            repeat nInput:
                varlong inputAmount
                varint inputHandleDelta   # ascending within the recipe
"""

from collections import defaultdict


def _varint(data, pos):
    value = 0
    shift = 0
    for _ in range(5):
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, pos
    raise ValueError("varint longer than 5 bytes")


def _varlong(data, pos):
    value = 0
    shift = 0
    for _ in range(10):
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, pos
    raise ValueError("varlong longer than 10 bytes")


class Recipe:
    """One recipe. `out` and `inputs[i][0]` are 0-based item nodes (handle - 1)."""

    __slots__ = ("out", "out_amt", "ws", "inputs")

    def __init__(self, out, out_amt, ws, inputs):
        self.out = out
        self.out_amt = out_amt
        self.ws = ws
        self.inputs = inputs

    def key(self):
        """The canonicalization key: output, output amount, inputs. Workstations
        are deliberately excluded, exactly like `RecipeKeyLess` in C++."""
        return (self.out, self.out_amt, tuple(self.inputs))

    def self_consumption(self):
        """The amount of its own output the recipe also consumes."""
        return sum(amt for (item, amt) in self.inputs if item == self.out)


class Graph:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:4] != b"AWR\x01":
            raise ValueError("bad magic %r" % data[:4])

        pos = 4
        self.n_real, pos = _varint(data, pos)
        n_output, pos = _varint(data, pos)

        recipes = []
        max_handle = 0
        output = 0
        for _ in range(n_output):
            delta, pos = _varint(data, pos)
            output += delta
            max_handle = max(max_handle, output)
            n_recipe, pos = _varint(data, pos)
            for _ in range(n_recipe):
                out_amt, pos = _varlong(data, pos)
                n_ws, pos = _varint(data, pos)
                ws = []
                w = 0
                for k in range(n_ws):
                    d, pos = _varint(data, pos)
                    w = d if k == 0 else w + d
                    max_handle = max(max_handle, w)
                    ws.append(w - 1)
                n_input, pos = _varint(data, pos)
                inputs = []
                inp = 0
                for _ in range(n_input):
                    amt, pos = _varlong(data, pos)
                    d, pos = _varint(data, pos)
                    inp += d
                    max_handle = max(max_handle, inp)
                    inputs.append((inp - 1, amt))
                recipes.append(Recipe(output - 1, out_amt, ws, inputs))

        if pos != len(data):
            raise ValueError("trailing bytes after the last entry")

        self.n_raw_recipe = len(recipes)
        self.n_item = max_handle
        self.recipes = recipes
        # Item -> recipe ids. Rebuilt by prepare().
        self.i2r = _index(recipes)

    # ---- C++ parity -----------------------------------------------------

    def drop_useless_recipes(self):
        """`dropUselessRecipes`: real recipes that are a net loss on their own
        output, and real recipes with no workstation, can never be used."""
        kept = []
        for rec in self.recipes:
            real = rec.out < self.n_real
            if real and (rec.self_consumption() >= rec.out_amt or not rec.ws):
                continue
            kept.append(rec)
        self.recipes = kept
        self.i2r = _index(kept)

    def canonicalize(self):
        """`canonicalizeRecipes`: dedup by key, uniting workstations, keeping the
        file order of the lowest-numbered member of each group."""
        order = sorted(range(len(self.recipes)), key=lambda r: self.recipes[r].key())

        rep = list(range(len(self.recipes)))
        i = 0
        while i < len(order):
            j = i + 1
            while j < len(order) and self.recipes[order[j]].key() == self.recipes[order[i]].key():
                j += 1
            survivor = min(order[i:j])
            for r in order[i:j]:
                rep[r] = survivor
            i = j

        groups = defaultdict(list)
        for r in range(len(self.recipes)):
            groups[rep[r]].append(r)

        merged = []
        for survivor in sorted(groups):
            rec = self.recipes[survivor]
            ws = sorted({w for r in groups[survivor] for w in self.recipes[r].ws})
            merged.append(Recipe(rec.out, rec.out_amt, ws, rec.inputs))
        self.recipes = merged
        self.i2r = _index(merged)

    def prepare(self):
        """The exact sequence `registerCraftingGraph` performs."""
        self.drop_useless_recipes()
        self.canonicalize()
        return self

    # ---- queries over the prepared graph --------------------------------

    def is_pseudo(self, item):
        """A tag/pseudo item: a synthetic node with one member edge per member."""
        return item >= self.n_real

    def is_tag_recipe(self, recipe):
        """A recipe whose output is a pseudo item. Tag edges cost 0 in the
        planner objective, so the harness excludes them from `cost`."""
        return self.recipes[recipe].out >= self.n_real

    def produced_items(self):
        return [item for item in range(self.n_real) if self.i2r.get(item)]

    def leaf_items(self):
        """Real items with no producing recipe after preparation: the raw
        materials the player is expected to gather."""
        return [item for item in range(self.n_real) if not self.i2r.get(item)]

    def counts(self):
        return {
            "n_real": self.n_real,
            "n_item": self.n_item,
            "n_recipe": len(self.recipes),
            "n_recipe_raw": self.n_raw_recipe,
            "n_tag_recipe": sum(1 for r in range(len(self.recipes)) if self.is_tag_recipe(r)),
            "n_produced": len(self.produced_items()),
            "n_leaf": len(self.leaf_items()),
            "r2i_edges": sum(len(r.inputs) for r in self.recipes),
            "ws_edges": sum(len(r.ws) for r in self.recipes),
        }


def _index(recipes):
    index = defaultdict(list)
    for r, rec in enumerate(recipes):
        index[rec.out].append(r)
    return index


def load_names(path):
    """`handle -> (kind, name)` from a `.names.tsv`. Missing file -> empty dict."""
    names = {}
    try:
        handle = open(path, encoding="utf-8")
    except OSError:
        return names
    with handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line[0] == "#":
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            names[int(parts[0])] = (parts[1], parts[2])
    return names


def name_of(names, item, n_real):
    """Display name for a 0-based item node, matching `awr_inspect`'s labels."""
    handle = item + 1
    if item >= n_real:
        return "#%d" % handle
    entry = names.get(handle)
    return entry[1] if entry else "#%d" % handle
