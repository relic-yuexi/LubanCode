# 工具调用流程

[文档首页](../README.md) · [工具参考](../reference/tools.md) · [Query 数据流](query-data-flow.md) · [Hooks 流程](hooks-flow.md) · [PTC 手册](../features/tools/ptc.md)

这页讲一枚工具调用怎样从模型手里落到本机，又怎样把结果送回模型。工具名、参数与输出上限见[工具参考](../reference/tools.md)；四种 wire 的报文差异见[Query 数据流](query-data-flow.md)。

## 四个计数别混

- `turn`：用户交来一轮任务，直到主循环收束。
- `step`：这一轮里发一次模型请求，收一条 assistant 消息。
- `tool call`：assistant 消息里一枚工具调用块。
- `request`：真正送到 provider 的一次 HTTP/SSE 请求。

一个 step 可以带多枚工具调用。默认策略下 LubanCode 逐枚执行，收齐结果，再开下一个 step；开 `parallel_read` 策略后，同一 step 里连续的只读段可并行，写入仍独占（见[多枚调用怎样收账](#多枚调用怎样收账)）。

## 总流程

```mermaid
flowchart TD
    accTitle: 工具调用总流程
    accDescr: 宿主组装请求并接收模型消息；若消息含工具调用，便逐枚执行、收齐结果、追加历史，再发下一步请求。
    U[用户消息] --> B[组装 system / history / tools]
    B --> API[向模型发请求]
    API --> A[流式拼成 assistant 消息]
    A --> Q{含工具调用块?}
    Q -- 否 --> Z[本轮收束]
    Q -- 是 --> L[按策略调度:连续读段并行 / 其余独占]
    L --> R[收齐 ToolResultBlock]
    R --> H[作为一条 user 消息追加进 history]
    H --> B
```

模型并不直接碰文件、shell 或网络。它只产一枚有名字、有 id、有 JSON 入参的调用块。宿主认出它，再走本地执行链。

## 请求前：工具表怎样来

每个工具都向 `ToolRegistry` 交三样东西：名字、说明、输入 JSON Schema。`AgentLoop` 每个 step 都重新拼工具表。这样 `tool_search` 若在半途挂上新工具，下一次请求立刻能看见它。

工具分两层：

- 核心工具直接进请求。
- 延迟工具先留在注册表，暂不进 schema。模型须调用 `tool_search`，命中并挂载后，下一步才带完整定义。

模型若硬叫一枚尚未挂载的工具，宿主不执行，只回一条错误，叫它先走 `tool_search`。

## 模型回包：先拼消息

四种 provider wire 各有各的流事件。assembler 把文字、思考、工具名与 JSON 参数攒成一条中立 assistant 消息。Agent loop 只认中立块，不把 Chat、Anthropic、Responses、Gemini 四套细节带进执行层。

终止帧偶尔会写 `end_turn`，消息里却已有工具块。此时信消息内容：照样执行工具。若只信终止原因，history 里便会留下孤零零的调用块，下一次请求容易被远端拒绝。

## `RunOneTool` 的七道关

```mermaid
flowchart TD
    accTitle: 单枚工具执行闸门
    accDescr: 工具调用依次经过注册、挂载、权限、参数、确认与 Hook 检查，放行后执行并清洗结果，再回填模型。
    C[ToolUseBlock] --> F{注册表找得到?}
    F -- 否 --> E1[回未知工具错误]
    F -- 是 --> M{已挂载且本角色可用?}
    M -- 否 --> E2[回挂载或角色限制错误]
    M -- 是 --> P[发 PreToolUse]
    P --> D{deny?}
    D -- 是 --> E3[回拦截结果]
    D -- 否 --> I{updatedInput?}
    I -- 是 --> S[按工具 schema 复检]
    S --> V{通过?}
    V -- 否 --> E4[拦截，不拿旧参数偷跑]
    V -- 是 --> CFM
    I -- 否 --> CFM{工具需确认?}
    CFM -- 是 --> PR[PermissionRequest / 用户确认]
    PR --> OK{放行?}
    OK -- 否 --> E5[回拒绝结果]
    OK -- 是 --> X[执行 Tool::execute]
    CFM -- 否 --> X
    X --> UTF[清洗成合法 UTF-8]
    UTF --> POST[发 PostToolUse]
    POST --> OUT[追加反馈，产 ToolResultBlock]
```

次序不能乱：

1. 查名字。找不到便回错误。
2. 查过滤器。延迟工具未挂载、子代理角色无权使用，都在这里停。
3. 跑 `PreToolUse`。`deny` 直接拦，连确认框也不弹。
4. 若 Hook 给了 `updatedInput`，拿工具 schema 重新验。验不过就拦；不能偷偷改回旧参数执行。
5. 工具若标了 `needs_confirm`，再走权限策略、`PermissionRequest` 与用户确认。
6. 调 `Tool::execute`。
7. 清洗外部文本，再跑 `PostToolUse`。Hook 只能追加反馈，不能撤销已经发生的副作用。

JSON 工具调用与 PTC 脚本都走这一个 `RunOneTool`。PTC 只换模型怎样编排调用，不换权限、Hook、schema 与执行边界。

这条链内部分四个阶段：`PrepareToolCall`（门禁审批，上表 1–5 关）→ `MarkExecutionStarted`（持久记录 started，副作用闸）→ `ExecuteApprovedTool`（真正执行，可上 worker 线程）→ `CompleteToolCall`（Post Hook、结果捕获、显示与回填）。串行路四阶段依次同线程跑；并行路只有第三阶段上 worker，阶段一、二、四留在主线程——权限确认、Hook、审计没有第二扇门。

## 多枚调用怎样收账

同一条 assistant 消息若含三枚调用，程序按出现次序执行。三份结果收进同一条 `role=user` 消息，各自用调用 id 配对：

```text
assistant: ToolUse(a), ToolUse(b), ToolUse(c)
user:      ToolResult(a), ToolResult(b), ToolResult(c)
```

### 执行策略两档

- **`exclusive`（默认）**：全部逐枚串行。不声明策略就是这档，行为与并行改造前一字不差——确认框、Hook、终端转录与副作用次序都有一条清楚的账。
- **`parallel_read`**：连续只读段有界并行。配置写在 `agent.tool_execution`，并发上限 `agent.parallel_read_concurrency`（1–16，默认 4；配 1 即回到完整串行语义）。

### 段调度与屏障

`parallel_read` 下，批次按模型声明序切成"连续读段"与"独占节点"：

```text
模型声明序：read(A), read(B), write(A), read(A), read(C), edit(B)

执行次序： [read(A) || read(B)]
                    ↓ 全段收口（结果落账、Post Hook 跑完）
                 write(A)
                    ↓ 收口
            [read(A) || read(C)]
                    ↓ 收口
                 edit(B)
```

规矩：

- 只有审定过的内置只读工具（首批 `read_file`、`search`）进段。判定认三件套：名字在册、注册来源是内置（插件/MCP 不许借名影子放行）、工具不需确认。写、undo、shell、Git、未知工具、插件、Lua、MCP、宿主状态操作（`job_cancel` 一类）一律独占——不按名字里含 read/get 就放行。
- `tool_invoke` 包装先解引用出真实目标再判策略；解不开按独占走原错误路径。
- 读段与独占节点互为屏障：读段全收口才跑独占节点，独占收口才启下段；后面的 read 不会被提到前面的 write 之前。同一路径先写后读，读到的是新内容。
- 结果按原槽位回填、原 `tool_use_id` 配对，完成先后不改排列。
- 三种情况整批回退串行，行为与 `exclusive` 一致：任一只 Pre/Post 工具 Hook 在场（把 Hook 摁在主线程不够——它可能在另一只 read 执行时改文件）；批次混入 job_handle/native_deferred 异步协议；并发上限配 1。

各宿主（终端、one-shot、app-server、Gateway、子代理）吃同一条配置轴：子代理整份继承主会话策略。协议宿主没有终端审批口，确认类工具照旧走各自审批面（app-server 的 permission/request、渠道的 fail-closed 名单），与执行策略互不干扰。

用户若在中途按 ESC，正在跑的工具等它收口，结果照常入 history；尚未轮到的调用各补一条“未执行”错误结果。配对仍齐，随后退出本轮。取消、异常、落账失败的收口合同在串行/并行两路同款：每枚调用恰一份终态与一份协议结果。

## 结果怎样回模型

工具返回 `content` 与 `is_error`。宿主先修掉非法 UTF-8，再包装成 `ToolResultBlock`。错误也是结果，不另开一条隐形异常通道。

收齐后，结果消息追加进 history。下一次 step 重拼请求：旧 assistant 调用块在前，新结果块紧随其后。模型据此继续调用别的工具，或给最终正文。

长结果进请求前还会过上下文视图层：重复只读结果可折成引用，超长结果可换 artifact 预览。执行与 session 真账不受影响。详见[上下文压缩机制](../features/context/compaction.md)。

## 权限与 Hook 怎样相接

`PreToolUse` 能表态 `deny / ask / allow`。归并顺序是 `deny > ask > allow`：

- `deny`：工具不执行。
- `ask`：即使普通策略本想放行，也要问用户。
- `allow`：可跳过普通确认；硬权限规则仍可拦。

`PermissionRequest` 只在宿主原本要确认时发。它可拒绝、免弹，或不表态交回原流程。完整归并规则见[Hooks 流程](hooks-flow.md)。

## 常见失败

| 结果 | 是否执行工具 | 模型会看到什么 |
| --- | --- | --- |
| 未知工具 | 否 | 未知工具错误 |
| 延迟工具未挂载 | 否 | 先用 `tool_search` 的指路 |
| Hook 拒绝 | 否 | 拒绝理由与 Hook 附注 |
| Hook 改参后 schema 不合 | 否 | 改参校验错误 |
| 用户拒绝确认 | 否 | 用户拒绝执行 |
| 工具自身报错 | 已调用 | `is_error=true` 与错误正文 |
| `PostToolUse` 失败 | 工具已调用 | 原结果照留，另有告警；不能回滚 |

## 安全边界

- 工具 schema 只是参数形状，不是操作系统沙箱。
- `needs_confirm`、权限策略和 Hook 都在宿主侧执行，模型越不过。
- 外部工具输出先过 UTF-8 边界，再进 Hook、history 与终端。
- 工具调用与结果须成对。恢复、打断、流错误都要补齐这条契约。
- PTC、插件、Lua、MCP 最终仍须走同一执行链；若另开旁路，便会漏掉权限与审计。

## 源码入口

- `src/agent/loop.cpp`：`AgentLoop::Run` 与 `RunOneTool`（四阶段拆链与批次第二遍的段调度接线）。
- `src/agent/tool_batch_schedule.cpp`：策略两档解析、放行名单、段划批与有界执行器。
- `src/tools/registry.cpp`：工具注册与查找。
- `src/tools/schema_check.cpp`：Hook 改参后的 schema 复检。
- `src/api/assembler.cpp`：流事件拼成中立消息。
- `src/tools/tool_search.cpp`：延迟工具检索与挂载。
- `src/ptc/`：程序化调用 runner；最终回到 `RunOneTool`。

相关测试集中在 `tests/unit/agent/test_loop.cpp`、`tests/unit/agent/test_tool_execution_chain.cpp`（拆链）、`tests/unit/agent/test_tool_batch_scheduling.cpp`（段调度与屏障）、`tests/unit/agent/test_tool_batch_p3.cpp`（持久化/资源/性能）、`tests/unit/agent/test_agent_tool.cpp`、`tests/unit/tools/test_tool_search.cpp`、`tests/integration/ptc/test_ptc_tool.cpp`、`tests/unit/api/test_request_prefix.cpp` 与各 wire 请求测试。
