# Browser Agent(luban.browser-agent 0.1.0)

内容包(content-only)。箱里两样东西:一枚 Skill 教模型驾驭 browser MCP,
一张练习页供本地演练。零代码组件——放进目录即被发现,校验过了即登记,
全程不过信任门。

## 装了什么

| 目录 | 组件 | canonical id | 干什么 |
| --- | --- | --- | --- |
| `skills/browser-agent/` | Skill | `luban.browser-agent:browser-agent` | 操作纪律:快照→找 ref→动→复验;导航后 stale_ref 的正确反应;停手线 |
| `assets/practice-page/` | 资料 | —(不自动加载) | 零依赖练习页,本地起个 http 服务即可练手 |

浏览器本体不在此包,也永远不该进任何一只包。页签、Cookie、登录态、下载
归 BrowserRuntime——LubanCode 核心产品层的一部分,代码在仓库自带的
`browser/` 目录(session 层即 Runtime 本体,MCP server 是它的适配入口,
边界见 `docs/reference/browser-runtime.md`)。眼下宿主还没长出内嵌
Browser Tab,经 MCP 挂进来是现阶段的唯一入口;往后桌面端直连 Runtime,
MCP 退居 CLI 与第三方客户端的兼容口。包只管"教模型怎么用",不管
"跑浏览器":这就是 content-only 的边界,也是 Skill 与 Runtime 的分工。

## 安装

把整只 `browser-agent/` 目录(不是里头某层)放进:

```text
~/.lubancode/packages/            个人,全项目可见
<项目>/.lubancode/packages/       随 git 分发,团队人人 clone 即得
```

重启 LubanCode 后:

```text
/package list                        → 应见 luban.browser-agent 0.1.0,valid,skills:1,无 [code-bearing]
/package show luban.browser-agent    → 全账:组件、文件数、内容哈希、来源
/package doctor luban.browser-agent  → 静态诊断,应无 error
```

开发调试不必拷贝:

```text
lubancode --package-dir <本包所在目录>
```

`--package-dir` 是开发层(dev),优先级最高,直接指向源目录。

## 挂 browser MCP(一次性,兼容入口)

Skill 教的是工具用法;工具本身来自 BrowserRuntime 的 MCP 适配入口。编辑
`~/.lubancode/config.json`,加一节 mcpServers(args 里的路径按你机器上
仓库的实际位置写成绝对路径):

```json
"mcpServers": {
  "browser": {
    "command": "node",
    "args": ["<仓库根>/browser/server.js", "--engine", "chromium", "--headless"]
  }
}
```

前置:Node.js 18+;在仓库 `browser/` 目录装依赖——`npm install` 加
`npx playwright install chromium`(本机 CDN 直连不通时先给 npm 设代理)。
去掉 `--headless` 则弹出真浏览器窗口,你自己能看见它在动。

重启 LubanCode,`/tools` 应见 `mcp__browser__browser_click` 等十二件工具。

## 试一手

先起练习页(不碰真网站):

```bat
cd <本包>/assets/practice-page
python -m http.server 8901
```

然后对 LubanCode 说:"打开 http://localhost:8901 把计数器点三下再填表提交"。
模型照 Skill 走 `browser_open → browser_snapshot → 点击/填表 → 复验`。
练习页埋了四道题:同名按钮的唯一定位、计数复验、表单填写与提交、
提交后 ref 是否失效。

## 排错

| 症状 | 缀 |
|---|---|
| `/package list` 不见本包 | 包根须有 package.yaml;放的是整只包目录,不是里头某层 |
| doctor 报 error | 照诊断里指的文件改;内容组件的错不修,整包 invalid |
| `/tools` 不见 browser 工具 | config.json 的 JSON 语法(node 会拒);`node <仓库>/browser/server.js` 手跑看报错 |
| `stale_ref` 报个不停 | 正常保护:每次导航后重新 snapshot,别囤 ref |
| 双开报 profile 锁 | persistent profile 进程锁,一只会话一只浏览器,别配两份同 profile |
| 杀进程后工具全报失效 | `browser_status` 看 server 活没活,重启会话 |
| headless 下想看画面 | 去掉 `--headless`,或 `browser_screenshot` 拿图(多模态模型才能读) |

## 什么时候选 MCP 而不是 process plugin

翻隔壁 `examples/packages/gui-agent/`(桌面那路的对照)。一句话:状态
跨回合、要常驻连接的东西(浏览器/数据库/连接池/登录态)选 MCP;状态在
目标程序手里的桌面操作选 process plugin。
