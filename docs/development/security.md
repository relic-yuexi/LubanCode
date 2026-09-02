# 安全模型

[文档首页](../README.md) · [配置手册](../reference/configuration.md) · [工具参考](../reference/tools.md) · [Hooks 手册](../features/extensions/hooks.md) · [扩展指南](../features/extensions/README.md)

LubanCode 能读写文件、起进程、访问网络、加载扩展，也能把材料发给模型服务。安全不能只靠一句“会询问”。这页把信任边界、权限与本地数据分开讲。

## 1. 先认清边界

`default / accept_edits / yolo / auto / dont_ask` 是审批策略，不是操作系统沙箱。旧值 `confirm` 只作为 `default` 的弃用兼容别名。它们决定某项动作自动执行、询问还是直接拒绝，不限制进程已经拥有的文件、网络或账户权限。

同理：

- worktree 隔离 Git 工作目录，不隔离操作系统权限。
- 子代理隔离对话历史，不隔离用户账户权限。
- MCP 隔着子进程，不等于远端服务可信。
- PTC runner 有资源与 RPC 围栏，不是对抗恶意代码的强化沙箱。
- Lua 与 C ABI 插件在宿主进程内，能直接拖垮主程序。

## 2. 要保护什么

- 源码、未提交改动与构建产物。
- API key、cookie、SSH 凭据与环境变量。
- 会话、项目记忆、提示词和工具输出。
- 终端输入中尚未发送的草稿。
- 本机进程、网络、剪贴板与用户目录。
- 远端 Provider、MCP、Hook 或插件能看到的内容。

## 3. 信任区

| 来源 | 默认看法 | 主要风险 |
| --- | --- | --- |
| LubanCode 主程序与内置资源 | 随发行包信任 | 代码缺陷、供应链 |
| 用户全局配置 | 用户本人授权 | 明文密钥、危险 Hook |
| 项目受版本控制配置 | 未必可信 | 仓库诱导执行、开外接工具 |
| `settings.local.json` | 本机用户决策 | 过宽 allow 规则 |
| AGENTS/Skill/Prompt | 指令材料 | prompt injection、诱导调用工具 |
| MCP/Hook/外部命令 | 独立进程或服务 | 任意代码、数据外传 |
| Lua/C ABI 插件 | 宿主进程内 | 崩溃、内存破坏、任意权限 |
| 模型 Provider | 外部服务 | 数据留存、日志、供应商行为 |

“在仓库里”不等于可信。克隆陌生项目后，先看 `.lubancode/`、AGENTS、Hook、Skill 与插件引用。

## 4. 确认档

| 档位 | 行为 | 适合 |
| --- | --- | --- |
| `default`（默认模式） | 需确认的动作询问；已明确放行或本来无需确认的动作照常执行 | 陌生仓库、审查期 |
| `accept_edits`（接受编辑） | 自动放行普通文件编辑；撤销/破坏性文件动作、命令和外接工具仍问 | 集中修改文件时 |
| `yolo`（YOLO）/ `--yes` | 显式全放 | 隔离环境、已审任务、自动化 |
| `auto`（自动模式） | 自动放行普通文件编辑和内置判定为安全的命令；危险/未知命令与外接工具仍问 | 熟悉项目的日常工作 |
| `dont_ask`（不询问） | 本来无需确认或已明确放行的照常执行；凡是还需要问的动作直接拒绝，不弹审批 | 无人值守且不能冒险时 |

`dont_ask` 是“会问则拒”，绝不是 YOLO。Plan 能力边界和 PreToolUse Hook 的 `deny` 是更早的硬闸，五档包括 YOLO 都越不过。`Hook ask` 仍进入审批裁定：在 `dont_ask` 下结果是拒绝。

项目 deny 规则不应伪装成操作系统强制策略。**已发布兼容例外：用户显式选择 `yolo` 或 `--yes` 时，`deny_commands` 不拦。** 文档与 UI 必须如实说“全放”；若要真正隔离，靠容器、低权限账户、虚拟机或 OS sandbox。

## 5. 文件与命令工具

- `write_file`、`edit_file` 接受相对路径，也接受绝对路径。普通会话没有“只能写工作区”的硬沙箱。
- worktree 隔离态会拦住写回主 checkout；这道闸不等于通用路径沙箱，主树之外的路径仍可能触达。
- 写入前先看路径与 diff，再按确认策略执行。真要圈住目录，须另套容器、低权限账户或 OS sandbox。
- `run_command` 的 shell 字符串按当前平台解释。模型生成的引号、重定向、管道与变量都可能有副作用。
- 超时要收进程树，不只杀父进程。
- 后台命令日志可能含密钥与业务数据；路径只给当前用户，清理策略要明确。
- 同一条命令再次执行仍是新副作用，不能因历史去重而跳过。

`auto` 下的 allow/deny 前缀只是策略匹配。不要拿字符串前缀当完整 shell 安全分析。

## 6. 鉴权与密钥

Provider 鉴权用三态：

- `none`：明确无需鉴权，不发 Authorization。
- `env`：从 `key_env` 指定的环境变量取。
- `inline`：明文 `api_key` 落配置。

