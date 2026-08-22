# 更新记录

这里只记用户看得见的变化。每个版本留三条，细处可点版本标题查看提交差异。

## [v0.26.35] - 2026-08-23

- **一轮回答收成一册账。** 回合结尾落一道带字的分界线 `──── Worked for 6m 41s ────`，打断写 `Stopped after`、失败写 `Failed after`；错误与预算耗尽不再从中途裸退漏掉尾线，管道模式也落纯文案版（时间账属于 automation 契约）。
- **工具看得出批次了。** 同一次模型响应吐的多枚工具先整批登记"本拍排队中"，再逐枚点亮执行；ESC 后跑完的照旧、跑着的记打断、没轮到的如实标"未执行"，屏上不缺枚。五行 PowerShell 只画一个条目，标题带 `+4 lines` 不再横铺。
- **Working 计的是整轮了。** 活动条认 turn 不认单次请求：正文流、工具批次、下一次模型请求都不熄、秒数不归零；ESC 后换"正在停…"，终态落账才退场。Ctrl+E/Esc 返回不再追打横幅，token 长行退到详细态（Ctrl+O），Ctrl+L 重放与实时画面同一颗渲染器。

## [v0.26.34] - 2026-08-23

- **无界面后台接口补齐了血肉。** 工具条目带中立 diff 行表、回合用量与上下文压力实时通报、图片输入入协议;出站队列的增量合并落了真(delta 并条、溢出通报带账);jsonrpc 字段去留、事件序号、图片字段名一并冻结成文。
- **后台也能管会话了。** thread/archive、unarchive、delete 三法接通统一收口,开着的热线程拒绝动;thread/list 与终端同吃一碗结构化摘要,/workflow 的运行快照与增量事件也从同一扇门出来。
- **SSH 承载有了实跑口径。** 本机管道冒烟六项全过(握手/坏报文/协议纯度/断线退场/无孤儿);真 SSH 的手测口径与完整协议文档落 docs,三平台方法面一览无余。

## [v0.26.33] - 2026-08-23

- **Workflow 并行 map 的产物不再串位。** 并发分支的产物落位改预分配槽、各写各的下标,收拢后按 items 顺序拼装;修掉了共键覆写导致的乱序取值(libc++ 时序下现形的竞态,TSan 已清)。
- **中文名会话在 Windows 上删得掉了。** 会话搬删的档名拼装改走 UTF-8 通道,不再被 ANSI 代码页解成乱码名——此前中文档在 Windows 上删除/归档一直 NotFound,是真产品 bug;相关测试夹具一并改掉「写找同错相抵」的侥幸,柄收口回归补齐。

## [v0.26.32] - 2026-08-23

- **显示与业务分家(地基)。** 引擎按 engine/runtime/terminal 拆开编译：工具改动的行级 diff 有了中立行表，Web 与终端吃同一份数据；事件流合同落地，终端与 JSON 两只事件出口同流同账，后续 Web/Tauri 接同一颗运行时，不再复制终端逻辑。
- **会话内核可脱离终端单测。** 会话存档、权限与发号搬进 SessionRuntime；`/model`、`/resume` 与审批回答有了类型化接口，远程前端不再伪造 slash 字符串；交互会话类更名终端控制器，只管画面接线。
- **引擎日志不再裸写标准流。** 模型端与钩子的诊断改投统一日志出口，app-server 的 stdout 从此只剩协议字节；依赖边界进了测试账，引擎层混入终端件直接编不过测试。

## [v0.26.31] - 2026-08-23

