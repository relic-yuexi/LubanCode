# Agent 扩展示例:GUI 与 Browser

两套能跑能改的案例,教你替 LubanCode 添特殊能力时怎样选路。先看表,
再往下读:

| 你的能力 | 首选 | 状态放哪儿 | 示例 |
| --- | --- | --- | --- |
| 截一张图、点一下、跑一条本地函数 | process 插件 | OS / 目标程序 | [gui-agent/](gui-agent/) |
| 一枚可信 Lua/C 函数 | Lua / native 插件 | 宿主进程 | [examples/plugins/](../plugins/) |
| 浏览器、数据库、连接池、登录态 | MCP | 长命服务 | browser-agent(另一单在建) |
| 只教模型一套步骤 | Skill | 无运行状态 | 仓内 `skills/` |

## 先破三个误会

1. **"工具每次只调用一次"≠"工具没有副作用"。** 点击一次也可能付款、
   删除、发信。写动作永远要确认,示例不教任何"允许全部"的捷径。
2. **"进程短命"≠"任务无状态"。** GUI Agent 的状态在桌面与目标程序;
   Browser Agent 的状态在 Browser MCP 会话里。插件与服务器只是手脚。
3. **"截图保存成功"≠"模型看见截图"。** 须经富工具结果或显式图片输入;
   当前协议 v1 只回文本,示例在 observation 里如实标注(见
   [gui-agent/README.md](gui-agent/README.md) 第 7 节)。

## 最短启动路径

**GUI(Windows,十分钟):** 见 [gui-agent/README.md](gui-agent/README.md)
第 2-3 节——拷目录、开 dry-run、起夹具、跑七步链。装法有两路:用户级
(`%USERPROFILE%\.lubancode\plugins\`)免信任门;装进仓库的项目级
(`<项目>\.lubancode\plugins\`)要过信任门,`/plugin trust <id>` 一条命令
批准(详见 gui-agent README 第 2 节"装到哪")。

**Browser(另单在建):** 常驻 MCP + Playwright,DOM/ref 路线,依赖
`todos/MCP富结果与专属浏览器.todo` 的底层能力。目录落地后此处补链接。

## 同一任务,两条路(占位)

两例共用同一份教学业务("填名字、选颜色、提交、复验"):GUI 走截图像素
与坐标,Browser 走 DOM snapshot 与 ref。等 Browser 半边落地,此处放同台
对照表(工具调用数、耗时、失败点、各自收住误点的方式),不评谁更高级,
只讲选择边界。

## 源码地图

```text
examples/agents/
  README.md            ← 本页:选路导航
  gui-agent/           ← 案例一:process 插件(Windows 桌面)
    plugin.json          manifest:九件工具的静态真账
    runner.py            协议 v1:stdin/stdout 各一份 JSON
    gui_actions.py       合同:坐标、stale、上限、dry-run、危险键闸
    gui_backend.py       Win32 ctypes:枚举/DPI/SendInput/截图 + FakeBackend
    png.py               零依赖 PNG 编码
    SKILL.md             教模型的操作纪律
    test_runner.py       离线自测(零真输入)
    fixtures/            教学夹具(tkinter 小窗)
    scripts/manual_e2e.py 真桌面 E2E(默认 SKIP)
  browser-agent/       ← 案例二:常驻 MCP(另单在建,勿在此占位造空壳)
```

## 改造地图(五步,两例通用)

1. 改工具名(manifest/MCP 工具定义)——模型看到的名字就是合同。
2. 改 schema——参数收窄,`additionalProperties: false` 别松。
3. 改实现——协议/合同/平台三层分开改,别挤成一坨。
4. 改权限——确认粒度、env allowlist、dry-run、危险动作闸。
5. 补测试——先离线(fake 后端),后真机(专用桌面),报告分开写。

## 平台与依赖(不藏账)

| 示例 | 平台 | 依赖 | 体积 |
| --- | --- | --- | --- |
| gui-agent | Windows 10/11(其余平台如实报 unsupported) | Python 3.10+,**零第三方包**(ctypes/zlib/tkinter 均自带) | 插件目录约 60KB 源码 |
| browser-agent | 按 Playwright 支持范围(另单定) | Node + MCP SDK + Playwright + browser binary(另单列版本) | — |
