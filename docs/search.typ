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
}
```

这里的 `chunks` 是 UTF-8 编码的、以 `\0` 分隔的字符串，而 `offsets` 则是指向它的指针。每个物品在 `offsets` 里都会占有连续的 `fieldsPerHandle` 这么多个数（其实固定是三个），分别是 ResourceLocation，英文名以及中文名。

我们会复制 `chunks` 所指向的内容。毕竟只注册一次，而且只有几 MB，应当是问题不大的。