- **本地插件一站齐活。** lubancode plugin init python 一键生成插件三件套;Lua 插件进统一台账并加三道软墙(pure 档缺省关 io/os/package、指令预算防死循环、内存帽);原生插件三平台一个形状(Windows/Linux/macOS 真编真载),ABI 升 v2(版本协商/shutdown/宿主分配器)且兼容读 v1。
- **插件跑在最小环境里。** 进程插件整环境替换:只递 PATH 与声明的几个变量,宿主的密钥一概到不了插件;项目级 .lubancode/plugins/ 要过内容指纹信任门,改一字节就得重新批;/plugin inspect|doctor|test 一套管理面看得见状态。
- **常驻进程有据不做。** 冷启动实测 Python 全协议往返 14~50ms,占整次调用 1%~4%——重依赖该修插件设计,不是宿主先造握手心跳,数字与依据落进架构文档,哪天变了再立单。

## [v0.26.30] - 2026-08-23

- **办事章法能存成一张图了。** 新增 Workflow:把一套流程说给 LubanCode,追问缺口、生成有类型的执行图(先校验、预览、确认再装进 .lubancode/workflows/),往后 /<别名> <参数> 照图开工;图有 ASCII 与 Mermaid 两种画法,项目级遮用户级、撞名别名禁直呼。
- **引擎一条龙:并行汇合、重试回落、断点恢复。** 五种 join 策略(all/all_settled/any/quorum/race)、map/reduce、取消与步数·时限·token 三道预算;审批与补问走 Broker 悬起,journal 落盘带脱敏,断点续跑已成功的节点不重跑;副作用节点没幂等键不许重试,secret 明文直接拒载。
- **动态有缰绳。** 规划器只许用图里标注 template: 的积木补图,补丁 append-only、悬边越权全拒、动到既有节点或新增交互要再问一道;运行态出结构化快照与增量事件,终端与将来的 Web/Tauri/app-server 同吃一碗饭。

## [v0.26.29] - 2026-08-23

- **会话台账三副眼镜。** /resume 台账里 Ctrl+T 看整场转录(大文件只取头尾,收起回原行)、Ctrl+E 摊开标题/目录/id/模型/消息数与更新时间、Ctrl+O 紧凑舒展两种画法;浏览全程不动盘。
- **归档先行,删除另开明路。** 新增 lubancode archive/unarchive/delete 顶层命令与会话内 /archive、/delete:归档字节原样搬进 sessions/archive/ 且默认列表不再打扰;永久删除要过确认屏(标题+完整 id+目录,缺省取消),标题重名列短 id 绝不猜,路径越界一律拒;回合在跑、后台子代理在忙都拒绝动手。
- **搬删只此一家。** SessionLifecycle 统一收口,终端 slash、顶层命令与 app-server 同吃一碗结构化账(thread.list 结构化摘要、archive/delete 走 typed 命令),代码里搜不到第二条私搬私删路径;Windows 开句柄先收柄再动文件的回归有测试钉着。

## [v0.26.28] - 2026-08-23

- **工具文案全量双语收官。** 剩余九件工具(网页搜索/抓取、tool_search、skill、会话列举与跨会话传话、worktree、项目记忆、PTC)的描述与参数说明全部迁入语言分档;英文会话拿到全英文工具表,缺省中文与旧版逐字节一致,`LUBANCODE_LANG` 切换即时生效。
- **文案一致性进了测试账。** 语言驱动器扩到 100 节中英双语逐字节比对;工具源码不再容得下游离中文描述,grep 检查与负例钉进回归,后续新工具照此门进。

## [v0.26.27] - 2026-08-23

- **无界面后台接口接通。** `codex app-server` 同类能力落到 LubanCode：JSON-RPC stdio、thread/turn 生命周期、流式事件、审批反向请求、取消与硬超时已经接上；终端与后台接口共用运行时合同，不再各养一套 Agent 逻辑。
- **会话与终端更像一张清楚的账。** 裸敲 `/resume` 进入可搜索、筛选、排序的全屏会话台账；排队消息可随存档恢复；工具文案按语言分档，运行条目、ID 与状态刷新也收回统一口径。
- **插件、文档与三平台一道收口。** Process Plugin v1 冻结 manifest/stdio 合同，Lua 调用与主/子代理挂载补并发和去重；官方 docs 与 Skill 同包安装，文档按功能、参考、架构、开发分层；Windows、Linux、macOS 的文件时钟与安装路径差异补齐。

