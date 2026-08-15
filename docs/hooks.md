# Hooks:生命周期与信任协议

LubanCode 的 hooks 框架。对齐 Claude Code / Codex 的共同骨架(PreToolUse、PermissionRequest、PostToolUse、SessionStart/End、UserPromptSubmit、Stop、Pre/PostCompact、SubagentStart/Stop),配置贴两家的通用形状,少造方言。

- 配置写法与全部字段:见本文"配置"一节。
- 安全模型(项目 hook 信任审查):见"信任边界"。
- 每个事件的输入输出字段:见"协议"。
- 旧格式(pre_tool 等四类)兼容:见"legacy adapter"。

## 心智模型

```
业务层                    hooks 层                     进程层
-----------               ---------------              ------------
RunTurn/loop    --Emit--> HookDispatcher  --stdin JSON--> hook 命令
/clear /resume            匹配/信任/并发/归并           <--stdout JSON---
cli_app(会话起落)         HookRunRecord(全程留痕)
```

业务层只发事件,不亲自遍历配置、不起进程、不猜退出码。匹配、信任审查、并发执行、超时杀树、逐事件 schema 校验、决策归并,全收在 `HookDispatcher`(`src/hooks/`)。

## 事件(第一阶段,Codex 骨架)

| 事件 | 触发时机 | matcher 匹配字段 | 能做什么 |
|---|---|---|---|
| `SessionStart` | 会话开始/恢复/清空/压缩后 | `source`:startup / resume / clear / compact | 追加上下文(additionalContext) |
| `SessionEnd` | 会话收尾 | `reason`:exit / clear | 只作收尾审计 |
| `UserPromptSubmit` | 用户 prompt 送模型前 | 无(只认 `*`) | 阻断本轮;追加上下文 |
| `PreToolUse` | 工具执行前、权限确认前 | `tool_name`(支持 `a\|b` 集合、显式 regex) | deny/ask/allow;`updatedInput` 改参;追加上下文 |
| `PermissionRequest` | 宿主本来要弹确认时 | `tool_name` | deny(拒)/allow(免弹)/不表态(照常问) |
| `PostToolUse` | 工具跑完、结果清洗后 | `tool_name` | 只许追加反馈;不能撤销副作用 |
| `PreCompact` | 压缩前 | `trigger`:manual / auto | 可拦下这次压缩 |
| `PostCompact` | 压缩后 | `trigger` | 审计;上下文重注入走 SessionStart(compact) |
| `SubagentStart` | 前台/后台子代理开跑 | 无 | 审计/记账 |
| `SubagentStop` | 前台/后台子代理收口 | 无 | continue=false 要求再收口一轮(最多一次) |
| `Stop` | 主回合正常收束 | 无 | continue=false 要求补跑一轮(最多一次) |

## 配置

```json
{
  "hooks": {
    "schema_version": 2,
    "PreToolUse": [
      {
        "matcher": "run_command|write_file",
        "hooks": [
          {
            "type": "command",
            "command": "python",
            "args": ["${LUBANCODE_PROJECT_DIR}/.lubancode/hooks/policy.py"],
            "timeout": 20,
            "statusMessage": "检查工具策略"
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "write_file",
        "hooks": [{ "command": "fmt.sh", "async": true, "timeout": 60 }]
      }
    ]
  }
}
```

- `matcher`:省略 / `*` 全匹配;`a|b` 精确集合;`"regex": true` 时按正则解释(不偷偷把特殊字符当正则)。没有匹配字段的事件(UserPromptSubmit/Stop/Subagent*)写具体值会在解析时报错。
- handler 字段:`type`(本期只有 `command`,别的值报错)、`command` + `args`(exec form,不经 shell,Windows 引号泥潭免了;**首选**;Windows 可另给 `command_windows`/`args_windows`)、只写 `command` 不写 `args`(shell 字符串,平台默认 shell:Windows cmd.exe / POSIX sh)、`timeout`(秒,1..600,默认 30)、`async`(本期解析并展示,不执行,见"不做")、`statusMessage`、`failure_policy`(`warn` 默认 / `deny`)。
- 路径占位符:`${LUBANCODE_PROJECT_DIR}` 在装载期替换成项目根。
- 事件名拼错、或照抄了未实现的事件(如 `Notification`),解析报错——不假装支持。
- 旧四类键(`pre_tool`/`post_tool`/`session_start`/`session_end`)继续受支持,走 legacy adapter,可与 schema 2 键同文件共存。

