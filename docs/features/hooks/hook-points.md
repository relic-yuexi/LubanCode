# 挂点手册

[返回](README.md) · [中间件配置手册](middleware-config.md) · [Host API 手册](host-api.md)

挂点(hookPoint)决定插入哪个**已定义的执行时点**——不是脚本任意插进
C++ 行号。功能实现可替换,正式消息接纳、请求冻结、权限复核、身份发行
和提交边界仍由宿主执行。

## 通用合同

Lua handler 的形状(冻结):

```lua
return {
  handler名 = function(ctx, input, next)
    -- ctx 只读:dispatchId/invocationId/hookId/hookPoint/stage/depth/
    --          registryRevision/package,另有 ctx.deny(code, message)
    -- input  挂点专用候选副本:本地怎么改都不碰 session
    -- next   至多调用一次;零次返回 = 短路
    local downstream = next(input)      -- {status, value, code, message}
    return {output = downstream.value}  -- 透传;可另带 effects
  end,
}
```

- `next` 至多一次;第二次调用拿 `hook.next.already_consumed`(协议违规,
  变 handler 失败),下游不重跑。跨 invocation 保存 next、终态后调用一律
  `hook.next.expired`。
- 返回 `ctx.deny(code, message)` 是**业务拒绝**:结局 Denied,用户看到
  message,不落错误码表;与脚本错误(错误码表)是两件事。
- 返回 nil = 透传(已消费 next)或纯观察;返回表带 `effects` 数组携带
  效果候选,宿主按挂点矩阵逐项裁决(applied/rejected 都落账)。
- 观察者(observer=true)无 next、可并发、只许 context.append /
  result.supplement 两枚效果。

## 挂点一览

效果矩阵是"这个位置允不允许这枚效果"的冻结答案;不在表里的效果给了
也 reject,账上可查。

| 挂点 | 时点 | input 形状 | 允许的效果 | 生产接线 |
| --- | --- | --- | --- | --- |
| PreSystem | system 拼装中 | `{system, cause, …}` | system.change, context.append | 合同冻结,接线沿批次 |
| PostSystem | system 落稳后 | 同上 | 无(不再改当前 system) | 合同冻结,接线沿批次 |
| PreUser | 队列输入获接纳机会后、正式提交前 | `{prompt, …}` | input.rewrite, admission.decision, context.append | 已接(CLI/one-shot/app-server) |
| PostUser | user 消息与接纳关系落稳后 | `{prompt, …}` | context.append, result.supplement | 已接 |
| PreAssistant | 模型原始回复准入 | `{message, …}` | admission.decision | 合同冻结,接线沿批次 |
| PostAssistant | assistant 定稿后 | `{message, …}` | context.append, result.supplement | 合同冻结,接线沿批次 |
| PreTurn / PostTurn | 回合首尾 | 回合摘要 | PreTurn: context.append, admission.decision;PostTurn: 无 | PostTurn/goal.review 已接 |
| PreStep / PostStep | 步骤首尾 | 步骤摘要 | PreStep: input.rewrite, context.append, admission.decision;PostStep: 无 | 合同冻结,接线沿批次 |
| PreRequest | 每次实际模型请求,分三段 | 请求快照(见下) | 见分段表 | 已接(mutate/estimate/capacity) |
| PreAction | 参数/后端候选退栈后、真实执行前 | `{toolName, input, …}` | input.rewrite, backend.select, admission.decision, context.append | 合同冻结,接线沿批次 |
| PostAction | 原始结果保存后、唯一 tool message 提交前 | `{toolName, content, isError, …}` | result.replace, result.supplement, result.filter | 合同冻结,接线沿批次 |

## PreRequest 的三段(§4.36)

`mutate → freeze(宿主掌握,不可注册)→ estimate → capacity`

| 段 | 能做什么 | 不能做什么 |
| --- | --- | --- |
| mutate | 经 `next(candidate)` 改输入;改写被采用则本批重新准备请求 | 不能越过 freeze |
| estimate | 产出 EST1 形状的测量结果(估算值即输出) | 输入已冻结,改写一律 reject |
| capacity | 消费宿主落稳的估算与输出预留,回 allow/recover/reject | 不改输入、不产出估算 |

估算/容量是 required 内置槽位(`context.token_estimate` /
`context.capacity_check`):Lua 可同名替换实现,不能靠删 handler 绕过
完整请求检查。compact/title 等旁路请求的估算走同一 estimate 槽
(`purpose=compact` 进匹配),旁路不自带第二份公式。

## 取消与恢复

- Esc/父任务取消旗贯通 Lua guard(指令 hook)、HTTP 与工具桥——同一根
  真值。取消后 dispatch 整体 cancelled,链项记 `skipped_cancelled`,
  context 候选不被采用;取消后只准清理,不再提交业务改写。
- 事件账次序:requested → (skipped 汇总) → started → proposed(before/
  after_next/short_circuit)→ consumed → settled(applied/rejected)→
  completed/failed/cancelled。候选先存,不冒充 handler 已完成。
- 只读 replay 零脚本执行;恢复断点(候选已存/next 已消费/下游完成等
  八类)按 `src/runtime/middleware_recovery` 的判例表推进,已完成项
  不重跑。

## 禁用边界(作者不许碰)

- 不伪造 provider/model/usage;派生内容不冒充模型原话或工具原话。
- 不自报来源层级提权(层级由装载位置定,清单里没有 source 字段)。
- 不直调 MCP Client、不绕过宿主写文件、不伪造 tool call 开权限。
- 脚本顶层只构造函数与常量;发网络、读写业务文件、改状态都放 handler 里
  经 Host API 走。

完整可跑样例见 `examples/hooks/`(每枚带 fixtures,`lubancode hook test`
即验)。
