# 上下文压缩机制

[文档首页](../../README.md) · [会话与上下文](../sessions/README.md) · [Context 压缩算法深挖](../../architecture/context/compaction.md) · [Query 数据流](../../architecture/query-data-flow.md) · [项目记忆流程](../../architecture/memory/flow.md)

这页只讲一件事：长会话怎样收短，又怎样守住尚未做完的活。配置字段与命令语法见[配置手册](../../reference/configuration.md)和[命令参考](../../reference/commands.md)。项目记忆另走一套链，见[项目记忆流程](../../architecture/memory/flow.md)。

若要追到主 system 是否改写、episode 怎样切、map/reduce 怎样装预算、哪些中间结果只做弱验收，直接读 [Context 压缩算法深挖](../../architecture/context/compaction.md)。

## 先分清三本账

| 名称 | 装什么 | 压缩会不会改它 |
| --- | --- | --- |
| `history` | 下一次模型请求要看的活历史 | 会。语义压缩会换成“存档 + 热区” |
| session JSONL | 用户消息、回复、工具来回、usage、压缩事件 | 不删旧账，只追加 `compact_v2` |
| project memory | 跨会话仍有用的项目事实与偏好 | 不碰。它按下一条用户消息另行召回 |

所以，`/compact` 不是删聊天记录，也不是整理项目记忆。它只换模型眼前那本账。旧流水仍能 `/resume`，也能 `/export`。

## L0 到 L4 的阶梯

源码把它叫“四级压缩阶梯”：L0 是未压缩基线，真正收束从 L1 算起。

| 层级 | 手段 | 何时动 | 改哪里 | 原文去向 |
| --- | --- | --- | --- | --- |
| L0 | 原样与按需装载 | 结果尚短 | 请求视图照放 | history、session 原样 |
| L1 | snip / 结构压缩 | 每次拼请求视图 | 重复引用、版本标记、artifact 预览 | history、session 原样；长结果另存 artifact |
| L2 | 按需 microcompact | 模型显式调用 `context_read(summarize=true)` | 在历史尾部追加局部摘要工具结果 | artifact 与 session 原文都留着 |
| L3 | global compact | 手工、回合前或回合中达到窗口线 | 用存档替换冷 history，留下热区 | session 旧账仍在 |
| L4 | hard trim | L3 没赶上或失败，工作视图仍过大 | 裁请求视图 | session 原文仍在；终端告警 |

L1 先收拾工具结果。相同只读结果只留一份正文，后来者改成引用；旧版本留下预览，新版本照常保留；超长结果换成 artifact 引用。带副作用的工具结果不判重。

L2 与 L3 都会请模型写摘要，分量却不同。L2 一次只收调用方点名的一枚 artifact，把摘要当新工具结果添在 history 尾部；L3 收整段旧对话，会换活 history。L4 连活 history 也不改，只钉住一份较短工作视图。四层不可混叫。

## L2：工具结果微压缩

程序默认不跑 L2。L1 artifact 预览不够时，模型先用 `context_search`、`context_read` 找证据。只有原文很长，逐段读取反倒更贵，才显式调用 `context_read(artifact_id="aNNNN", summarize=true)`。

这次调用走 cheap 模型。单枚输入最多取 24 KiB，超出便从原文头尾各取一半，并在行边界收口。模型须回严格 JSON：`summary` 与 `key_facts`。摘要少于 20 字节、JSON 坏、请求失败或 45 秒超时，工具便报错；旧消息、L1 预览与 artifact 原文一概不动。

L2 守四条规矩：

- 不扫描冷区，不在回合收尾自动花 token。
- 输入永远从 artifact blob 原文来，不拿旧摘要再摘要。
- 摘要带 source artifact id 与产出模型，作为新的 `tool_result` 追加。证据不够时可用 `context_read` 追回全文。
- 成败都不删 blob，也不改 session 旧行；本次工具调用与结果照常追加。

`summarize=true` 不可与 `chunk_id`、`line_start`、`line_count` 混用。子代理只拿到普通 `context_read`，不露摘要参数，免得几路 cheap 请求同时抢账。

## 何时触发

```mermaid
flowchart TD
    A[准备发下一次模型请求] --> B[估算 system + tools + history + 输出预留]
    B --> C{达到窗口约 80%?}
    C -- 否 --> D[照常发请求]
    C -- 是 --> E[在安全点请求语义压缩]
    E --> F{压缩成功并验收?}
    F -- 是 --> G[换成存档 + 热区]
    G --> D
    F -- 否 --> H[保留原 history]
    H --> I[必要时让字符硬裁兜底并告警]
    I --> D
```

触发口有三处：

- 手工：`/compact [重点说明]`。
- 回合前：上一回实测 usage 已过阈值，下一条用户消息入模前先收一次。
- 回合中：工具结果已收齐、下一次请求尚未发出时，拿 projected token 再算一次。过线便先压，免得长工具循环撞窗。

回合中的安全点很要紧。工具不能跑到一半被切断；调用块与结果块也不能拆散。故而程序只在两次模型请求之间动 history。

## 预算怎样算

压缩模型也有自己的窗口。可用输入预算是：