### 来源相加,不再整段覆盖

全局(`~/.lubancode/config.json`)与项目(`<cwd>/.lubancode/config.json`)的 hooks **相加**:两层同事件的 handler 都会跑(用户级在前)。项目配置删不掉、也压不掉全局的任何一条——项目 `hooks` 段不再整段盖掉全局(旧行为,升级时会打一次迁移说明)。决策冲突按固定法归并(deny > ask > allow),不按"谁覆盖谁"。

## 协议

### 输入:stdin JSON

每只 v2 hook 从 stdin 收一份 JSON(大输入不塞环境变量——Windows 环境块有尺寸边界):

```json
{
  "schema_version": 2,
  "hook_event_name": "PreToolUse",
  "hook_run_id": "hookrun_...",
  "session_id": "...", "turn_id": "...",
  "cwd": "...", "transcript_path": "...",
  "permission_mode": "confirm",
  "agent_id": null, "agent_type": null, "parent_agent_id": null,
  "tool_name": "run_command", "tool_use_id": "toolu_...",
  "tool_input": { "command": "dir" }
}
```

工具事件再带 `tool_input`(以及 PostToolUse 的 `tool_response`/`tool_succeeded`);SessionStart 带 `source`;SessionEnd 带 `reason`;Pre/PostCompact 带 `trigger`;SubagentStart/Stop 带 `agent_id`/`agent_type`/`parent_agent_id`(Stop 另带 `agent_transcript_path`/`last_assistant_message`/`stop_hook_active`);Stop 带 `stop_hook_active`/`last_assistant_message`;UserPromptSubmit 带 `prompt`。`agent_id` 非空 = 子代理触发(主代理为 null),主/子不会串账。

工具事件再带 `tool_input`(以及 PostToolUse 的 `tool_response`/`tool_succeeded`);SessionStart 带 `source`;SessionEnd 带 `reason`;Pre/PostCompact 带 `trigger`;SubagentStart/Stop 带 `agent_id`/`agent_type`/`parent_agent_id`(Stop 另带 `agent_transcript_path`/`last_assistant_message`/`stop_hook_active`);Stop 带 `stop_hook_active`/`last_assistant_message`;UserPromptSubmit 带 `prompt`。`agent_id` 非空 = 子代理触发(主代理为 null),主/子不会串账。

### 编码契约(Windows 必读):stdin 是 UTF-8 无 BOM 字节流

宿主往 hook 的 stdin 写的是 **UTF-8、无 BOM** 的原始字节——不按当前 OEM/ANSI 代码页转换,也不猜。中文、emoji、反斜杠、换行、超长 prompt(32KB+)都按字节原样进管道。

但 Windows 上的解释器**默认不按 UTF-8 读管道**:PowerShell 5.1 用 `[Console]::InputEncoding`(中文机器上是 gb2312/GBK)解码重定向的 stdin,直接 `$input` / `[Console]::In.ReadToEnd()` 会把中文读成乱码,`ConvertFrom-Json` 随之报错。hook 脚本必须**明示按 UTF-8 读**。四种写法的最小例子:

**PowerShell 5.1**(拿 StreamReader 明示 UTF-8,不依赖控制台状态):

```powershell
$reader = New-Object IO.StreamReader([Console]::OpenStandardInput(), (New-Object Text.UTF8Encoding $false))
$payload = $reader.ReadToEnd() | ConvertFrom-Json
$payload.prompt   # 中文、emoji 原样到达
```

**pwsh 7**(重定向 stdin 默认就是 UTF-8;显式写法与上面同款,最稳):

```powershell
$payload = [Console]::In.ReadToEnd() | ConvertFrom-Json
```

**Python**(按字节读再解码,不碰 locale):

```python
import sys, json
payload = json.loads(sys.stdin.buffer.read().decode("utf-8"))
print(payload["prompt"])   # 如需往 stdout 写中文/emoji:
sys.stdout.reconfigure(encoding="utf-8")  # 先这一行,再 print
```

**Node**(块累积成 Buffer,按 UTF-8 转字符串):

