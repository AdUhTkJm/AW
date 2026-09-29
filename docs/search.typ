#import "@preview/cuti:0.4.0"
#set document(title: "Pruning dominated tag members")
#set page(paper: "a4", margin: 2.3cm)
#set text(size: 10.5pt, font: ("Libertinus Serif", "Noto Serif CJK SC"))
#set heading(numbering: "1.1")
#show link: underline

#align(center)[
  #text(size: 17pt, weight: "bold")[Minecraft 物品搜索]
]

= 简介

除了合成规划以外，为了让 mod 真正可用，我们还需要让玩家搜索他们想要合成的物品。考虑到 ATM10 已经有六万五千个物品，我们选择将这一部分也传入 JNI，以防打一个字卡五秒。

不过，除了物品序号一样以外，这个搜索部分跟规划器本体没有任何关系。但它们同属于 JNI，所以被链接进了同一个 `aw_jni` 里面。

== 数据格式

只有真实物品有名字，而 tag 是没有的。传入的数据是

```java
public final class SearchKernel {
  void registerSearch(ByteBuffer[] chunks, int[] offsets, int fieldsPerHandle);
  int[] search(String query, int limit);
}
```

这里的 `chunks` 是 UTF-8 编码的、以 `\0` 分隔的字符串，而 `offsets` 则是指向它的指针。每个物品在 `offsets` 里都会占有连续的 `fieldsPerHandle` 这么多个数（其实固定是三个），分别是 ResourceLocation，英文名以及中文名。

我们会复制 `chunks` 所指向的内容。毕竟只注册一次，而且只有几 MB，应当是问题不大的。

== 匹配规则

查询先按字段同样的分隔符切成 token：ResourceLocation 按 `_`、`:` 等非字母数字分割，英文名按空格和标点分割，拼音按汉字分割（每个汉字一个音节）。

对英文名和 ResourceLocation，满足下面任一条即算命中：

+ 每个查询 token 都是某个条目 token 的前缀，且条目 token 的下标递增（允许跳过）。所以 `shadow iron`、`iron ingot`、`shadow ingot` 都能命中 `Shadow Iron Ingot`。
+ 查询只有一个 token 时，它也可以是某个条目 token 的子串，例如 `stone` 命中 `Redstone`。多 token 不做子串，且任何 token 都不能跨越条目 token，所以 `shadowironingot` 不会命中 `Shadow Iron Ingot`。

中文名分两路：原文走连续子串，所以 `铁锭`、`影铁` 命中而 `暗铁` 不命中；拼音见下节。

== 拼音

不把查询切成音节，而是把整个中文名的拼音连成一个字符串，并记下每个汉字起始的字节边界。查询 token 只要能在某个边界处成为后缀的前缀即算命中，结束位置可以落在音节中间：

- `tied` 命中：在 `tie` 的边界处 `tieding` 以 `tied` 开头；
- `ied` 不命中：没有任何边界后缀以 `ied` 开头，它跨过了 `tie` 和 `ding`；
- `yingtie` 命中：从 `ying` 的边界开始，属于「中间几个字」；
- `td` 命中：`td` 是首字母串 `aytd` 的子串。

多音字会生成多个变体（最多 8 个，优先只改一个字的组合）。于是 `重锤` 的主读音 `zhongchui` 和异读 `chongchui` 都能命中，而主要读音的变体在排序里更靠前。非汉字字符不进入拼音串，否则英文回退名里的每个字母都会变成一个首字母。

== 排序

每个条目取各字段里最好的一次匹配，再按下面的元组排序（越小越靠前）：

```text
(匹配种类, 变体权重, 字段, 起始位置, 跨度, handle)
```

匹配种类由好到差为：整段相等、从字段开头的前缀、字段中连续、带间隔的子序列、首字母子串、token 内子串。字段优先级为英文名、中文名、资源位置。

== 性能

Release 下对 65114 个条目的实测：注册约 33 ms（一次性），单次查询平均约 5.5 ms、最差约 7 ms。这仍是一次线性扫描；如果以后嫌慢，可以在注册时建 token 前缀到 handle 的倒排表，先用第一个 token 缩小候选，再验证其余 token。

注册在 `aw-planner` 工作线程上跑，查询在渲染线程上跑，所以两者用 `std::shared_mutex` 隔开：注册独占，查询共享。
