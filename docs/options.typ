#import "@preview/cuti:0.4.0"
#set document(title: "规划器的选项与预算")
#set page(paper: "a4", margin: 2.3cm)
#set text(size: 10.5pt, font: ("Libertinus Serif", "Noto Serif CJK SC"))
#set heading(numbering: "1.1")
#show link: underline

#align(center)[
  #text(size: 17pt, weight: "bold")[规划器的选项与预算]
  #v(0.3em)
  #text(size: 10pt)[应用工作台 (AW) 的规划器可调参数]
]

= 简介

规划器有两中可调参数：剪枝开关与各类预算（`aw::Options`），以及求解器的预算（`aw::solver::Options`）。本文记录每个字段的含义、默认值与作用时机；字段的算法背景见 `docs/algorithm.typ`，这里就不再重复。

== 声明方式

每个可序列化字段只在头文件里的一个 X-macro 列表中出现一次，形如

```cpp
#define AW_PLANNER_OPTION_FIELDS(X)      \
  X(bool, nonoptimal, true)              \
  X(bool, tagPruning, true)              \
  X(TagInlineMode, tagInlining, TagInlineMode::QUERY_TIME) \
  ...
```

`X(type, member, default)` 这一行同时被展开成三处：结构体的成员声明、`OptionsJson.cpp` 里的 JSON 读入、以及 JSON 写出与已知键集合。枚举类型通过 `EnumCodec` 的模板特化来将名字与序号对应。

== 补丁语义

读入一份 JSON 是一次“部分补丁”：没有出现的键保持当前值，出现但无法解析的键使整份补丁失败，未知的键也会被拒绝。这样就可以检查配置文件的拼写错误了。我们会先复制，在副本上修改，最后再提交，因此这是原子的。

我们的算法是先预处理，再查询，因此会分为注册期与查询器两种时机。注册器的字段必须在 `registerCraftingGraph` 之前设置，而查询期开关可以随时改。下面，我们会在每一项后面标出时机。

= 规划器选项 `aw::Options`

== 剪枝开关

- `nonoptimal`（bool，默认 `true`，注册期）：支配类 pass 使用整数松弛后的代价关系。打开时剪得更狠，可能丢掉最优解本会用到的配方；关掉是精确模式。这个开关只影响剪枝，不影响模型本身的可行性。
- `tagPruning`（bool，默认 `true`，注册期）：丢掉被同一 tag 的其它成员在代价上支配的成员边。
- `recipePruning`（bool，默认 `true`，注册期）：当某个 guard 输入的所有生产者都能被内联时，丢掉被同物品的兄弟配方支配的配方。
- `directPruning`（bool，默认 `true`，注册期）：丢掉整列都被兄弟配方支配的配方。
- `substitutionPruning`（bool，默认 `true`，注册期）：当一个被支配的输入替换之后配方输给兄弟时，把该配方丢掉。
- `seedPruning`（bool，默认 `true`，查询期）：查询时丢掉既无法对任何可行方案有贡献、又拿不到种子的节点。它取代了已退休的 `deadNodePruning`。
- `tagTidy`（bool，默认 `true`，查询期）：求解之后，把方案并不消费的 tag 转换清零。见 `TagTidy.cpp`。

== 快速模式

- `flash`（bool，默认 `false`，查询期）：快速模式，返回第一个可行的方案而不是最省的那一个。它只保证方案真实可执行，代价可以是任意的。这是过程级开关；`solver::State::flash` 的初值来自它，而一次查询的平衡方案若被 fireability 检查拒绝，重试时会把它关掉（见 `docs/algorithm.typ` 的启动可达性一节）。
- `flashProbe`（bool，默认 `true`，查询期）：快速模式的早期贪心探测。打开时，flash 查询在卫星消除和变体类 pass 之前，先在廉价剪枝后的子图上通过贪心尝试给出一个方案；一旦拿到就跳过那些 pass 直接返回。探测给出的方案比完全剪枝的子图更容易找到，因为被跳过的 pass 同时也会删掉 Greedy 可以使用的路线。四个数据集上，中位差为 0，最差约 1.2 倍，所以默认打开。

== Tag 内联

- `tagInlining`（`off` | `prePrune` | `queryTime` | `both`，默认 `queryTime`，混合）: 单次使用 tag 在何处被展平。
  - `off`：从不内联。
  - `prePrune`：注册时、在支配 pass 之前，用全部成员边展平。这样给 pass 一个最平的图，但可能把 tag 剪枝本会丢掉的成员重新引入，从而让查询期子图变大。
  - `queryTime`：每次查询在 `reachableSubgraph` 内、遍历与所有剪枝 pass 之后展平。只有活下来的配方会被内联，于是 tag 存活的成员边——包括被支配但玩家有库存的成员——恰好是仍然可以花掉的那些。子图不会变大，库存也保持可用。
  - `both`：两者都做，持久的单次使用 tag 为整个语料展平一次，子图特有的一次一查询。主要用于实验。