```javascript
let raw = [];
process.stdin.on("data", (chunk) => raw.push(chunk));
process.stdin.on("end", () => {
  const payload = JSON.parse(Buffer.concat(raw).toString("utf8"));
  console.log(payload.prompt);
});
```

反向同样成立:宿主按"先认 UTF-8"的明示策略解 hook 的 stdout/stderr(见下节)。hook 要往 stdout 写含中文的 JSON,也得按 UTF-8 写——PowerShell 先 `[Console]::OutputEncoding = [Text.Encoding]::UTF8` 再输出,或用 `[IO.File]`/`[Console]::OpenStandardOutput()` 写字节;Python 用 `sys.stdout.reconfigure(encoding="utf-8")` 或 `sys.stdout.buffer.write(...)`。

### 输出:stdout JSON + 退出码

```
0      成功;stdout 是 JSON 就按事件 schema 解析,空白 = 无结构化输出
2      阻断;stderr 作阻断理由
其它   hook 自己失败;按 failure_policy 处理(warn = 记录+放行,deny = 当拦截)
```

```json
{
  "continue": true,
  "stopReason": null,
  "systemMessage": null,
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "命令会删工作区",
    "updatedInput": null,
    "additionalContext": null
  }
}
```

逐事件校验,字段用错报 hook failure(schema_error),不悄悄吞:

- `updatedInput` 只许 PreToolUse,且**只与 allow 同返**;改写后的入参会重新过工具 schema、deny_commands 与权限判断——**不许借 hook 越权**(schema 打回即拦,不悄悄按原参数跑)。
- PermissionRequest 不认 `updatedInput`(改参走 PreToolUse)。
- PostToolUse 不认 `continue: false`(副作用已发生,不许冒充撤销)。
- `hookEventName` 写了就必须与触发事件一致。

### 决策归并(固定法)

| 场景 | 法子 |
|---|---|
| PreToolUse 多钩子 | deny > ask > allow;无人表态 = 无决策 |
| PermissionRequest | 任一 deny 拒;无 deny 且有 allow 免弹;全不表态照常问用户 |
| UserPromptSubmit / PreCompact | 任一 `continue:false` 或 exit 2 拦下 |
| Stop / SubagentStop | 任一 `continue:false` = 再跑一轮;`stop_hook_active` 防咬尾,最多续一次 |
| PostToolUse | 不能拦,反馈追加进模型所见 tool_result |

同事件命中的 handler **并发执行**(总耗时接近最慢一只);收齐后按定义序(来源 → 声明次序)归并,日志与记录顺序稳定,不按谁先跑完谁先说话。同事件下 definition hash 相同的多条定义只执行一次(来源账保留)。

## 后台子代理的 hooks(快照执行 + 安全点归并)

产品主路已是后台 agent,审计、策略、计费不能在最忙处断账。线程规矩:

- **后台线程不碰 dispatcher 账本。** 起后台任务时(宿主主线程)从 dispatcher 拷一份**只读策略快照**(定义表含信任/禁用账)进 `DetachedHookSession`;后台线程在会话里真跑钩子(匹配、信任闸门、并发执行、归并与前台同一个核),跑完的 `HookRunRecord` 只**投递**进 dispatcher 的外部队列。主会话在安全点(轮起、轮收、`/hooks`)归并落账,打一行 `[hooks] 后台子代理 hooks 落账 N 条运行记录`,之后 `/hooks runs` 可查。
- **快照是只读的。** 会话中途 `/hooks trust`、改配置重装,只影响之后新起的后台任务;在跑的那份不追改——信任闸门照过(未信任的照记 `skipped_untrusted`,绝不起进程),不会因为快照旧了就静默放行新命令(hash 对不上就不跑)。
- **同步决策真执行,不静默绕过:**
  - `PreToolUse`:deny 照拦(工具不执行,理由进 tool_result);allow 照放(带 `updatedInput`);**ask 在后台没有终端可问,明示降级为拒绝**——理由写进 tool_result,主会话安全点带一条告警,不装作问过了。
  - `PermissionRequest`:后台本来的硬边界是"需要确认的操作一律拒绝"(无终端)。接上钩子后它成了唯一的策略口:deny 照拒(告警随行);**allow 替人工放行**;不表态维持拒绝——但现在这一票钩子看得见、账上有。
