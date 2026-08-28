# Full Stack Sample(moontide.full-stack 0.1.0)

完整包夹具:浏览器冒烟验收一套。六类组件各一件,另有 assets、docs 两兜资料。
本文写给没读过 LubanCode 源码的人——照着目录放文件,就能说清会发现什么、会执行什么、哪一步要信任。

## 装了什么

| 目录 | 组件 | canonical id | 干什么 |
| --- | --- | --- | --- |
| `agents/` | Agent 定义 | `moontide.full-stack:browser-tester` | 照清单验收网页的角色 |
| `prompts/profiles/` | Prompt Profile | `moontide.full-stack:browser-tester` | 该角色的身份与 Web 验收文案 |
| `skills/` | Skill | `moontide.full-stack:browser-testing` | 验收章法:摘要、核对、给证据 |
| `workflows/` | Workflow | `moontide.full-stack:smoke-test` | 采摘要 -> Agent 复核 -> 缴单 |
| `plugins/` | process Plugin | `moontide.full-stack:dom-analyzer` | `inspect` 工具:本地统计 HTML |
| `mcp/` | MCP server | `moontide.full-stack:browser` | `navigate`/`screenshot` 两件示例工具 |

`assets/` 放验收清单,`docs/usage.md` 讲用法,均不自动加载,组件按相对路径读。

## 放进目录后,会发生什么

1. **立刻被发现。** 把本目录(整只,不是里头某层)放进
   `~/.lubancode/packages/`(个人)或 `<项目>/.lubancode/packages/`(团队),
   `/package reload` 或重启后进 `/package list`。
2. **静态校验。** 清单、六件组件、包内引用逐一过各自 parser。全对,才可列、可 inspect;
   有一处错,整包 invalid,一件组件都不挂。
3. **内容组件先上。** Agent、Prompt、Skill、Workflow 属内容组件,校验过后即登记进各 Catalog。
   `/skills`、`/workflow list` 能看到它们,canonical id 全名展示。

## 会执行什么

两件代码组件,启动前看得一清二楚:

| 组件 | 进程 | 命令 | 出网 | 环境 |
| --- | --- | --- | --- | --- |
| Plugin `dom-analyzer` | 短命 Python,每次调用一只 | `python <包根>/plugins/dom-analyzer/runner.py` | 否 | 只传最小集,无密钥 |
| MCP `browser` | 常驻 Node,会话期一只 | `node <包根>/mcp/browser/server.js` | 否 | 读宿主 `BROWSER_TOKEN`,清单只记变量名 |

两件都是夹具:Plugin 纯本地统计,不真联网;MCP 只记账与占位,不真开浏览器。

注册出来的工具名(wire 名与展示名,编码规矩见 `docs/packages.md` §6.1):

| 展示名 | wire 名 |
| --- | --- |
| `plugin__moontide.full-stack.dom-analyzer__inspect` | `plugin__moontide%2Efull-stack%2Edom-analyzer__inspect` |
| `mcp__moontide.full-stack.browser__navigate` | `mcp__moontide%2Efull-stack%2Ebrowser__navigate` |
| `mcp__moontide.full-stack.browser__screenshot` | `mcp__moontide%2Efull-stack%2Ebrowser__screenshot` |

## 哪一步要信任

- 放在 `~/.lubancode/packages/`:你亲手放的,视作已安装来源,不加信任门。
- 放在 `<项目>/.lubancode/packages/`:这是外来代码。项目包的代码组件先过内容哈希信任门——
  `/package trust moontide.full-stack` 批一次,批的是当前每一字节。未信任时:整包仍可发现、
  可 doctor;Plugin 与 MCP 一个进程都不起,依赖它们的 Agent、Workflow 标 unavailable。
- 改动任何文件(哪怕一字节),哈希变,旧信任失效,须重批。
- 批准页会亮:内容哈希、两件代码组件的清单、上表那些命令与工具名。

## 卸载与数据

删目录即卸载。运行数据不写包内,落在 `~/.lubancode/package-data/moontide.full-stack/`,
卸载默认保留,`purge` 才删。启停状态记在 `~/.lubancode/package-state.json`,不在包里。

## 与最小包的差别

`../minimal-content-only/` 只有一件 Skill:放进目录即用,零信任动作。本包多出的门槛,
全在 Plugin 与 MCP 这两件代码组件上——这就是"发现不等于执行"的分界线。