## [v0.26.0] - 2026-08-15

- **终端交互大修。** 子代理面板移到输入框上方：任务全量列示可选，`x` 停止/清除，`Ctrl+X Ctrl+K` 两段确认停全部，Enter 进查看态后可直接给那只代理递话；排队消息在工具边界按序送达，`Esc` 打断并立即送，`Shift+←` 取回编辑；流式打 `/` 出补全，context 状态栏回合内实时刷新（`~` 标旧值），流式 Shift+Tab 切档，思考可 Ctrl+O 展开，ask_user 不再被子代理状态遮挡。
- **模型侧 worktree 全套。** 新增 `worktree` 工具（enter/status/list/exit），大改动住进隔离分支，写主树/命令 cwd/git 改道三道闸拦截；子代理可带 `isolation: worktree` 各住各的隔离房，干净自动清、有活留房；`/resume` 验明正身后回搬住房会话；`.worktreeinclude` 把 `.env` 一类带进新房。
- **更结实也更清爽。** 工具输出含非法 UTF-8 不再杀整场会话（清洗前移到公共信任边界，`read_file` 对编码明规矩）；后台子代理完成后结果自动回流，空闲会话不再冻死；main.cpp 从六千余行拆到 39 行，app 层分件立编译边界，改一条命令不再重编整个程序；GLM-5.3 进厂家目录。

## [v0.25.1] - 2026-08-14

- **录一遍，生成技能。** `/record` 开录后在 LubanCode 里做一遍活计，工具调用、备注、验收一路记下；停录起草 SKILL.md，预览点头后装进项目级或主目录级，本场立刻可用。入参密钥全程打码，必须明着开录，状态栏常挂 REC 标记。
- **排队输入不再拧成一股。** 输入行只画你正打的字；落队后正文挪到输入框上方逐条摆，空输入按上键取回改写，Delete 删、Esc 放回；「Esc 打断」挪进状态行。
- **同机跨会话能递话。** `/peers` 列出本机其它会话，`/send` 送一张字条；收件只挑轮次边界，不掐正在跑的工具，来信不能批工具、不能改配置，slash 只算文字。Windows 走具名管道，Linux 走 Unix socket，均限当前用户。

## [v0.25.0] - 2026-08-14

- **思考与调度看得见。** 模型的 thinking/reasoning 过程实时上屏；异步子代理带常驻面板与 Explore 模式；后台命令有任务台账、完成通知、输出查询与停止。
- **选择全用方向键。** 工具确认、配置向导、slash 补全改为方向键选择，Enter 确认；Ctrl+O 原地展开最近一条转录。
- **长输入更稳更顺。** 正文与输入框软换行；终端重画按帧差只描变动行，长会话不卡不画偏；输入法整词提交与快打连击不再丢字迟到；安装后 Windows PATH 即时生效。

## [v0.24.1] - 2026-08-06

- **模型接入更全。** 新增 Chat Completions 协议与内置 Provider 目录；可从常见厂家预设中添加服务，也能在会话里更新目录、切换 Provider 与模型。
- **项目知识能接着用。** 新增默认关闭的项目记忆，可召回稳定事实与偏好；本地 Skill 支持安装和管理，会话恢复、续聊与导出也补齐了交互细节。
- **终端操作更稳。** 状态栏可配置，彩色 diff 能按语言着色，大段粘贴、待办更新与工具条目重画更可靠；LaTeX 可铺成分式、根式、上下限与矩阵；模型列表会沿用当前项，也可按 Esc 取消；切到详细模式时，已经执行过的工具会立即展开。

## [v0.23.5] - 2026-07-24