- 触发面:`SubagentStart`/`SubagentStop`(含 continue=false 续跑一轮)与子代理内部工具事件(内置/插件/MCP)。后台任务自己的 prompt 不算用户输入,不发 `UserPromptSubmit`。后台里再派后台(嵌套子代理)那一层不接 hooks——它的工具表没有主会话的 hooks 通道,如实不装。

## 信任边界(供应链防线)

1. **项目 hook 先审后跑。** 项目配置里的 hook 按定义哈希(SHA-256,覆盖 command/args/windows 变体/timeout/async/type)记信任账;**未信任的绝不启进程**,启动时打提示,`/hooks` 里可见。
2. **命令一改,重审。** 命令、参数、timeout、async、handler 类型任一变化,hash 即变,信任失效。
3. **仓库挪了地方,重审。** 信任键 = 配置文件路径 + hash(保守取边:宁可多问一次,不让陌生目录顺走旧信任)。
4. 信任账放 `~/.lubancode/hook-trust.json`(用户目录),**绝不写回仓库**——仓库里的配置改不动自己的信任账。
5. user 级 hook 不走审查(文件在用户自己手里);managed 钩子(规划中,本期无装载源)将由策略信任、不可由普通用户在 UI 里禁用。
6. 未信任的项目 hook 不拖累全局钩子——user 级照跑。

## `/hooks` 管理面

```
/hooks                 列出全部定义:事件/matcher/命令/来源/信任状态/hash 短码/sync/timeout/最近结果
/hooks runs [N]        最近 N 条运行记录(新在前)
/hooks trust <#id>     审查后信任该定义的当前 hash(即时生效)
/hooks untrust <#id>   撤信(下次起跳过)
/hooks disable <#id>   禁用(managed 不可禁)
/hooks enable <#id>    重新启用
```

## 失败策略与运行记录

超时、起不来、坏 JSON、schema 不合、未知退出码,每一条都生成 `HookRunRecord`(outcome:ok/blocked/failure/timeout/spawn_failed/schema_error/skipped_*),`/hooks runs` 可查——**不只往 stderr 丢一行**。严禁把超时静默当放行:warn 策略也明报"这次门卫没起来"。`failure_policy: deny` 时,门卫没起来按拦截算(只在能拦的事件上生效)。

每条 v2 运行记录还带 stderr 的明示解码账:事件、退出码、耗时之外,`stderr:` 一行列出解码口径(`utf-8` / `cp936` 这类)与 stderr 首段,超上限带 `…(截断)` 标志。宿主对子进程 stdout/stderr 都是"先认 UTF-8,次选控制台输出页/系统 ANSI 页,命中标注;都解不动就保留原始字节的十六进制摘要"——绝不把中文报错无声替换成替换符。

退出码语义上,"未知退出码不一概当 deny,也不静默当成功":exit 1 = hook 自己坏了(failure,按 policy 走);只有 exit 2(v2)或任意非零(legacy pre_tool)才是真拦截。

## legacy adapter(旧四类)

`pre_tool`/`post_tool`/`session_start`/`session_end` 继续受支持,**守旧语义**:

- 任意非零退出码仍拦 `pre_tool`(不只 exit 2);
- `LUBAN_TOOL_NAME`/`LUBAN_TOOL_INPUT`/`LUBAN_TOOL_RESULT`(前 8192 字节)/`LUBAN_TOOL_IS_ERROR` 环境变量照导(**已废弃**,新能力只走 stdin JSON);
- shell 字符串照跑,固定 30 秒超时;
- 同样过信任审查与 `/hooks` 台账(标 `[legacy]`)。

升级不会让旧安全策略静默失效;旧协议将来迁到 schema 2(启动时提示)。

## 工具覆盖矩阵

| 工具路径 | PreToolUse/PermissionRequest/PostToolUse |
|---|---|
| 内置本地工具(read_file/write_file/edit_file/search/todo_write/ask_user/...) | 全支持(本地 function-tool path) |
| run_command | 全支持 |
| MCP 工具(mcp__server__tool) | 全支持 |
| Lua 工具 / C ABI 插件工具 | 全支持 |
| agent(子代理本身作为工具) | 全支持(拦的是"启动子代理"这个动作) |
| 子代理内部工具 | 前台子代理:全支持(带 agent_id/agent_type);后台子代理:全支持(只读快照执行,见"后台子代理的 hooks") |
| hosted web/search(服务端内置,如 Responses 的 web_search_call) | **不走**本地执行链,三事件均不触发 |
| transport poll(peer 传输) | 不触发(非工具调用) |