```text
compact 模型窗口 - 输出预留 - 协议余量
```

程序再估压缩指令与待压历史。装得下，走单次压缩。装不下，走分层压缩。模型窗口若已知而输入仍塞不下，程序明报失败，不截一半去碰运气。窗口未知时仍可发请求，只在结果中记下“未做窗口校验”。

## 单次压缩

单次压缩把冷历史一次交给压缩模型。模型须交两份东西：

1. 六栏 Markdown 存档：任务目标、已证实事实、关键决策、涉及文件与符号、关键命令与结果、未完成事项。
2. 一枚 JSON manifest：`goal`、`constraints`、`open_items`、`next_action`。

用户给 `/compact` 添的重点会写进压缩要求。活动待办也会逐字列给模型，不能随手改写。

## 分层压缩

历史塞不进单次请求时，程序先切阶段，再做 map/reduce：

```mermaid
flowchart LR
    H[完整 history] --> Z[划出最近热区]
    Z --> C[冷区按 episode 切块]
    C --> M1[局部小结 1]
    C --> M2[局部小结 2]
    C --> MN[局部小结 N]
    M1 --> R[归并]
    M2 --> R
    MN --> R
    P[上一轮存档] -. 只在归并时作参考 .-> R
    R --> A[终稿存档 + manifest]
    A --> N[新 history: 存档 + 热区]
```

episode 有两种显式边界：一条新的外层用户输入，或一次 `todo_write` 带来的计划变化。块界落在轮边界，工具调用与工具结果同进同退。

每块先从原始消息生局部小结。上一轮存档不进 map，只在 reduce 时作参考。这样可挡住“旧摘要再抄成新摘要”的层层失真。归并材料若还太大，便两两再并，直到装得下。

若整份历史都挤在最后一轮，冷区为空，也就无从分块。这时程序拒绝，绝不把一轮巨型输入劈碎。

## 热区怎样留下

摘要通过后，程序从最后一轮往前收整轮消息，直到热区预算用尽。默认热区预算是 12k token。最新用户消息所在轮无论多大，都整轮留下。

最终形状如下：

```text
[对话存档,此前内容已压缩]
六栏摘要
JSON manifest

最近几轮原始消息
最近工具调用与结果
```

压缩从不拆开一组工具调用与结果。否则恢复后再发请求，远端多半会因配对不全报错。

## 验收闸

模型回了摘要，不等于可以换账。程序还要查：

- 正文去掉空白后不少于 40 个 UTF-8 字符。
- manifest 能解析，`goal` 非空。
- 活动待办逐项留在 `open_items`，只许调整空白。
- 工具调用与结果仍成对。

有一项不过，旧 history 原样留下。程序不会拿半份摘要顶掉真账。

## 落盘与恢复

成功后，程序把压缩边界追加成 `compact_v2` 事件。事件里有 archive、`kept_from`、压缩 epoch、触发来源、实现路径、map 块数、reduce 轮次、前后 token、manifest 与 `source_digest`。

`/resume` 读到它，便按同一规则重建“存档 + 热区”。旧版 `compact` 事件仍能读。session 旧消息未删，导出时也能看见压缩点。

压缩会改请求前缀，故而另开一个 cache epoch，并把断因记作 compact。后续工具来回再从这份新前缀往后追加。

## 失败时怎样认

| 现象 | 程序怎样办 | 去哪里查 |
| --- | --- | --- |
| 压缩模型请求失败 | 原 history 不动 | 终端错误、provider 配置 |
| 压缩输入超过模型窗口 | 不发请求，明报预算不足 | `compact_model`、模型目录里的窗口 |
| 摘要漏待办或 manifest 坏 | 拒收摘要 | 压缩错误与活动 todo |
| 单轮巨型历史无法分块 | 明报拒绝 | 缩小单次输入，或另开会话 |
| 按需摘要请求或 JSON 失败 | 工具报错；旧消息、L1 artifact 预览与原文不动 | cheap 路由与工具结果 |
| 语义压缩失败后仍过大 | 字符硬裁兜底，终端告警 | `/export` 查完整流水 |

`/compact --dry-run` 只看结构压缩能省多少、哪些内容被钉住。它不发模型请求，也不改 history。

## 源码入口

- `src/agent/compact.cpp`：单次压缩、episode 切分、map/reduce、manifest 校验。
- `src/agent/loop.cpp`：回合中 projected 估算、结构压缩、硬裁与 cache epoch。
- `src/agent/context.cpp`：轮级裁剪与工具结果截断。
- `src/sessions/session_store.cpp`：`compact` / `compact_v2` 的写入与回放。
- `src/agent/context_events.cpp`：L1 结构压缩、artifact/重复/版本视图。
- `src/agent/microcompact.cpp`：L2 按需局部摘要、输入裁面与格式校验。

相关测试集中在 `tests/unit/agent/test_compact.cpp`、`tests/unit/memory/test_microcompact.cpp`、`tests/unit/api/test_context.cpp`、`tests/unit/api/test_context_events.cpp`、`tests/unit/sessions/test_session_store.cpp`、`tests/unit/api/test_request_prefix.cpp` 与 `tests/unit/memory/test_artifact_store.cpp`。
