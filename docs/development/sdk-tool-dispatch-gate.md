# 工具实际调用前的内部执行门

[阶段进度](sdk-stage-status.md) · [模型发送门](sdk-model-send-gate.md) · [Managed 身份](lubancore-managed-identity.md) · [Sandbox 设计](../architecture/sandbox-platform-design.md)

基线为功能分支 `5aaad8b5`。这片补共用执行接点，候选尚待三平台 CI。
它不开放 Managed 会话，不注册沙箱，也不改公开 SDK ABI。

## 公开什么、谁拥有、怎样收场

`TurnWiring::on_tool_dispatch_gate` 属内部宿主接线。它收实际工具调用 ID、
解析后的工具名、最终参数和本次 `ToolExecutionContext`，返回
`std::expected<void, std::string>`。公开宿主尚不能据此配置完整租户授权。

回调随原 TurnWiring 借给这轮执行，不另起线程。同步调用走当前执行线程；
并行读段走各自 worker。宿主持捕获，活过整批 worker 退出，随后按原回合
退场次序释放。回调须能并发调用，不能重入 Agent、关闭 owner 或改 wiring。
后续 Policy 接线仍须进已有回调屏障，不持 SDK 状态锁调宿主。

## 成功与拒绝路径

```text
解析工具 → 改参与 schema → 原权限与审批 → Started 意图落账
         → 原追踪写入门 → 本次 invocation → 实际线程重查 → Tool::execute
```

这道门置于 `ExecuteApprovedTool`，不看 `needs_confirm`。只读、已审批和
原权限预放行工具都经过它。PTC、动态工具解析及并行读段凡共用这条函数，
也经过同一处检查。最终参数在此只读，不准门再改参。

门为空，LocalTrusted 保原行为。允许则原样调用工具；拒绝固定返回
`tool.dispatch.denied`，异常固定返回 `tool.dispatch.gate_exception`，
均记 `permission_declined`。宿主返回正文、异常正文不进入结果和轨迹。
只捕获回调本身；工具异常仍走原执行错误路径。

配置了门才新增调用前取消检查。回调前后均查本次真实取消旗；取消优先，
返回 `runtime.tool.cancelled_before_start`，不调工具。门为空时不改旧直接
调用的取消语义。此处检查不承诺与另一线程撤权或取消原子同步。

Started 仍表执行意图，可能已经落账。拒绝结果沿原 `CompleteToolCall`
收尾、配对并提交，后置 Hook 仍会看见失败结果；不能说此前零 I/O，
也不能把拒绝结果当作工具已运行。崩溃卡在 Started 后仍沿原未知态恢复。

## 扩展边界

后续 Managed 宿主用闭包持原发起主体、固定资源及 operation，逐次调已有
PolicyProvider；当前查询者不能替换原发起者。Sandbox 可用同一门核冻结
执行域和能力，再交实际后端；一份旧允许决定不能供后续调用通行。

这片不接 Policy，也不重定向文件、搜索、命令到 guest。已启动后台 Job
的真正进程派发须守 Job 自己的最终门；Summary、Compact、MCP 内部调用和
其他直接 `Tool::execute` 旁路须另盘点。不能据此宣称沙箱或 ACL 已交付。

## 验收

来源为 `tests/unit/runtime/test_tool_trace.cpp`，保原案例和断言。
真 AgentLoop 五枚工具前两枚放行、后三枚撤权，核实际调用次数、逐枚
终态与五份模型结果。真 RunOneTool 覆盖只读/需审批、改参、允许、拒绝、
异常、审批期间撤权、Started 后撤权及取消。机密失败串不得进返回正文。

本地只查源码、文档和纯数据；三平台原生与现有回归须取本源远端证据。