优先用 `env`。不得凭 localhost、HTTP 地址或“变量暂时没设”猜成 `none`。列表、诊断、日志与错误须打码；原始 key 不进截图、TODO、session、memory、Skill 与测试 fixture。

搜索服务、MCP 环境与 Hook 子进程也可能拿环境变量。只传所需变量，不把整套凭据随手灌给第三方程序。

Lua 插件的 Secret 另走一条路：manifest 声明逻辑 id 与 env 名，宿主解析并代填请求头，Lua 与模型都摸不到明文（见 9.1 节）。

## 7. 项目配置

项目 `.lubancode/config.json` 可压过部分全局字段。几道硬闸：

- 项目不能把用户全局关闭的长期记忆自行打开。
- 项目 Hook 首次运行前须做信任审查；定义变了重新审。
- 本地权限决策放 `.lubancode/settings.local.json`，不应提交。
- 项目不能靠配置偷偷把用户学习档从 `review` 抬到 `auto`。

合并逻辑须保留字段来源。诊断时能说出“这个值从哪来”，才查得清恶意或误配覆盖。

## 8. Hooks

Hook 会执行程序，风险等同本地脚本。优先 exec form 的 `command + args`，少走 shell 字符串。输入走 stdin JSON，日志写 stderr；stdout 只回协议 JSON。

实现须守：

- 事件、matcher、超时与 failure policy 明确。
- 项目来源审查按定义哈希，命令变化后旧批准失效。
- `PermissionRequest` 等决策事件按最严格结果归并。
- Hook 失败不能伪装成工具成功。
- legacy 环境变量协议只作兼容，不承接新能力。

详见 [Hooks 手册](../features/extensions/hooks.md)。

## 9. 扩展

| 扩展 | 边界 | 建议 |
| --- | --- | --- |
| Skill | 提示与文件材料 | 读完 `SKILL.md` 和脚本再用 |
| MCP | 独立子进程/服务 | 限环境变量，审工具 schema 与服务来源 |
| LSP | 独立语言服务器 | 固定命令与版本，留意它会读项目文件 |
| Lua（裸 `.lua`） | 宿主进程内，纯计算无 Host API | 只装可信源码；pure 画像三道软墙拦跑野脚本，不是沙箱 |
| Lua（manifest v2） | 宿主进程内，网络与 Secret 由宿主代管 | 只批精确网络账；`.env` 放数据目录，别放源码树 |
| C ABI | 宿主进程内 | 固定 ABI、架构、依赖与来源 |

不可信原生能力优先改走 MCP，让崩溃与内存边界留在子进程。进程隔离仍不限制文件和网络；需要时再套 OS sandbox。

### 9.1 manifest v2 Lua：Secret 与受控 HTTP

联网的 Lua 插件必须带 `plugin.json`（`manifest_version: 2`）。一句话：Lua 只描述请求，宿主握住水管和钥匙。裸 `.lua` 里不存在 `luban` 模块，结构性碰不到网络与 Secret。

**Secret 来源与优先级。** 同一逻辑 Secret 按序取第一份非空值：

```text
宿主进程环境中 manifest 声明的 env 名
-> 插件专属 .env
-> 未找到
```

第一期不接 OS Keychain；接口留了 provider seam，日后可加 Windows Credential Manager、macOS Keychain、Linux Secret Service，Lua 合同不变。

**`.env` 住数据目录。** 源码树里的 `.env` 永不自动读取——它会进插件/Package 内容指纹，改 Key 就撞掉信任，打包分享还容易捎走 Secret：

```text
standalone 插件：~/.lubancode/plugin-data/<plugin-id>/.env
packaged 插件：  ~/.lubancode/package-data/<package-id>/plugins/<local-id>/.env
```

Windows 的 `~` 取 `%USERPROFILE%`，POSIX 取 `$HOME`；路径由宿主算，不交 Lua 猜。文件走窄语法（UTF-8 可带 BOM、`KEY=value`、引号包值、`#` 注释；不做插值与命令替换，超 1 MiB 拒读），只装 manifest 声明过的键。`.env` 改动不改代码指纹，下一次调用读到新值——轮换 Key 无须重启。

**五道网络边界。** 发请求前依次落闸，任一不过即拒发：

1. URL 完整解析；禁 fragment 与 userinfo。
2. scheme/host/port/method 精确命中 manifest 声明（只收 `https` + 精确 DNS host + 443 + `GET`/`POST`；通配符、IP 字面量、私网地址、任意端口不收）。
3. host 不是 IP 字面量、`localhost` 或 `.local`。
4. DNS 候选逐枚分类：loopback、link-local、RFC1918、CGNAT、组播、保留段、云 metadata 段，混一枚整体否决。
5. 连接期钉住已验地址，防 DNS rebinding。

**重定向一概不跟。** 3xx 原样作为 HTTP 响应交 Lua。顺带一记：cpr 的 Session 默认跟随重定向（`Redirect::follow` 缺省 true），插件 HTTP 传输已用 `cpr::Redirect(false)` 显式关死——不关的话，3xx 会静默跟还拿真 DNS 解析 Location，绕过钉地址。provider SSE 路同一患，尚未迁移，已记账留后（0.26.122）。

