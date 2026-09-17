# business-deny:业务拒绝(PreUser)

输入里疑似明文凭据就拒绝接纳。演示:

- `ctx.deny(code, message)` 是业务决定,不是脚本错误——结局是 Denied,
  用户看到 message,不落 hook.lua.* 错误码;
- 短路后链尾零次(terminalRuns=0),后续 handler 不伪造执行。

跑法:`lubancode hook test .`。
