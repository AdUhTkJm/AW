"""Independent .awr reader + canonicalization for the cross-engine bench harness.

This mirrors the C++ side (`src/CraftingGraph.cpp`) so that every engine in the
comparison sees the same graph:

  * `scan`      -> the raw recipe list in file order,
  * `drop_nonpositive_inputs` -> the parse-time cleanup `dropNonPositiveInputs`,
  * `drop_useless_recipes` -> the parse-time cleanup `dropUselessRecipes`,
  * `canonicalize`         -> `canonicalizeRecipes` (dedup by output, amount and
                              inputs; workstations of merged recipes are united).

Item handles are 1-based in the file and are never renumbered by either step, so
a handle drawn here is valid for `awr_inspect`, `aw_bench` and the Java harnesses
alike. Only recipe ids shift.

Format (see src/CraftingGraph.cpp, `ByteReader` / `scan`):

    magic "AWR" + version byte (0x01, 0x02 or 0x03)
    varint nReal
    varint nOutput
    repeat nOutput:
        varint outputHandleDelta          # ascending, so handles are 1-based
        varint nRecipe
        repeat nRecipe:
            # v3 only; v1/v2 are all-cost-1 datasets:
            varlong cost                   # underlying executions per execution
            varlong outputAmount           # anchor amount
            varint nWorkstation; if >0: varint absolute, then nWorkstation-1 deltas
            # v2 and v3 only; v1 has no byproduct list:
            varint nByproduct
            repeat nByproduct:
                varlong byproductAmount
                varint byproductHandleDelta  # ascending within the recipe
            varint nInput
            repeat nInput:
                varlong inputAmount
                varint inputHandleDelta   # ascending within the recipe

`cost` is how many times the underlying Minecraft recipe runs per execution of
this entry: 1 for an ordinary recipe, 0 for the free member edge of a
pseudo-resource, and the batch size for a level of a chanced recipe, which the
mod folds into one deterministic column. It is parsed and carried through
canonicalization, because it is part of the recipe identity in `RecipeKeyLess`
-- but the harness's `cost` column still counts one per real execution, so a
dataset with chanced batches reports an unweighted objective until that model
learns to weight by `Recipe.cost`.

`inputAmount` is a flow, not a flag: the file may still spell a degenerate
input as `0` (AE2's entropy recipes carry a fluid ingredient with no amount at
all), which `prepare` normalizes away rather than passing on. Every engine
plugs this into the same normalization, so a `.awr` written before the rule
existed graphs identically to one written after it.
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
    """One recipe. `out` and `inputs[i][0]` are 0-based item nodes (handle - 1).

    `byproducts` holds the extra outputs as `(item, amount)` pairs, ascending and
    never containing the anchor, exactly like the C++ `r2o` rows.
    """

    __slots__ = ("out", "out_amt", "byproducts", "ws", "inputs", "cost")

    def __init__(self, out, out_amt, ws, inputs, byproducts=(), cost=1):
        self.out = out
        self.out_amt = out_amt
        self.byproducts = list(byproducts)
        self.ws = ws
        self.inputs = inputs
        self.cost = cost

    def outputs(self):
        """`(item, amount)` for the anchor and every byproduct. Not sorted."""
        return [(self.out, self.out_amt)] + list(self.byproducts)

    def key(self):
        """The canonicalization key: anchor, anchor amount, byproducts, inputs
        and cost. Workstations are deliberately excluded, exactly like
        `RecipeKeyLess` in C++. The anchor is compared first, so this matches
        the C++ ordering of the full sorted output list, and the cost is last so
        the order stays dominated by the column. Two recipes with the same
        column but different costs are not interchangeable -- merging them would
        keep an arbitrary one -- so the cost is part of the identity."""
        return (self.out, self.out_amt, tuple(self.byproducts), tuple(self.inputs),
                self.cost)

    def self_consumption(self):
        """The amount of its own anchor output the recipe also consumes."""
        return sum(amt for (item, amt) in self.inputs if item == self.out)

    def column_is_nonpositive(self):
        """`columnIsNonPositive`: on every output row the recipe consumes at
        least as much as it produces, so the whole column is <= 0 and no optimal
        plan executes it."""
        consumed = defaultdict(int)
        for (item, amt) in self.inputs:
            consumed[item] += amt
        return all(amt <= consumed[item] for (item, amt) in self.outputs())


class Graph:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:3] != b"AWR":
            raise ValueError("bad magic %r" % data[:4])
        version = data[3]
        if version not in (1, 2, 3):
            raise ValueError("unsupported schema version %d" % version)

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
                # A real recipe costs at least one underlying execution, and a
                # pseudo-resource's member edge is free, whatever the blob
                # spells. Before v3 every real recipe cost exactly one.
                cost = 1
                if version >= 3:
                    cost, pos = _varlong(data, pos)
                cost = max(cost, 1) if output <= self.n_real else 0

                out_amt, pos = _varlong(data, pos)
                n_ws, pos = _varint(data, pos)
                ws = []
                w = 0
                for k in range(n_ws):
                    d, pos = _varint(data, pos)
                    w = d if k == 0 else w + d
                    max_handle = max(max_handle, w)
                    ws.append(w - 1)

                anchor = output - 1
                merged = {anchor: out_amt}
                if version >= 2:
                    n_by, pos = _varint(data, pos)
                    byproduct = 0
                    for _ in range(n_by):
                        amt, pos = _varlong(data, pos)
                        d, pos = _varint(data, pos)
                        byproduct += d
                        max_handle = max(max_handle, byproduct)
                        merged[byproduct - 1] = merged.get(byproduct - 1, 0) + amt
                anchor_amt = merged.pop(anchor, 0)
                byproducts = sorted(merged.items())

                n_input, pos = _varint(data, pos)
                inputs = []
                inp = 0
                for _ in range(n_input):
                    amt, pos = _varlong(data, pos)
                    d, pos = _varint(data, pos)
                    inp += d
                    max_handle = max(max_handle, inp)
                    inputs.append((inp - 1, amt))
                recipes.append(Recipe(anchor, anchor_amt, ws, inputs, byproducts, cost))

        if pos != len(data):
            raise ValueError("trailing bytes after the last entry")

        self.n_raw_recipe = len(recipes)
        self.n_item = max_handle
        self.recipes = recipes
        # Item -> recipe ids. Rebuilt by prepare().
        self.i2r = _index(recipes)

    # ---- C++ parity -----------------------------------------------------

    def drop_useless_recipes(self):
        """`dropUselessRecipes`: real recipes whose whole column is <= 0, and
        real recipes with no workstation, can never be used."""
        kept = []
        for rec in self.recipes:
            real = rec.out < self.n_real
            if real and (rec.column_is_nonpositive() or not rec.ws):
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
            merged.append(Recipe(rec.out, rec.out_amt, ws, rec.inputs, rec.byproducts,
                                 rec.cost))
        self.recipes = merged
        self.i2r = _index(merged)

    def drop_nonpositive_inputs(self):
        """`dropNonPositiveInputs`: an input that consumes nothing per firing is
        not a flow. It contributes 0 to every balance row, so dropping the edge
        changes no plan, while keeping it would make three engines disagree on
        what "free input" means (the Java planners reject a non-positive
        amount outright). Runs before `drop_useless_recipes` so
        self-consumption never sums a degenerate amount, and re-canonicalizes
        because the drop can make recipes identical."""
        changed = False
        for rec in self.recipes:
            if any(amt <= 0 for (_, amt) in rec.inputs):
                rec.inputs = [(item, amt) for (item, amt) in rec.inputs if amt > 0]
                changed = True
        if changed:
            self.canonicalize()
        return changed

    def prepare(self):
        """The exact sequence `registerCraftingGraph` performs."""
        self.drop_nonpositive_inputs()
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
            "r2o_edges": sum(1 + len(r.byproducts) for r in self.recipes),
            "ws_edges": sum(len(r.ws) for r in self.recipes),
        }


def _index(recipes):
    index = defaultdict(list)
    for r, rec in enumerate(recipes):
        for (item, _amt) in rec.outputs():
            index[item].append(r)
    return index


def load_names(path):
    """`handle -> (resource location, en_US name)` from a `.names.tsv`.

    The table the mod writes is `<handle>\t<resource location>\t<en_US>\t<zh_CN>`.
    Only the first three columns are read; the Chinese name is for the search
    index and is not used by the tools. Missing file -> empty dict.
    """
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
