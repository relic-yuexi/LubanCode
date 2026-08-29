# Luban Browser(luban.browser 0.1.0)

官方浏览器内容包。前身是 `examples/packages/browser-agent`(content-only
的一枚 Skill 加一张练习页),阶段 5 归拢成官方包:Agent、四只 Workflow、
练习页都在,mcp/ 里另添一件**可选** code-bearing 启动器。

边界一句话:**Runtime 是本事,Package 是装法。** 浏览器本体——
BrowserRuntime、Playwright、页签账、Console/Network journal——归
LubanCode 核心产品层,代码在仓库自带的 `browser/` 目录,随 LubanCode
发布,不吃任何 Package 信任(`docs/reference/browser-runtime.md`)。
本包只教 Agent 办事、排检查步骤,一件浏览器代码都不带(mcp/ 那件是
"找核心"的薄启动器,不是 Runtime 副本)。

## 装了什么

| 目录 | 组件 | canonical id | 干什么 |
| --- | --- | --- | --- |
| `agents/browser-reviewer.yaml` | Agent | `luban.browser:browser-reviewer` | 审网页、复现界面故障、回报带证据的验收员;只读工具面 |
| `skills/browser-agent/` | Skill | `luban.browser:browser-agent` | 操作纪律:快照→找 ref→动→复验;journal 查账;停手线 |
| `workflows/smoke/` | Workflow | `luban.browser:smoke` | 冒烟:开页→等关键元素(即断言)→快照→复核缴单 |
| `workflows/responsive/` | Workflow | `luban.browser:responsive` | 双视口对照:桌面服+手机服各开一页,截图比对布局与溢出 |
| `workflows/console-error/` | Workflow | `luban.browser:console-error` | 开页收 Console journal,断言无不容忍的 error;容忍清单可配 |
| `workflows/network-failure/` | Workflow | `luban.browser:network-failure` | 开页收 Network journal,断言关键请求非失败、且真发出去过 |
| `mcp/browser-launcher/` | MCP(code-bearing) | `luban.browser:browser-launcher` | 可选薄启动器:找到核心 browser MCP 并接管 stdio |
| `assets/practice-page/` | 资料 | —(不自动加载) | 零依赖练习页,含巡账靶子(error、失败请求、溢出宽卡) |

## 卸掉这只包,核心照常

Browser Tab、App Server 浏览器协议、`browser/` 的 MCP——都不依赖本包。
拿走包,浏览器一寸不少;少的只是专门 Agent、操作章法 Skill 与四只
Workflow(`/agents`、`/skills`、`/workflow list` 里它们随包隐现)。
反过来,包里没有任何组件能替代或遮蔽核心浏览器:Agent 靠
`requires.tools` 断言 `mcp__browser__*` 在表,缺了标 unavailable,
不悄悄降级。

## 安装与验收

把整只 `browser/` 目录(不是里头某层)放进:

```text
~/.lubancode/packages/            个人,全项目可见
<项目>/.lubancode/packages/       随 git 分发,团队人人 clone 即得
```

重启 LubanCode 后:

```text
/package list                       → luban.browser 0.1.0,valid,agents:1 workflows:4 mcp:1
/package show luban.browser         → 全账:组件、文件数、内容哈希、来源
/package doctor luban.browser       → 静态诊断,应 valid(含 mcp 组件的 code-bearing 提示)
/agents                             → 见 luban.browser:browser-reviewer [package]
/agent doctor luban.browser:browser-reviewer
                                    → 静态预检;browser MCP 没配时工具项如实标 ✗
/workflow list                      → 见 luban.browser:smoke 等四只
```

开发调试不必拷贝:

```text
lubancode --package-dir <本包所在目录>
```

`--package-dir` 是开发层,指**包的上一层目录**(里面每只子目录是一只包)。

## 挂核心 browser MCP(一次性,推荐路)

包教的是工具用法;工具来自核心 BrowserRuntime 的 MCP 入口。编辑
`~/.lubancode/config.json` 的 mcpServers(args 里的路径按你机器上
LubanCode 仓库的实际位置写):

```json
"mcpServers": {
  "browser": {
    "command": "node",
    "args": ["<仓库根>/browser/server.js", "--engine", "chromium", "--headless"]
  },
  "browser-mobile": {
    "command": "node",
    "args": ["<仓库根>/browser/server.js", "--engine", "chromium", "--headless",
             "--profile", "ephemeral", "--viewport", "390x844"]
  }
}
```

`browser` 是主入口,Agent 与 smoke/console-error/network-failure 三只
workflow 都用它。`browser-mobile` 只有 responsive 要:viewport 是服务级
启动参数,一只服务一只视口,双视口就得两只服务。没配它,responsive 跑到
open_mobile 直接 tool_unavailable 失败——错误里看得明白。

