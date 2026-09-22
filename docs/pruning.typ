#set document(title: "Pruning dominated tag members")
#set page(paper: "a4", margin: 2.3cm)
#set text(size: 10.5pt)
#set heading(numbering: "1.1")
#set math.equation(numbering: "(1)")
#show link: underline

#align(center)[
  #text(size: 17pt, weight: "bold")[Pruning dominated tag members from the crafting graph]
  #v(0.3em)
  #text(size: 10pt)[An introduction to the algorithm used by the Applied Wheelchair planner]
]

= The crafting graph

A *crafting graph* has three kinds of nodes.

- An *item* (a resource, addressed by a handle) is something the player wants or
  has.
- A *recipe* produces one item (in some amount) from several input items (in
  given amounts), and can only be executed at one of a set of workstations.
- A *tag*, also called a *pseudo-resource*, is a synthetic node that stands for "any one of these items". The planner encodes it with one synthetic recipe $T <- m$ per member $m$, and no workstation. A tag is therefore just an OR-gadget bolted onto the item space. It can also be viewed as a special item.

The natural way to read the graph is *AND-OR*:

- an *item* is *OR* over its recipes -- you pick one;
- a *recipe* is *AND* over its inputs -- you need all of them;
- a *tag* is *OR* over its members -- you pick one.

That reading is the source of every subtlety below. A control-flow graph, the usual home of dominators, is a plain graph in which every node picks one successor, so it is OR all the way down. A recipe is therefore called a hyperedge.

The planner solves a linear program. With $x_r$ the number of times recipe `r` is executed, it minimizes the total number of executions subject to a balance constraint per item: production minus consumption must cover the target (plus whatever inventory is available). Base items that have no recipe are simply free. The objective is therefore

$ "minimize" sum_(r) x_r, quad x_r >= 0. $

Everything below is about removing columns $x_r$ that can never help, without changing the optimum.

= The redundancy

Consider `mekanism:steel_casing`, which is made from osmium, steel and four
units of a tag we will call `#2377`. That tag contains `minecraft:glass` and
all sixteen stained glasses. Each stained glass is made from glass (and a dye),
so no plan ever needs to *craft* stained glass in order to serve `#2377`:
normal glass is available as a member, and it is the cheaper choice. The
synthetic recipe `#2377 <- <stained glass>` is dead weight.

So we want a rule of the form: inside a tag $T$, if member $m$ is "dominated" by another member $w$, then the edge $T <- m$ can be dropped. The rest of this note is about what "dominated" has to mean, and why some of the obvious graph answers are wrong.

= What does not work

== Plain reachability is far too weak

Write `m -> j` when some recipe of `m` takes `j` as an input, and say `w` is
reachable from `m` if there is a path `m -> ... -> w`. The tempting rule "drop
`T <- m` when a co-member `w` is reachable from `m`" is unsound. Witness the
tiny graph

```text
tag T = {m, w}        tag J = {w, z}
m x1 <- J x1          z is a free leaf
w x1 <- a x1 <- b x1 <- base
P x1 <- T x1
```

The cheapest way to produce `P` is `P <- T <- m <- J <- z`, which never touches
`w`. Reachability still finds a path `m -> J -> w`, so the rule would drop
`T <- m` and multiply the cost of `P` by a constant factor.

On real data reachability is worse than merely loose. The requirement graph is
full of conversion cycles, so unrelated items reach each other: `acacia_planks`
has a recipe from `acacia_fence_gate`, a fence gate can be made from sticks, a
stick from a torch, a torch from a charcoal tag, charcoal from Mekanism sawdust,
and sawdust from a general "buttons" tag that contains `bamboo_button`. Hence
"`acacia_planks` reaches `bamboo_button`", which means nothing.

== Path post-dominance misses the AND nodes

In a control-flow graph one says `d` *post-dominates* `n` when every path from
`n` to the exit passes through `d`. The equation is a dataflow fixpoint

$ upright("pdom")(n) = {n} union inter.big_(s in "succ"(n)) upright("pdom")(s), $