- `inlineSingleMemberTags`（bool，默认 `true`，注册期）：内联器是否内联单成员的 tag，见 `inlineSingleUseTagsCore`。单独设开关是因为多成员那一半的代价很不一样。#strong[未序列化]，是实验开关。

== 子图重剪 `reprune`

这些参数#strong[未序列化]，只在开发与测试里通过 C++ 直接设置（见 `tests/plan/Main.cpp`）。

- `enabled`（bool，默认 `true`）：查询时对可达子图再做一次剪枝。
- `enabledOnFlash`（bool，默认 `false`）：flash 查询里是否也这样做。快速模式默认跳过它。
- `exact`（bool，默认 `true`）：重剪使用精确关系而不是松弛关系。松弛模式可能导致方案大幅度偏离最优。

== Pack 剪枝 `pack`

注册时运行。

多物品“浪费证书”的预算。

- `enabled`（bool，默认 `true`）：pass 总开关。
- `maxPackRecipes`（uint32，默认 `128`）：一个 pack 里不同配方数的上限。
- `maxPackValue`（int64，默认 `1024`）：一个 pack 里总执行次数的上限。
- `maxBranchDepth`（uint32，默认 `8`）：R3 搜索的深度预算，超过即把节点当叶子。
- `maxBranchNodes`（uint32，默认 `4096`）：R3 搜索的节点数预算，超过即把节点当叶子。
- `maxZeroStockItems`（uint32，默认 `32`）：pack 的零库存集合上限；需要更多库存的推导被丢弃。
- `maxSeconds`（double，默认 `60.0`）：整个 pass 的时间预算，`<= 0` 表示不限。

R3 目前已经禁用，但可以通过 `AW_R3=1` 重新打开。

== 孤岛消除 `satellite`

查询时运行。

- `enabled`（bool，默认 `true`）：pass 总开关。
- `enabledOnFlash`（bool，默认 `false`）：flash 查询里是否也运行。
- `maxComponentNodes`（uint32，默认 `512`）：跳过节点数超过它的无向分量，防止 LP 耗时太多。
- `maxIslandNodes`（uint32，默认 `4096`）：广义逃逸枚举的岛上界。单个逃逸的岛可能比无向分量宽得多（取的是极大闭集），所以它有自己更大的上界。这里数的是物品加配方。
- `maxSeconds`（double，默认 `0.05`）：一轮 pass 的时间预算。`<= 0` 表示不限。

== 变体类消除 `variantClass`

查询时运行。

对每个成员可能构成可互换类的 tag，把类收缩到对转换封闭为止，然后丢掉类内部的转换。

- `enabled`（bool，默认 `true`）：pass 总开关。
- `maxClassNodes`（uint32，默认 `4096`）：跳过成员数超过它的候选 tag。这么宽的东西一般不是装饰品。
- `maxCandidates`（uint32，默认 `4096`）：每次查询最多考察多少个候选 tag。
- `maxSeconds`（double，默认 `0.05`）：pass 的时间预算。`<= 0` 表示不限。

== Tag 独占生产者消除 `tagExclusive`

查询时运行。

若某个真实配方的输出全都对同一个 $T$ 独占，并且它产生的 $M(T)$ 容量不多于它消耗的，就丢掉它。

- `enabled`（bool，默认 `true`）：pass 总开关。
- `maxSeconds`（double，默认 `0.05`）：pass 的时间预算。`<= 0` 表示不限。

== 变体折叠 `variantFold`

查询时运行。

寻找资源的装饰性变体，并将它们折叠进本体。

- `enabled`（bool，默认 `true`）：pass 总开关。
- `maxFoldItems`（uint32，默认 `4096`）：折叠里物品数超过它的候选被跳过。检查对每个配方是线性的，所以这只是在限制修复搜索。
- `maxSeconds`（double，默认 `1.0`）：pass 的时间预算。`<= 0` 表示不限。

== Tag 剪枝预算

注册时运行。

Tag 剪枝的上界。

- `maxTagMembers`（size_t，默认 `1024`）：只剪成员数不超过它的 tag。
- `maxTagPairs`（uint64，默认 `4'000'000`）：只剪 alive-pairs 数不超过它的 tag。
- `maxTagCoverWork`（uint64，默认 `64'000'000`）：列支配测试次数的全局上限。
- `maxWitnessPairs`（uint64，默认 `4'000'000`）：门控 pairs 循环 (a) 所能贡献的上限。
- `maxPrunePairs`（uint64，默认 `4'000'000`）：pair 全域的上限。种子总是保留，传递闭包在总数达到它时停止。
- `maxSiblingRecipes`（size_t，默认 `2048`）：配方数超过它的真实物品不做剪枝。
- `maxWitnessProducers`（size_t，默认 `2048`）：跳过生产者数超过它的 witness 输入。

== 替换剪枝预算

注册时运行。

