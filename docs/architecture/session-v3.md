# Session v3：当前实现与边界

[架构总览](README.md) · [会话指南](../features/sessions/README.md) · [字段合同](trajectory-v3-schema.md) · [旧设计清理清单](../development/v3-legacy-audit.md)

这页说明当前源码怎样写账、恢复、压缩和接扩展。字段以 schema 为准；尚未接线的合同单列，不把设计定案当成运行完成。

## 默认格式与目录

`NewSessionV3WriteEnabled()` 默认返回 true。只有环境变量 `LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS` **恰为字符串 `0`**，新会话才走 v2。未设置、`1` 或其他值都走 v3。旧目录按自身格式读取，不改旧文件，不自动转换。

```text
~/.lubancode/workspaces/<workspace_key>/sessions/<sessionId>/
  <sessionId>.jsonl
  artifacts/
  subagents/<childSessionId>/
    <childSessionId>.jsonl
    artifacts/
```

v3 首行是 `system` 消息，`seq=1`，`turnId=null`。不另造 v2 `session.json`；v2 的 `main.jsonl`、manifest 与读写分支仍留在代码中。Workflow 编排账另管运行图，不能把它当成子代理账直接改名。

## 一份原账，几种视图

JSONL 顶层只有 `message` 与 `event`。两类行共用单写者、递增 `seq` 和哈希链。

| 数据 | 用途 |
| --- | --- |
| `message` | `system/user/assistant/tool` 四角色正文，带身份、用途与来源 |
| `event` | 请求、工具执行、结果选用、hook、压缩等事实与状态 |
| `HistoryTimeline` | 显示完整历史；压缩原文仍能查看 |
| `ProjectModelContext` | 按已提交上下文链选取模型输入，排除未采用候选与 compact 内部问答 |
| `model.request.prepared` | 记录这次请求引用的消息、system 版本及请求材料 |

`seq` 只说明本文件追加次序，不是 turn、step 或 action 编号。上下文顺序由 `(contextId, contextRevision)` 对应链决定。`display` 只管显示，不能据此删模型输入。

四角色已进 `api::Role` 与四家 adapter。不过 `AgentLoop` 当前仍把一批 `ToolResultBlock` 装入 `Role::User`；v3 写桥再记独立 tool 消息。读代码时须认出这层过渡容器，不能说主循环已经处处使用独立 Tool 消息。

## 请求、工具与 usage

请求准备、发送、流式开始、增量、终态各有记录。增量按批落事件，响应收口后落正式 assistant 消息；取消可留下 `completionStatus=interrupted`。账随执行推进，不等整轮结束才统一保存。

工具用 `actionId` 串起执行与结果。全文进结果仓，模型见选中的预览；结果持久化、选用和 tool 消息分别记账。默认文本预览预算为 32 KiB，多文件共享，来源说明也占预算。预览降档保留原文与原消息，生成派生消息，再提交新上下文链。

模型生成的 assistant 持 `provider/wire/model/responseModel/usage`。服务端没报 usage 就写 null。迟到或更正另记 `model.usage.appended`，不能重复累计。终端活统计与磁盘离线统计是不同读口：`ReadSessionUsage()` 仍沿 v2 流枚举和 `EventEnvelope` 解析，新 v3 档的 `/usage` 完整汇总尚待迁移。

## 压缩与恢复

`/compact` 和自动压缩先检查当前 writer；v3 场交 `RunV3Compact`。过程是范围计划、容量检查、模型候选、校验、摘要消息、`compact.applied`。压缩请求走 cheap 路由，未配时按路由规则回落 normal。

`compact.applied` 落稳后，宿主调用 `ProjectV3ContextHistory()`，再 `ReplaceHistory()`，同场下一请求随即使用新链。候选生成或校验通过都不等于采用。失败、取消、拒收不替换旧上下文；提交后若投影失败，当前命令入口只报错并建议 `/resume` 重开，内存仍留旧史；强制阻止后续发送尚待补齐，不能把目标合同当成已有保护。

连续压缩会把当前摘要纳入材料，再用新摘要替换它。请求装不下时按整轮退回范围，不拆工具配对；退无可退就拒收。`/compact --dry-run` 在 v3 场尚未接线，会明报未执行。

resume 采用 resume-as-new：验源账及来源链，建新 session 接续，源文件不追加。跨会话引用带 `sessionId/runId/seq/id/hash`。历史显示与模型输入分别恢复，压缩原文不会重新全塞回请求。缺引用、坏链须明报，不能拿当前环境补造过去。

## Hook 与各宿主

Lua 中间件已接 `PreUser`、`PostUser`、`PreRequest`；请求阶段分改写、估算、容量检查。同一 `(hookPoint,name)` 先选实现，再按阶段和依赖排序。内置估算与容量槽位为 required，同键覆盖不解除必执行要求。执行完成与效果采用分别写 `hook.completed` 和 `hook.effects.applied/rejected`。

Host API 已有文件、状态、上下文追加、日志与工具调用服务。实际权限取清单申请与宿主授权交集；生产默认只开 state/log。调用 MCP 走宿主工具服务并记子执行账。旧 command HookDispatcher 仍在，旧配置不能直接当 Lua 包使用。接线见 `middleware_assembly.cpp`、`middleware_runtime.cpp`、`middleware_v3_sink.cpp` 与 `hook_host_services.cpp`。

终端、one-shot、AppServer 已共用 SessionService 入口。AppServer 现行 1.2 的 `thread/read` 与 `thread/resume` 提供只读历史/恢复视图；`thread/resume` 不启动执行。进程事件 `lastSeq` 不能拿来替代 v3 文件游标，完整 2.0 恢复协议仍是后续工作。

## 尚待接通

| 范围 | 当前边界 |
| --- | --- |
| Telemetry | projector 仍解析旧 `EventEnvelope`，服务仍枚举 `main.jsonl`；配置能开不等于能投影 v3 |
| Memory | 召回桥仍取旧 recorder；新版作用域、CAS、忘记屏障、正式 memory 消息与请求引用合同尚待实施 |
| Workflow | 现有编排与 journal 可用；独立节点 Session v3、output commit 和完整节点恢复须另行接线 |
| Goal / Loop / btw | 现有功能按原实现运行；新增持久事务、独立上下文、验收与联合调度合同不能据已有命令认定完成 |
| 压缩边界 | 中途 compact 的 `parentTurnId` 尚未由命令入口传入；共用 loop 仍有 hard trim，不能声称所有请求裁剪都已记入 v3 链 |
| 归档/删除 | 归档仍要求 v2 manifest；删除仍查 main.jsonl 的封口与末 hash，完整 v3 生命周期待接 |
| 控制事件 | 标题、审批档、环境、verification 等缺项见 schema 第十节，不能把内存生效当持久恢复完成 |

逐项代码证据、替代方向和清理条件见[旧设计清理清单](../development/v3-legacy-audit.md)。

## 验证入口

源码入口：`src/trajectory/v3/`、`src/runtime/trajectory_session.cpp`、`src/runtime/v3_compact_runtime.cpp`、`src/app/commands/session_commands.cpp`。读写、来源链、投影和压缩测试在 `tests/unit/trajectory_v3/`；四角色合同在 `tests/unit/api/test_four_role_kernel.cpp`。

```powershell
python scripts/validate_trajectory_v3.py --help
python scripts/tests/v3_accept_matrix.py --help
bash scripts/check_docs.sh
git diff --check
```

验收矩阵驱动真实 exe，但模型端使用本地剧本化假后端，部分崩溃场景以截断账本模拟。它验证协议与恢复行为，不证明真实模型摘要质量；历史验收结果也不等于本次重跑。