and Lengauer-Tarjan computes it in near-linear time using the tree structure of
a depth-first search. This does *not* transfer here, for two reasons.

First, a path chooses one input per recipe, but a derivation needs all of them.
An AND node forces its other children even when a path skips them. Take
`m x1 <- {w x1, u x1}` with `u` a leaf: the path `m -> u` avoids `w`, so `w` is
not a path post-dominator, yet *every* derivation of `m` contains `w` because
the recipe needs both inputs. Path post-dominance is strictly *stronger* than
unavoidability, so it silently misses cases. The glass family is exactly such a
case: a stained glass also needs a dye, and the path to that leaf avoids glass,
so a path-based analysis never notices that glass is in fact unavoidable.

Second, the requirement graph is cyclic, and post-dominance to a set of leaves
is degenerate for items caught in a cycle (there may be no path to a leaf at
all). So even setting the quantifier issue aside, the classical algorithm has
nothing to compute on.

In short: no, Lengauer-Tarjan does not apply, and the reason is the AND nodes
(the hyperedges), not merely the sizes.

== Unavoidability still is not enough

Suppose $w$ really is in every derivation of $m$. That is not sufficient, because the objective counts executions, not materials. If

```text
m x8 <- w x1
```

then every derivation of $m$ contains $w$, yet one execution of $m$ yields 8 units. Dropping $T <- m$ in favour of $w$ might cause more $w$'s to be crafted, increasing total amount of recipes executed. So the rule must compare *quantities*: every recipe of $m$ has to spend at least as much as what $w$ spends to produce $m$.

= The relation we actually want

Define $U(m)$, the set of items that appear in *every* derivation of $m$, by the AND-OR equations:

$ U(X) = {X} union inter.big_(r in R(X)) U(r) $ <eq:item>

$ U(r) = union.big_(j in I(r)) U(j) $ <eq:recipe>

$ U(T) = inter.big_(m in M(T)) U(m), "where" U(ell) = {ell} "for a leaf" ell. $ <eq:tag>

Here $R(X)$ is the recipes producing $X$, $I(r)$ the inputs of $r$, and $M(T)$ the members of tag $T$. The definition is obvious to understand and captures the whole AND-OR interpretation.

We intend to solve it via iteration till fixed point. The fixed point is well defined because the right-hand sides are monotone in $U$; the largest solution is the one we
want, and it agrees with the semantic reading on obtainable items. Items that are not obtainable at all satisfy the equations vacuously, which is harmless because such an item can never be crafted anyway.

Now add the quantity. We say $m$ requires $w$ when for every recipe $r$ of $m$, there is an input $(j, a)$ with $a >= "out"(r)$ such that either $j = w$, or $j$ is a tag with $w in M(j)$ and every other member of $j$ also requires $w$. This is @eq:recipe and @eq:tag combined with the "at least as much as the output" condition.

== Why that is enough

Let $c(x)$ be the minimum number of executions needed to obtain one unit of $x$, with base items free. Take a recipe $r$ of $m$ whose gating input is a tag $j$ containing $w$. Because every other member of $j$ requires $w$, the cheapest way to satisfy $j$ is $w$ itself, so it costs $c(w)$ per unit; and the recipe consumes $a >= "out"(r)$ of it. That
route therefore costs at least

$ (1 + a c(w)) / "out"(r) >= c(w) + 1 / "out"(r) > c(w). $

Every route of $m$ is gated this way, and $c(m)$ is the cheapest route, so $c(m) > c(w)$. Serving a tag $T$ through $w$ is thus strictly cheaper than crafting $m$ for $T$.

The only way $m$ could still win is if it were already
sitting in the player's inventory, where it is free -- which is exactly the case handled separately in @sec:inventory.

= From requirement relation to edge pruning

For each tag $T$ build a directed graph on its members with an edge $m -> w$ whenever $m$ requires $w$. We keep a set $K$ of members and drop $T <- m$ for every $m$ that can reach some $w in K$. The kept set must be chosen so that every dropped member has a kept target, and no kept member is dropped. The clean choice is: condense the graph into strongly connected components (SCCs) and keep one representative from each *sink* component. Then