- **项目规矩自动生效。** 新增 `/init`，可生成并载入 `AGENTS.md`；指令从 Git 根逐层合并，主代理与子代理一同遵守。
- **粘贴长文不再误触。** 多行内容会折成一枚占位，仍可前后编辑；按下 Enter 后再展开原文交给模型。
- **长回答更好读。** Markdown 改为分段增量渲染，标题、列表、表格与代码块边生成边收束，滚屏后也不易露出原始标记。

## [v0.23.4] - 2026-07-23

- **单选不用再敲编号。** `ask_user` 改为方向键菜单，用 `↑`、`↓` 移动，按 Enter 确认。
- **多选可以逐项勾选。** 按空格选中或取消，至少选中一项后再提交。
- **取消与自由填写仍在。** Esc 可随时退出，移到“自己填写”便能输入自定义答案；管道等非交互场景仍保留编号输入。

## [v0.23.3] - 2026-07-23

- **工具状态原位更新。** 黄色运行条会在完成后直接变成绿色或红色终态，不再把开始与结束各留一行。
- **输入框及时回来。** 工具条目写完便重建底栏与输入框，不必等下一段模型正文才恢复。
- **滚屏时少花屏。** 工具条目、子代理输出与底栏共用一套重画事务，长输出下的错位、重复与覆盖一并收紧。

## [v0.23.2] - 2026-07-23

- **Working 不再挡住输入框。** 思考状态与输入框同屏摆放，模型尚未吐出正文时也能继续键入。
- **工具续轮仍可排队。** 一件工具执行完、模型再次思考时，光标仍停在输入区，下一条消息照常进入队列。
- **发包链更稳。** GitHub Release 上传组件升到新版本，三平台安装包仍由标签流水线自动生成。

## [v0.23.1] - 2026-07-23

- **Provider 会记住。** `/provider switch` 成功后保存当前选择，下次启动仍走同一家服务。
- **切换后立即换屏。** Provider、模型与推理档位更新后，横幅和状态会按新配置重画，不再留下旧信息。
- **交互工具更完整。** 新增 `ask_user` 单选、多选与自由填写；执行期间常驻输入框和消息队列，彩色 diff 收起后也不再留下背景色块。

## [v0.23.0] - 2026-07-22

- **编程代理主流程齐备。** 支持读写与搜索文件、容错编辑、前后台命令、diff 确认、子代理、消息排队及 Esc 打断。
- **模型与扩展接成一体。** 接入 Anthropic Messages 与 OpenAI Responses，并提供上下文压缩、会话恢复、MCP、LSP、Skills、Lua、C ABI 插件和联网工具。
- **三平台可以直接安装。** Windows、Linux 与 macOS 均有自动构建的发行包和安装脚本，CI 分别用 MSVC、GCC 与 Clang 编译测试。

[v0.26.35]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.34...v0.26.35
[v0.26.34]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.33...v0.26.34
[v0.26.33]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.32...v0.26.33
[v0.26.32]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.31...v0.26.32
[v0.26.31]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.30...v0.26.31
[v0.26.30]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.29...v0.26.30
[v0.26.29]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.28...v0.26.29
[v0.26.28]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.27...v0.26.28
[v0.26.27]: https://github.com/relic-yuexi/LubanCode/compare/v0.26.0...v0.26.27
[v0.26.0]: https://github.com/relic-yuexi/LubanCode/compare/v0.25.1...v0.26.0
[v0.25.1]: https://github.com/relic-yuexi/LubanCode/compare/v0.25.0...v0.25.1
[v0.25.0]: https://github.com/relic-yuexi/LubanCode/compare/v0.24.1...v0.25.0
[v0.24.1]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.5...v0.24.1
[v0.23.5]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.4...v0.23.5
[v0.23.4]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.3...v0.23.4
[v0.23.3]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.2...v0.23.3
[v0.23.2]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.1...v0.23.2
[v0.23.1]: https://github.com/relic-yuexi/LubanCode/compare/v0.23.0...v0.23.1
[v0.23.0]: https://github.com/relic-yuexi/LubanCode/releases/tag/v0.23.0
