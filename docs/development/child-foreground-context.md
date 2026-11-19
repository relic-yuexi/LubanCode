# 前台子 Agent 本次调用上下文

本批只接前台派工的取消与因果材料。公开 SDK 子 Agent、异步审批、后台借用寿命和全部异常终态另批交付。原生验收只在远程 CI 跑。

## 递什么，谁持有

`AgentTool` 与派工薄壳把本次 `execute(input, context)` 递到同一协调器。取消旗只借到阻塞调用返回；不写 shared Hooks，不存后台环境，不捕进后台线程。父 invocation 整枚拷值，明确叫 `parent_invocation_cause`，只供前台嵌套派工串因果。任务私有 handle 可保存这份值，不能保存取消指针。

孩子没有经过 SDK 接纳 operation。孩子 invocation 留空；不得拿父 operation 配子 session 拼五键。当前 SDK Memory 写口继续拒缺 owner 身份。本批不另造 child scope，也不伪称孩子已有 SDK operation。

## 取消与旧路

前台沿原 `CancelChain` 并入本次旗、任务取消、墙钟和旧 Hooks.cancel 后备；嵌套前台由工具 context 递下自己那根合并旗。任务终态也认本次取消。旧 input-only 入口仍可用；CLI 同步 routed/floored 审批与后台冻结放行账照旧。后台派工不保留调用旗或父 cause，仍用任务自有取消源。

本次材料存在 typed 请求与调用栈，不能借共享工具成员存“最近一场”。同一薄壳两场请求各持一根旗；main 活 Hooks 与参数连败账仍按原单宿主线程装配合同使用。

## 退场与验收

沿现有 ExecutionTurnScope 撤回调、CancelChain 收链和前台阻塞退场，不改 Session Close、子账 Finish 或 registry 回收。新接线不声称修完 RunSubagentTask 后半段异常欠账。

远端原生回归须实跑：直接前台、薄壳与嵌套前台取消；未置位旗不误停；同薄壳两场互不串旗；父 cause 拷值、空 context 不造 cause；孩子 invocation 缺席；旧 Hooks.cancel、同步确认和后台拒询保留。新增来源须在 ASan 精确登记、执行非零 case，三平台全量继续验旧路。本地只跑静态、文档与纯数据检查。
