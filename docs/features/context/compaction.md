# 上下文压缩机制

[文档首页](../../README.md) · [会话指南](../sessions/README.md) · [Session v3](../../architecture/session-v3.md) · [字段合同](../../architecture/trajectory-v3-schema.md)

这页讲默认 v3 会话怎样压缩、何时换上下文、失败后留下什么。旧四分区双账算法另存[旧算法参考](../../architecture/context/compaction.md)，不再用它解释新版。

## 怎么用

```text
/context
/compact
/compact 保留当前任务约束、测试结论和未完成事项
```

`/context` 查看预算；`/compact` 手动触发，尾部文字作为本次压缩重点。v3 的 `/compact --dry-run` 尚未接线，会明报没有发请求、没有改上下文。

压缩请求走 cheap 模型路由，未配时按路由规则回落 normal。容量取压缩路由自己的窗口、输出预留和协议余量，不把主模型窗口直接借过去。窗口未知时不能声称已核过容量。

## 原文去哪儿

| 数据 | 压缩怎样处理 |
| --- | --- |
| v3 JSONL 原账 | 只追加，不删旧消息，不改旧行 |
| 完整历史视图 | 仍能查看原文，并显示历次压缩分界 |
| 模型当前上下文 | 成功后改成选中 system、新摘要与保留消息 |
| 工具结果仓 | 全文与来源仍在；模型预览不等于全文 |
| 项目记忆 | 另有召回与写入流程，本次 compact 不整理记忆库 |

历史可查不等于每次请求都携带全部历史。v3 `/export`、`/copy` 当前走链投影，也不能当作完整原账备份。

## 什么时候动

手动入口与自动入口都先辨认当前 writer，再走 `RunV3Compact`。自动入口沿主循环压力检查，在回合前或下一请求发送前进入；命令接线分别记录 `threshold` 或 `pre_send_overflow` 原因。水位触发与压缩请求自身的容量检查是两道门。

源码还留着共用 loop 的结构压缩、hard trim 与旧 v2 分支。v3 提供预览降档事件和链提交能力，但不能据此声称所有工作视图裁剪都已接入同一条链。剩余工作见[清理清单 V3-GAP-07](../../development/v3-legacy-audit.md)。

## 一次成功怎样落账

```text
compact.requested
  → 范围计划与容量检查（必要时 compact.range.retreated）
  → compact.started
  → compact 专用 system / user 材料
  → 模型请求、响应与候选 assistant
  → compact.validation.started / completed
  → context_summary 消息（此时尚未采用）
  → compact.applied（提交新上下文链）
  → ProjectV3ContextHistory → ReplaceHistory
```

compact 内部问答有独立用途与身份，不混入主对话有效输入。摘要引用候选，applied 引用校验和摘要，同时保存旧/新上下文版本、hash、移除/保留消息引用及前后 token。

只有 `compact.applied` 落稳，才发布新上下文。当前宿主已接内存换账，同场下一请求随即使用新摘要，不必退出再 resume。

## 范围、容量与连续压缩

范围按完整 turn 和工具配对选取，保住未完成任务所需消息。压缩请求过大时，运行时按整轮退回范围，逐步记 `compact.range.retreated`；默认最多退三步，每步目标至少让出 8192 个估算 token。退无可退仍超限便拒收，不拿明知超限的请求碰运气。

连续压缩会把当前旧摘要和后续可压缩历史一起纳入材料。成功后新摘要替换旧摘要，不把历次摘要堆在上下文头部。没有足够材料或收益不足，也应拒收，不空转。

默认输入估算用 UTF-8 字节数除以四并向上取整。compact 估算已接 PreRequest 中间件槽位；估算不是供应商实报。applied 留下当时数字，恢复显示不拿今天的估算器重算过去。

## 失败与恢复

| 停在哪儿 | 怎样处理 |
| --- | --- |
| 模型失败、用户取消、校验拒收 | 记 failed/cancelled/rejected，不采用候选 |
| 候选或校验已写，applied 尚未写 | 旧上下文仍有效，恢复不能抢先采用摘要 |
| applied 落稳，内存尚未发布 | 恢复按已提交链重建新上下文 |
| applied 写盘失败 | 不发布新内存上下文 |
| 提交后读投影失败 | 当前仅报错并建议 /resume，内存留旧史；阻止后续发送尚待接线 |

当前命令入口尚未给中途 compact 填入主 turn 的 `parentTurnId`；provider 报 context_overflow 后的完整恢复调度也不能据上述自动入口认定已完成。

## 源码与验证

- `src/runtime/v3_compact_runtime.*`：范围、容量、候选、校验和提交。
- `src/trajectory/v3/compact.*`、`writer.*`、`reader.*`：持久合同与链投影。
- `src/app/commands/session_commands.cpp`：手动/自动分派、路由、同场内存换账。
- `tests/unit/trajectory_v3/test_v3_compact_runtime.cpp`、`test_v3_compact.cpp`：运行时与账本测试。
- `scripts/tests/v3_accept_matrix.py`：驱动 exe，检查实发请求、compact 与恢复链；模型使用本地假后端。

本页依据源码说明行为。协议与恢复验收不能替代真实模型摘要质量评测。