- every tag keeps at least one member, so no tag ever becomes unsatisfiable;
- every other member reaches a sink representative along `requires` edges, so it
  can be replaced by it;
- mutual-requirement cycles collapse to one surviving representative instead of
  all members being dropped.

= Inventory
#label("sec:inventory")

The cost argument assumes nothing is free. Player inventory breaks that assumption for the item being dominated, so the pruning is applied per query and guarded: drop $T <- m$ only when $"inventory"[m] = 0$; keep it when the player actually holds $m$, so the free units can still be spent on the tag.

The member node itself is never removed, and none of its other consumers are touched. This matters: a stained glass is also used for panes and for Mekanism pigments, so
removing its *production* recipes would be wrong, while removing the *tag edge*
is fine.

Note also that workstation availability never enters the argument. Filtering
recipes by workstation only removes options, and removing options cannot create a
new cheaper route for `m`, so a precomputed `requires` relation stays valid for
every workstation set.

= The algorithm

The greatest fixpoint of `requires` is computed with a worklist over pairs $(m, w)$.

1. For every real item $m$ with recipes, compute the one-step candidates by intersecting, over its recipes, the set of inputs that carry at least the recipe's output (real inputs directly; tag inputs expand to their members).
   
  This is a superset of the pairs worth keeping.

2. Add support pairs $(z, w)$ for members $z, w$ of a common tag, where $z$ is producible. Initialize all pairs as alive.

3. For each tag $J$ and each member $w$, maintain a counter of how many other members $z$ currently have $(z, w)$ alive. The tag may gate through $w$ exactly when that counter reaches $|M(J)| - 1$; a member with no recipe contributes nothing, so a leaf member always blocks the gate.

4. Rip pairs out of the alive set whenever they are no longer justified, and when a pair $(z, w)$ dies, decrement the counters of every tag containing both $z$ and $w$, re-queueing that tag's consumers. Each pair dies at most once.

5. Pick the surviving representatives per tag by SCC condensation, as in the previous section, and mark the corresponding synthetic recipes.

The candidate pairs are bounded by the sum of squared tag sizes plus the one-step candidate sets: about 47 thousand pairs for the small sample corpus and 1.05 million for the large one. With the counters the fixpoint is near-linear in
that size.

= Results

Reachable subgraph, no inventory, comparing the full graph with the pruned one:

#table(
  columns: (auto, auto, auto, auto),
  table.header([*corpus*], [*dominated tag edges*], [*`steel_casing` recipes*], [*items*]),
  [small], [817 / 3102 (26.3%)], [5075 -> 4230], [1246 -> 1212],
  [large], [3648 / 11398 (32.0%)], [23304 -> 19664], [5965 -> 5194],
)

On the small corpus the `mekanism:steel_casing` solve drops from 915 to 154
simplex iterations, and a 300-target sample showed no objective increase and no
lost feasibility. Workstation-independent, and the inventory guard keeps the
dominated members spendable.

= Cheat sheet

#table(
  columns: (auto, auto, auto),
  table.header([*Rule*], [*Sound?*], [*Why*]),
  [some path reaches `w`], [no], [ignores AND: `m <- J <- z` route is missed],
  [every path reaches `w`], [incomplete], [misses AND-forced inputs such as the dye path],
  [every derivation contains `w`], [no], [bulk amplification: `m x8 <- w x1`],
  [`requires(m, w)` with amounts], [yes], [cost argument of §5],
  [same, plus inventory guard], [yes], [free inventory for the dominated item],
)

= Further reading

- T. Lengauer and R. E. Tarjan, "A fast algorithm for finding dominators in a
  flowgraph": the classical path-dominator algorithm that does *not* generalise
  to the AND nodes here.
- R. Gupta, "Generalized dominators and post-dominators": a different
  generalisation (sets of vertices that dominate together).
- Any standard treatment of monotone dataflow / Kildall iteration for the
  fixpoint style used above.