前置:Node.js 18+;在仓库 `browser/` 目录 `npm install` 加
`npx playwright install chromium`。去掉 `--headless` 可见真窗口。
配好后 `/tools` 应见 `mcp__browser__browser_open` 等十四件工具,
`/agent doctor luban.browser:browser-reviewer` 的工具项转 ✓。

## mcp/browser-launcher:可选的兼容口

一件 code-bearing 组件,默认**不挂**——项目级 Package 里,代码组件先过
内容指纹信任门(审批页亮命令、参数与哈希),未信任时整包照常可发现,
唯独它不起。它是给四类场景的:终端版没挂 Desktop、第三方 MCP 客户端、
旧配置兼容、远程无界面运行。

它自己不是浏览器:启动时找核心分发的 `browser/server.js`(先看环境变量
`LUBANCODE_BROWSER_MCP`,再从包位置逐级往上找),找到就把 stdio 原样
接管,参数透传;找不到就 stderr 说明白、退出,不下载、不内置副本。
过了信任门,它的工具走包名空间 `mcp__luban.browser.browser-launcher__*`
——与本包 Agent/Workflow 声明的 `mcp__browser__*` 是两套名,LubanCode
本机用户优先走上一节的直配路,名字才对得上。

## 四只 workflow 的形状与失败路

| workflow | 形状 | 失败路 |
| --- | --- | --- |
| smoke | open → route(有无断言文本)→ wait_text/settle → snapshot → report → gate | 等不到关键文本/开不了页/快照失败:工具报错,run 直接 Failed,journal 指到节点;复核判 fail:走 review_fail 收口,判词在 result.verdict |
| responsive | open_desktop → open_mobile → settle → compare → gate | 缺 browser-mobile 配置:open_mobile 报 tool_unavailable,run Failed;布局毛病:layout_fail 收口,findings 在 result |
| console-error | open → settle → audit → gate | 开不了页:run Failed;有不容忍的新 error:errors_found 收口,新账在 result.new_errors |
| network-failure | open → settle → audit → gate | 开不了页:run Failed;关键请求失败或压根没发:failures_found 收口,账在 result.failures |

判词节点(agent)被要求只回一行 JSON;模型多嘴时整段退成文本,
gate 落到 default 判 fail——单子还在 result 里,人看得见,不静默。
journal 翻账在 agent 手里做:browser_console/browser_network 要
page_id,而 page_id 只在开页回执文本里,workflow 的 Store 抽不出字符串,
工具节点串不起来;reviewer 读得到回执,顺手就抠出来了。

拿练习页当靶子:

```bat
cd <本包>/assets/practice-page
python -m http.server 8901
```

```text
/workflow run luban.browser:smoke url=http://localhost:8901 expect_text=计数器
/workflow run luban.browser:console-error url=http://localhost:8901 tolerated=["练习靶"]
/workflow run luban.browser:network-failure url=http://localhost:8901 url_contains=/api/
```

练习页第三课埋了故意的 error、失败请求(fetch 打 127.0.0.1:9,稳败)与
720px 宽卡(手机口必溢出)——smoke 该过,console-error 与
network-failure 该翻案,responsive 该指出溢出。

## 排错

| 症状 | 缀 |
|---|---|
| `/package list` 不见本包 | 包根须有 package.yaml;放的是整只包目录,不是里头某层;`--package-dir` 指包的上一层 |
| doctor 报 error | 照诊断指的文件改;内容组件的错不修,整包 invalid |
| `/agents` 不见 browser-reviewer | 包 invalid 时一件不挂;先 `/package doctor` |
| `/agent doctor` 工具项全 ✗ | browser MCP 没配(或没起):config.json 配法见上,`node <仓库>/browser/server.js` 手跑看报错 |
| `/workflow validate` 报 unknown_tool | 同上,能力对账对着当前会话注册表;MCP 配好即消 |
| responsive 报 tool_unavailable | 没配 browser-mobile 服务,配上第二键 |
| 判词退成文本、全走 fail | 模型没守"只回一行 JSON"的规矩;看 result 里的单,重跑或换模型 |
| stale_ref 报个不停 | 正常保护:每次导航后重新 snapshot,别囤 ref |
| 双开报 profile 锁 | persistent profile 进程锁,一只会话一只浏览器;mobile 服用 ephemeral |

## 什么时候选 MCP 而不是 process plugin

翻隔壁 `examples/packages/gui-agent/`(桌面那路的对照)。一句话:状态
跨回合、要常驻连接的东西(浏览器/数据库/连接池/登录态)选 MCP;状态在
目标程序手里的桌面操作选 process plugin。
