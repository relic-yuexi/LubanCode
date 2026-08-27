# Browser Agent:常驻 MCP 驾驭 Playwright

案例二。与案例一(gui-agent)的差别在**状态放谁手里**:桌面程序自己
保存状态,GUI 插件每回起一只短命进程就够;浏览器要跨回合保住页签、
Cookie、登录态与下载,所以走 MCP——留一只长命 browser server,
工具调用都打到它。

## 1. 前置

- Node.js 18+(`node --version`)
- 仓库 `browser/` 目录(LubanCode 自带的 Browser MCP 本体,
  0.26.72 起)

装依赖(仓库根目录):

```bat
cd browser
npm install
npx playwright install chromium
:: 想用 WebKit 引擎再装:npx playwright install webkit
cd ..
```

> 本机 CDN 直连不通时给 npm/下载设代理(如 `set HTTPS_PROXY=http://127.0.0.1:10808`)。

## 2. 挂进 LubanCode

编辑 `%USERPROFILE%\.lubancode\config.json`,加一节 mcpServers
(路径按你仓库实际位置写绝对路径):

```json
"mcpServers": {
  "browser": {
    "command": "node",
    "args": ["D:\\lubancode\\browser\\server.js", "--engine", "chromium", "--headless"]
  }
}
```

去掉 `--headless` 则弹出真浏览器窗口(你自己能看见它在动)。

重启 LubanCode,`/tools` 应见 `mcp__browser__browser_click` 等
十一件工具。

## 3. 试一手

```text
你:打开 https://example.com 截个快照,告诉我页面上有什么可点的
模型:mcp__browser__browser_open → browser_snapshot →
      列出链接与按钮,各带 ref
```

练手用 `fixture/` 里的本地页(不碰真网站):

```bat
cd examples\agents\browser-agent\fixture
python -m http.server 8901
```

然后让模型"打开 http://localhost:8901 把计数器点三下再填表提交"。

## 4. 配套 Skill

`SKILL.md` 是给模型读的操作纪律:快照→ref→动作→复验的循环、
导航后 stale_ref 的正确反应、停手线(登录支付先复述、密码永不进
对话、连败两次停手)。把它拷进技能目录即生效:

```bat
xcopy /E /I examples\agents\browser-agent\SKILL.md %USERPROFILE%\.lubancode\skills\browser-agent\
```

## 5. 排错

| 症状 | 缀 |
|---|---|
| `/tools` 不见 browser 工具 | config.json 的 JSON 语法(node 会拒);`node browser/server.js` 手跑看报错 |
| `stale_ref` 报个不停 | 正常保护:每次导航后重新 snapshot,别囤 ref |
| 双开报 profile 锁 | persistent profile 进程锁,一只会话一只浏览器,别配两份同 profile |
| 杀进程后工具全报失效 | server 会明报旧 page 失效;`browser_status` 看 server 活没活,重启会话 |
| headless 下想看画面 | 去掉 `--headless`,或 `browser_screenshot` 拿图(多模态模型才能读) |

## 6. 什么时候选 MCP 而不是 process plugin

翻 `examples/agents/README.md` 的选择表。一句话:状态跨回合、
要常驻连接的东西(浏览器/数据库/连接池/登录态)选 MCP;
状态在目标程序手里的桌面操作选 process plugin。