"全支持"指走 `RunOneTool` 的本地 function-tool path;不走该路径的在表内明说,不笼统宣称"所有工具都支持"。

## 不做 / 边界(如实)

- **async handler 本期不执行**(解析、展示、校验照常,执行记 skipped_async)。安全点投递(空闲不唤起模型、批次收口后送达、会话结束作废)是后续工作;不执行就不可能拿 async 做权限决定,也不假装支持。
- **大输出 spill 未实现**:additionalContext 目前直接进上下文(超大时截断标注),落盘+头尾预览后续接。
- **嵌套后台(后台子代理再派后台子代理)不接 hooks**:那一层的工具表没有主会话的 hooks 通道。主会话直接派的后台子代理全支持(见"后台子代理的 hooks")。
- **Stop/SubagentStop 续跑最多一次**,续跑轮没有输入监听(ESC 由外层兜底)。
- **SessionEnd 非必达**:它是 advisory 收尾——正常退出、异常被接住时会发;进程被硬杀(taskkill / kill -9 / 断电)时析构不运行,事件不会发出。协议如实如此,不承诺必达。
- **第二阶段事件**(PostToolUseFailure、PostToolBatch、StopFailure、Notification、TaskCreated/Completed、ConfigChange、InstructionsLoaded、WorktreeCreate/Remove 等)按真实产品能力补,先不造空壳。
- **HTTP/MCP/prompt/agent 四类 handler** 暂不支持,`type` 只认 `command`(结构留了门)。

## 真机手测清单

1. 项目配 `PreToolUse` v2 钩子(deny 版),启动 → stderr 提示未信任;工具调用不拦;`/hooks` 看到 `待审查`;`/hooks trust #1` 后即时生效拦截。
2. 改钩子命令(加个参数)重启 → `/hooks` 里回到待审查(hash 变了)。
3. 旧格式 `pre_tool`(退出码 1)照拦;`post_tool` 非零只警告。
4. user + project 两层 hooks 同时在 → 两层都跑(相加),启动打迁移说明。
5. PreToolUse 返回 allow + updatedInput → 确认不弹、按改写参数执行;返回的参数类型故意写错 → 被拦并提示 schema。
6. 确认档 confirm 下配 PermissionRequest(allow)→ 不弹确认直接跑;deny → 打一行拒绝。
7. `/hooks runs` 看运行记录;`/hooks disable #N` 后该钩子记 skipped_disabled。
8. /clear、/resume、/compact、自动压缩各走一遍,钩子(SessionStart matcher=resume/clear/compact、PreCompact manual/auto)各命中各的。
9. 大 tool input(>32KB)经 stdin 到钩子,原样可达(环境变量路径早撑爆了)。
10. 中文 prompt("只回复 OK，不调用工具")经 UserPromptSubmit 钩子原样到达:PowerShell 5.1 / pwsh 7 / Python 三支钩子各配一遍,照"编码契约"一节的例程读 stdin,`/hooks runs` 里该条 outcome=ok;故意用 `$input` 直读(不按契约)会看到乱码——那是脚本没按契约读,不是宿主写错。
11. emoji(如 🎉)、反斜杠路径、带换行的 prompt 各试一遍;钩子 stderr 里打中文,`/hooks runs` 的 stderr 首段不乱码。
12. 后台子代理(`run_in_background` 或交互会话默认后台)跑一单:任务收尾后等主会话下一轮起/收,屏上出现 `[hooks] 后台子代理 hooks 落账 N 条运行记录`,`/hooks runs` 里 SubagentStart/PreToolUse/PostToolUse/SubagentStop 各有流水,stdin JSON 的 agent_id 是这只后台任务的 id。
13. 后台 + PreToolUse(deny 版):工具被拦、任务台账里那笔工具调用是 error 且理由说清;ask 版:工具被按"后台无终端,降级为拒"处理,主会话安全点带告警;PermissionRequest(allow 版)配一个 needs_confirm 工具:后台不再直接拒绝,钩子放行即执行,`/hooks runs` 有这一票。
