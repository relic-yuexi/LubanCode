# Host API 手册(Lua 侧 `luban.*`)

[返回](README.md) · [挂点手册](hook-points.md) · [中间件配置手册](middleware-config.md)

权限总则:**清单 capabilities 申请 ∩ 槽位允许 ∩ 宿主授权**,缺一头就
`capability_not_granted`。默认不开放原始 SessionRuntime、可变历史、宿主
地址、任意 C 模块加载及 shell;文件写入、外部工具调用和状态改变都走
宿主执行件——脚本不能先绕过宿主写完文件,再补一句"我执行了"。

Lua 库白名单:base(文件口除外)、string、table、math、utf8、os 的
时间四函数。`os.execute`/`io.*`/`require`/`dofile`/`loadfile` 一类真调
即报错,不是纸面 nil。

失败形状统一为 `nil, err`(err 是 `{code, message}` 表,HTTP 另带
`status`/`retryable`);码是稳定串,只增不改。

## 能力词表

| capabilities 词条 | 开什么 |
| --- | --- |
| `http` | luban.http / luban.secrets(自己的权限与预算,不借 tool call) |
| `fs.read` | luban.fs.read / list |
| `fs.write` | luban.fs.write(须另授写根) |
| `state` | luban.state.*(按包隔离) |
| `context` | luban.context.append(候选,采用经效果管道) |
| `log` | luban.log.event |
| `tools` | luban.tools.call / list(宿主统一执行服务,含 MCP) |

## luban.http.request

```lua
local resp, err = luban.http.request({
  method = "GET",                      -- 只认 GET/POST
  url = "https://kb.local/notes",
  headers = {["X-Trace"] = "abc"},     -- 键值都须字符串;Authorization/
                                       -- Cookie/Host 一类宿主代填,自写即拒
  json = {query = "热换"},             -- 与 body 二选一
  timeout_ms = 1500,                   -- 只降不升(宿主帽之内取小)
  auth = {type = "bearer", secret = "kb_token"},  -- secret 收逻辑 id 或
                                                  -- luban.secrets.ref(...)
})
if resp == nil then
  -- err.code: invalid_request / network_failed / secret_not_declared /
  --           secret_missing
  return next(input)
end
-- resp = {status, headers(小写键), body, json?(body 可解析时), url, bytes}
-- HTTP 非 2xx 不混作网络错:status 原样走成功形状,自己判。
if resp.status ~= 200 then ... end
local data = resp.json
```

取消:Esc/父任务取消旗贯通 transport。Secret 明文不进任何日志。

## luban.secrets.available / ref

```lua
if not luban.secrets.available("kb_token") then
  return ctx.deny("secret_missing", "缺 kb_token,先配置再启用本 hook")
end
local ref = luban.secrets.ref("kb_token")   -- 不透明引用,不解析不持值
```

## luban.fs.read / write / list(受控文件)

```lua
local text = luban.fs.read("notes/today.md")   -- 相对授权根;越界/符号链接
                                               -- 逃逸拒绝(hook.fs.path_denied)
luban.fs.write("cache/last-run.json", "{}")    -- 原子写;须授 fs.write
for _, entry in ipairs(luban.fs.list("notes")) do
  -- entry = {name = "a.md", dir = false}
end
```

限额:单次读/写 256 KiB、列目 512 条;超帽 `hook.fs.too_large`。

## luban.state.get / set / keys(插件状态)

```lua
luban.state.set("last_query", {text = "热换", at = os.time()})
local prev = luban.state.get("last_query")
for _, key in ipairs(luban.state.keys()) do ... end
```

按包隔离(别的 hook 看不见你的键);限额:64 键 / 单值 64 KiB / 每包
1 MiB,超帽 `hook.state.quota_exceeded`。**跨 invocation 的状态只有这里**
——Lua 局部变量每次调用归零。

## luban.context.append(上下文候选)

```lua
luban.context.append("知识库补充材料:\n" .. text, "knowledge.recall")
```

只是提交候选:handler 正常返回后折成 `context.append` 效果,由执行核按
挂点矩阵裁决采用;PostUser 不能回写原 user(§4.47)。限额:16 条 /
64 KiB,超帽 `hook.context.quota_exceeded`。来源自动带
`hook:<包名>[/<note>]`。

## luban.log.event(结构化日志)

```lua
luban.log.event("warn", {hook = ctx.hookId, what = "recall_failed",
                         code = tostring(err.code)})
```

level 只认 info/warn/error;fields 须是字符串键表,长值截 2 KiB 并标
truncated。显示投影归宿主,**不自动注入模型**——要进上下文走
context.append。

## luban.tools.call / list(工具桥,含 MCP)

```lua
local listed = luban.tools.list()          -- 授权名单内、注册表现存的工具
for _, tool in ipairs(listed) do ... end

local result, err = luban.tools.call("kb.search", {query = "热换", limit = 3})
if result == nil then
  -- err.code: hook.tool.not_allowed(名单外)/ hook.tool.unknown(未注册)/
  --           hook.tool.call_cap(总帽 8 次/invocation)/
  --           hook.tool.repeat_cap(同名 3 次)/
  --           hook.tool.recursion_depth(嵌套 2 层)/
  --           hook.tool.drained(会话排空中)
  return next(input)
end
-- result = {status, content, isError, executionId, contentTruncated, details}
--   status: finished | failed | cancelled | unknown
--   unknown = 超时/断连——远端可能已执行,不自动重试(§5.1)
```

- 路径:Lua → 宿主统一执行服务 → 注册表解析/schema 复验/准入 →
  Tool::execute(与模型 Action 同一底座)→ MCP client → 服务端。不直调
  Client、不伪造 tool_call、不向 provider 塞孤立 tool message。
- 每次子执行独立 `executionId`,tool.execution.* 全链留账(linkage 带
  hookInvocationId)——引用它可追溯。
- 正文截到 32 KiB(`contentTruncated` 标);超时/取消不证明远端未执行。

## ctx.deny(业务拒绝)

```lua
return ctx.deny("secret_in_prompt", "输入里有疑似明文凭据,请改用 Secret。")
```

结局是 Denied(不是脚本错误),本轮不接纳,用户看得到 message。

## 排空窗口的行为

会话 clear/exit 排空期间:新 invocation 不授 http/fs/tools(只留进程内
的 state/log/context);在途 invocation 的工具桥对新的子执行回
`hook.tool.drained`。迟到终态落发起会话的账。

## 完整可跑样例

`examples/hooks/` 六枚覆盖本文全部 API 的用法;每枚
`lubancode hook test <目录>` 即验(fixtures 里 HTTP/工具按编排应答)。