- `maxSubstitutionWork`（uint64，默认 `64'000'000`）：替换 pass 的测试次数上限。
- `maxSubstitutionCostWork`（uint64，默认 `256'000'000`）：代价关系闭包的步数上限。
- `maxSubstitutionDepth`（uint32，默认 `4`）：替换链的深度上限。
- `maxCostDepth`（uint32，默认 `16`）：代价关系接受的最长推导链，每条配方取一个合格输入。
- `maxSubstitutionGuardItems`（size_t，默认 `256`）：一条记录的 guard 允许的物品数上限。
- `maxSubstitutionGuardTotal`（size_t，默认 `1'000'000`）：所有 guard 记录的总物品数上限。

== Pack 预算

注册时运行。

- `maxNeed`（int64，默认 `2^40`）：pack 传播里需求的数值上界。
- `maxPropIterations`（int，默认 `4096`）：pack 传播的不动点迭代次数上限。

== 已退休的键

以下键仍然被接受，以免旧客户端发来时被拒，但值被忽略，也不会被写回：

- `deadNodePruning`：被 `seedPruning` 取代。
- `maxCostMemo`：早期代价关系备忘的上限，已被移除。
- `outputPruningProfile`：只在 `AW_PROFILE_PRUNING` 的开发构建里存在，是一个性能剖析开关，不是算法选项。

（TODO：我们已经可以移除它了。）

= 求解器预算 `aw::solver::Options`

这是每次查询都重新读入的求解器预算，与 `aw::Options` 分开存放，这样 mod 配置可以在不重新注册图的情况下重新调节它。

- `relativeGap`（double，默认 `0.01`）：允许返回解与最优解相差的比例。到达这个间距即停。
- `absoluteGap`（int64，默认 `0`）：绝对间距下限。`0` 表示只保留相对规则。
- `maxTimeSeconds`（double，默认 `2.0`）：时间预算，`<= 0` 表示不限。超时后 CP-SAT 返回它已经找到的可行方案。
- `numWorkers`（int，默认 `16`）：搜索线程数，`0` 交给求解器决定。
- `randomSeed`（int，默认 `1`）：随机种子。只有在 `numWorkers == 1` 时才能观察到；CP-SAT 自己的默认值是 1。
- `objectiveCap`（int64，默认 `0`）：目标总量的起始上界，因而也是每个变量的上界。`0` 表示从最优值的一个下界推导一个。求解器会在可行前按几何级数增长它，所以它不是硬上限。
- `objectiveUpperBound`（int64，默认 `0`）：已知可行的目标值，来自贪心；`0` 表示没有。与 `objectiveCap` 不同，它#strong[不]取代推导出的起始上界，只是给它封顶并限制上界的重试次数。
- `reducedCostGap`（double，默认 `0.0`）：reduced-cost 定型。求解器探测 LP 松弛附近这个范围内的任何方案；若存在，则每一列被 `floor((incumbent - LP) / d_r)` 界住，界为 0 的列被丢弃，其余的收紧定义域再做最后一次求解。`0` 关闭该 pass（因此默认关闭）。
- `maxCycleRetries`（int，默认 `3`）：调用者在方案被 fireability 检查拒绝后，最多额外求解多少次。`0` 关闭重试。
- `maxStartupGroups`（uint32，默认 `1024`）：启动切割枚举的预算，即最多添加多少个组和多少条 seed 切割。任一项为 `0` 就关闭该 pass。
- `maxStartupGroupMembers`（uint32，默认 `32`）：启动切割中，跳过成员数超过它的环。

= 求解器状态 `aw::solver::State`

`flash` 和其余“按一次求解而变”的东西不属于选项，而是求解状态，随 `solve` 调用一起传入，不参与序列化。`planCrafting` 拥有这个重试循环，是参考调用者；各字段的详细推导见 `Solver.h` 与 `docs/algorithm.typ` 的启动可达性一节。

- `flash`（bool，默认 `false`）：返回第一个平衡可行的方案而不是最省的。初值来自过程级开关 `aw::Options::flash`。
- `solutionHint`（`span<const int64_t>`）：贪心 DAG 预扫描给出的热启动，按列给一个建议值，作为 CP-SAT 的 solution hint 装入；长度与模型不匹配时整条忽略。
- `noGoods`（`vector<vector<int64_t>>`）：已经被后续检查拒绝的完整方案；每条作为一个禁止赋值表装入，使下一次求解不能返回同一向量。
- `entryGroups`、`seedCuts`：启动入口切割与联合 seed 切割。
- `stock`（`span<const int64_t>`）：求解器行空间里每个物品的物理库存，启动切割用它作为可用的种子。

= 与 Java 侧的接口

Java 配置类 `Config.java` 只把一部分旋钮暴露到配置文件，并在注册图之前调用 `setPlannerOptions`、查询之前调用 `setSolverOptions`。字段名与本文一致，所以配置项到 JSON 键的映射就是直接把值放进同名键。注意 `solverFlash` 这一项虽然名字里带 solver，发出去的是规划器选项 `flash`：快速模式现在是过程级规划器开关，求解器只从它取一次求解用的副本。
