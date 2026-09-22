#import "@preview/cuti:0.4.0"
#set document(title: "Pruning dominated tag members")
#set page(paper: "a4", margin: 2.3cm)
#set text(size: 10.5pt, font: ("Libertinus Serif", "Noto Serif CJK SC"))
#set heading(numbering: "1.1")
#set math.equation(numbering: "(1)")
#show link: underline

#align(center)[
  #text(size: 17pt, weight: "bold")[基于支配的剪枝]
  #v(0.3em)
  #text(size: 10pt)[应用轮椅的合成规划器优化]
]

// definition for math environment
#let witness = "witness"
#let dominate = "dominate"
#let cnt = "cnt"
#let valid = "valid"
#let alive = "alive"

= 基于支配的剪枝

== 合成图

合成图是一个二分图。物品只能指向配方，而配方只能指向物品。

它还是一个 AND-OR 图，同时配方指向物品的边是*超边*（AND 边）：我们熟悉的边（OR 边）是任选一条边即可，而这里的超边则需要同时走完所有边。

== 支配

我们考虑这样的一个例子：假设我们要合成钢制机壳，它需要 4 个任意种类的玻璃。我们知道 Minecraft 中有 16 种染色玻璃，它们通过普通玻璃和染料合成。注意到在合成任何一种染色玻璃时，我们都需要经过普通玻璃，所以普通玻璃*支配*了所有的合成路径。这时，额外花费物品去合成染色玻璃自然是不划算的。

在编译原理中，我们熟知计算支配关系的算法。然而，那个算法没法直接用在这里，因为它只能处理普通的图，而这是一个 AND-OR 图。

所以，我们接下来就需要根据我们的使用场景，找出计算*支配*关系的算法，然后基于它进行*剪枝*。

我们的直觉可能会 $w$ 支配 $m$ 定义为：合成 $m$ 时，一定绕不开 $w$。然而，这样其实还不够：假设 $8m <- w$ 是 $m$ 唯一的合成方式。这时，合成 $m$ 时确实一定绕不开 $w$，但这能说明我们不需要 $m$、直接用 $w$ 了吗？其实是不可以的，因为如果有一个配方中 $m$, $w$ 可以互换，那么显然用 $m$ 更赚。

然而当前的实现有个问题，线性规划里我们的目标是合成步数最少，所以这时 $m$ 可能确实没有被规划。但总之从合成规划而不是算法实现的角度来看，$m$ 是不能消去的。

所以，我们给出*支配*的定义。这两种情况都算作 $w$ 支配 $m$：

1. 合成 $m$ 的任何一种方法，至少会消耗*等量*的 $w$。
2. 合成 $m$ 的任何一种方法，至少会消耗*等量*的某个 tag，而其中所有不是 $w$ 的资源都支配 $w$。

我们将这个关系记作 $dominate(m, w)$。

=== 支配关系的计算

最直接的计算方式是直接遍历所有的 $(m, w)$，然后根据定义判断。这肯定很慢，所以为了加速，我们只想收集有用的 $(m, w)$，排除一些明显不可能支配 $m$ 的 $w$。

具体怎么算有用呢？对于一个物品 $m$ 与合成 $a$ 个它的配方 $r$ 而言，如果 $r$ 中含有至少 $a$ 个 $w$ 作为输入，那么 $w$ 确实有可能支配 $m$，这时 $(m, w)$ 是有用的。如果 $r$ 中含有的不是某个具体的资源，而是一个 tag $J$，那么 $J$ 的每一种资源都可以当做上述的 $w$，这时所有的 $(m, w)$ 都是有用的。我们将这个集合称作 $witness(r)$.

接下来，将 $m$ 所有配方 $r$ 的 $witness(r)$ 取交集，得到的结果称作 $c_1(m)$。这里取交集是因为根据定义，*每一个*配方都必须用到 $w$。

有了所有的 $(m, w)$，我们又该如何精确判断 $w$ 是否支配 $m$ 呢？

我们的想法是，首先假设所有的 $(m, w)$ 都满足 $dominate(m, w)$，然后逐渐删除明显不正确的那些。我们将算法过程中逐渐缩小的这个关系称作 $alive(m, w)$，在结束时它应当收敛于 $dominate(m, w)$。由于 $dominate$ 的定义是递归的，不动点迭代是常规方法。

我们不妨将支配定义的两个条件记作：$valid(m, w) = forall r in "recipes"(m). exists (j, a) in "input"(r). j = w or "isTag"(j) and w in M(j) and forall z in M(j). "alive"(z, w)$。

注意到，这和 $dominate$ 的定义是一样的，只是将递归检测 $dominate$ 的部分改换为了检测 $alive$。下面的代码实现中我们将用到这个辅助函数。

在删除 $valid(m, w)$ 不成立的节点时，一些节点可能受到影响，不再成立。对于普通的资源倒还好说，但如果遇到的是 tag 就比较麻烦了。将 tag 记作 $J$，它的成员记作 $M$，其中 $w = M[i]$ 是 $M$ 中的第 $i$ 个资源。根据定义，如果 $M$ 里除了 $w$ 以外，使得 $dominate(z, w)$ 成立的 $z$ 数量恰好是 $|M| - 1$，那么 $w$ 就支配 $m$。我们不妨将这个数量称作 $cnt(J, i)$。当且仅当 $cnt(J, i)$ 从 $|M| - 1$ 掉到 $|M| - 2$ 的时候，$alive(m, w)$ 从成立变为不成立，才会产生影响。这就是维护 $cnt$ 的作用。

我们维护一个 worklist，来将 $alive$ 迭代至不动点：

```py
for (m, w) in universe:
  worklist.push(m, w)

# 不断删除不可能的节点
while not empty(worklist):
  m, w = worklist.pop()
  if alive(m, w) and not valid(m, w):
    kill(m, w)

def kill(m, w):
  dominate(m, w) = 0
  for (J: tag) in tagsOf(m).intersect(tagsOf(w)):
    M = members(J)
    i = lowerBound(M, w)
    cnt(J, i) -= 1
    if cnt(J, i) + 1 == len(M) - 1:
      for m2 in consumers(J):
        push(m2, w) # may not exist; skip if absent
```

== 剪枝

对于含有成员 $M$ 的 tag $T$，在 $M$ 中对所有留下的 $alive(m, w)$ 建立一条边 $m -> w$。注意这可能成环，所以我们需要先缩点：计算强连通分量（SCC），然后在所有出度为 0 的 SCC 中挑选一个代表元。这时，其他的所有标签都可以到达这个代表元，将它们替换即可。

== 复杂度

最坏是 $O(N^2)$ 的，但一般坏不到这个程度。对于 2000 个资源的 `recipe-small`，最初的 $alive$ 大概有 47000 对，而含有 12000 个资源的 `recipe-nast` 则有 101 万对，不算太多。

== 效果

在 `recipe-small` 上，大概可以剪去 16% 的边，减少 10% 的单纯形法变基。

额外耗时倒是没有测量，但既然大头只在预处理时进行一次，应当是赚的。