**日志打码。** 错误、日志、trace、inspect、doctor 只记 Secret 逻辑 id、来源类别与 available/missing，不写值、长度、前后缀与 fingerprint。宿主侧 `SecretValue` 禁复制、移动即覆写、析构 best-effort 清 buffer；上游错误体若回显 Secret，落日志与模型结果前替换 `[REDACTED]`。best-effort 覆写不是绝对保密证明——优化器、TLS 库与系统缓冲仍可能留副本。Secret 明文从未进入 Lua、工具入参、模型上下文、session、trace 与 Hook payload；Lua 拿到的只是不透明引用（`tostring` 只得 `<secret:id>`）。

字节帽（URL/请求头/请求体/响应头/响应体/墙钟）全部在数据入口处落锤；manifest `limits` 只许下调宿主硬上限。选型与写法见[扩展指南第 5.3 节](../features/extensions/README.md)。

## 10. 子代理与跨会话

子代理有独立 history 和工具表，可共享项目指令、cwd 与模型后端。它不是低权限 worker：

- 主代理能做的文件/命令动作，子代理通常也能做，仍走确认与 Hook。
- 子代理 prompt 必须自包含；不要暗带主会话敏感全文。
- 定向消息须按 task id 投递，终态任务不得悄悄改投 main。
- 子代理结论回 main 时，要标来源与终态，不能把失败冒充完成。
- 跨会话 peer 消息是外来输入，应按 prompt injection 看待。

## 11. 会话、记忆与导出

- session JSONL 会存用户消息、助手回答、工具事件、usage 与 compact 事件。
- `/export` 会把会话写成可读 Markdown；导出前审一遍敏感内容。
- 项目记忆住用户目录，只进本轮请求视图，不进 session，也不随 export 外带。
- 记忆候选须过长度、敏感内容、证据与学习档闸门。
- 日志、PID、临时端口、原始网页、密钥与个人数据不该进长期记忆。
- 删除/forget 要说明是归档、拒收还是物理清除，不能含糊。

## 12. PTC

PTC 允许模型写受限 Python，再经 RPC 调宿主白名单工具。它能减少重复工具往返，也多了一条代码执行面。

必须守：

- 只暴露画像允许的工具与字段。
- 宿主仍做 schema、权限、Hook、确认、取消与输出上限。
- runner 限时间、内存、输出、RPC 次数与源码大小。
- 不把 API key 和全量环境交给 runner。
- 自动启用须有能力画像、熔断与回退。
- 文档明写它不是强化沙箱。

详见 [PTC 手册](../features/tools/ptc.md)。

## 13. 网络

- Provider 请求会发送 system prompt、相关 history、工具 schema 与本轮材料。
- `web_fetch`/`web_search` 会把 URL 或查询发给外部服务。
- MCP 可自带网络能力，LubanCode 无法替它证明去向。
- 插件 HTTP 只走 manifest 声明的精确 HTTPS 目的地，五道边界与字节帽由宿主落闸；重定向不跟（见 9.1 节）。
- 诊断端点如 `metrics_url` 只在用户显式配置后访问，不从公网 base URL 擅自猜。
- HTTP 重定向、代理与自定义 header 都可能改变数据去向，配置时要审。

## 14. 安全审查清单

- [ ] 新能力读、写、执行、联网、落盘分别有哪些？
- [ ] 来源是用户级、项目级、模型、外部服务还是插件？
- [ ] 默认关闭还是默认开启？项目能否越过用户全局闸？
- [ ] default/accept_edits/yolo/auto/dont_ask 五档各怎样走？
- [ ] 失败、取消、超时后有没有半写与孤儿进程？
- [ ] 日志、session、memory、截图是否会漏秘密？
- [ ] 子代理与 PTC 是否复用同一权限链？
- [ ] 测试是否包含拒绝、逃逸、坏输入与并发？
- [ ] 文档有没有把“确认策略”误写成“沙箱”？

## 15. 已知边界

- 仓库尚无公开的强化 sandbox 方案。
- Lua/C ABI 插件没有进程隔离；Lua 三道软墙（pure 画像、指令预算、内存帽）拦跑野脚本，不拦恶意绕洞。
- 插件 Secret 第一期不接 OS Keychain，只收环境变量与数据目录 `.env`。
- cpr 默认跟随重定向：插件 HTTP 路已显式关死，provider SSE 路同患未迁（0.26.122 记账留后）。
- MCP/LSP 子进程仍以当前用户权限运行。
- 模型与外部服务的数据保留由各服务条款决定。
- 终端打码不能追回已经写入外部日志或 session 的秘密。

发现敏感漏洞时，不要把真实 key、私人仓库内容或可直接利用的凭据贴进公开 issue。先撤销受影响凭据，保留最小复现，再用项目维护者提供的私下渠道报告；仓库若尚未公布安全联系渠道，应先补一份明确政策。
