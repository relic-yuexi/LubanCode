# TUI 写屏入口清单(正文/footer/审批/Hook/异常/最终统计)

> 按代理状态投影单(`todos/TUI主子代理视图彻底隔离_单写者与按代理状态投影.todo`)
> §七 P0 的取证基建:把"谁能往终端上写字"全部列出来,注明调用线程与
> 锁顺序。P1(身份与状态分账)、P2(收拢写者、原子换页)与 P3(输入与
> 生命周期收口)均已落地,本清单随之改版:各入口标注 P3 后的归属。
> 清单以取证基线 `d20547e4` 为底、随各期落地同步更新。

## 锁的 vocabulary(P2 改版)

| 锁 | 住处 | 管什么 |
| --- | --- | --- |
| `StdoutWriteMutex()` | `cli/console_input.cpp` | 终端字节流互斥(锁内只写字节,不等输入) |
| 统一提交锁 `SessionUiDispatcher::commit_mutex()` | `app/session_ui_dispatcher.cpp`(P2,递归) | 一切落笔的核对位:调度命令逐枚执行、RunSync 就地执行、收口 chrome、确认菜单显示半边全握它 |
| 登记簿锁 `AgentViewRegistry::mutex_` | `app/agent_view_registry.hpp`(P1) | main 活回合账(视图账/修订号/水位)、查看页身份、帧令牌(view_epoch/layout_revision) |
| `ConsoleReadMutex()` | `platform/console.hpp` | 逐键输入权(composer 与监听线程错峰;与画屏路径只单向相交) |

**锁序铁律**:统一提交锁 → StdoutWriteMutex(渲染闭包内拿 stdout,无倒置);
统一提交锁 → 登记簿锁(事件应用);换页事务经 `RunUiSync` 持提交锁后经
钩子再拿 stdout。旧泵(`UiEventPump`,无调度器的单发/单测路)的公共
render_mutex 直通口已退役,泵内照旧全程握锁渲染。
`ConsoleReadMutex` 与 stdout 只在 DSR 光标查询处短暂相交(get/释放,限时)。

## P2 的调度骨架(谁怎么写字)

- **业务线程只提交**:`TerminalTurnSink` 装了 `SessionUiDispatcher` 后,
  流内与控制路事件一律 `PostEvent`(相邻同 item delta 队尾合并),产生
  事件的线程一个终端字节不写;旧 `DispatchInline` 的"画前排干"由队列
  FIFO 保序替代。停表(StopUiPump)后迟到的 Emit 退化就地(旧泵同款)。
- **同步口 `RunSync`/`RunUiSync`**:换页事务、插行、footer 重画、确认
  菜单的显示半边、空闲 composer 整帧(P3 收编)——先排干队列余量、再
  在统一提交锁内就地执行(调用线程即执行线程,零跨线程等锁)。
- **异步口 `PostAction`/`PostUiCommand`(P3 收编)**:不需要"回来时已画
  完"的周期性屏面动作(footer 心跳)打包成命令投进队列,由消费线程在
  统一提交锁内执行;投递侧带丢拍闸(上一拍未消费则丢弃本拍),慢终端
  不囤积。槽未接(单发/单测)就地直走。
- **FrameToken = {AgentViewKey, view_epoch, layout_revision}**:布局在
  锁外算好,写屏前在提交锁内 `TokenCurrent` 核对;resize/Ctrl+L/Ctrl+O
  经 `NotifyLayoutInvalidated` 翻版本,旧布局帧不通过。
- **P3 后无过渡批**:P2 留下的四名直笔 writer(心跳/空闲整帧/ask_user
  菜单/TermErr 旁路)已全部收编或归区,清单见下各表。

## 一、正文/思考/工具条目(回合内)

| 入口 | 调用线程 | P3 归属 |
| --- | --- | --- |
| `TerminalTurnSink::DrawEvent` | 调度消费线程(P2:流内+控制路统一)或旧泵消费/就地路(无调度器) | 提交锁内执行;绘制闸按登记簿当前页+水位(锁内核对) |
| `StreamBodyTracker::OnDelta/OnBlockBreak/FinalizeRepaint` | 同上 | 同上;换页事务内锚点账作废(`SetViewSwitchInvalidateHook`) |
| `ToolDisplay` 各 On* | 同上 | 同上;锚点账同款换页作废 |
| `TurnInputListener` 插行([已排队]/[已打断]/粘贴截断/面板回执/拒绝行) | 监听线程 | **已收编**:整段 `RunUiSync` 提交(排干+提交锁内落笔) |
| 换页/查看帧重铺 `print_view_frame`(忙路+空闲两份) | 监听线程 / 主线程(composer) | **已收编**:整段 `RunUiSync`;五步事务(核目标→存草稿/纪元+快照→作废旧锚点/diff 账→擦净铺新→水位接续);跨页坐标不复用 |
| `TranscriptUiController::PrintViewedTranscript` | 上述钩子内(会话装配) | 随换页事务在提交锁内;main 页活回合成对协议不变 |

## 二、footer/底栏

| 入口 | 调用线程 | P3 归属 |
| --- | --- | --- |
| 流式 footer 画/擦(`RedrawStreamFooterLocked/EraseStreamFooterLocked`) | 调度消费线程、监听线程(`RunUiSync`) | 提交锁内执行;挂起计数闸着菜单期间零输出 |
| footer 心跳 `StreamFooterHeartbeat` | 独立心跳线程(只活 Run() 一段) | **P3 已收编**:每拍的屏面动作(`RunTick`)经 `PostUiCommand` 投进调度队列,消费线程在统一提交锁内执行;丢拍闸防囤积;槽未接就地直走(单发/单测旧貌) |
| `BeginStreamFooter/EndStreamFooter` | RunTurn 线程(回合起收) | 收口侧在 finish 提交锁内 |
| 空闲 composer 整帧 | 主线程(ReadLineKeyByKey 的 100ms 拍) | **P3 已收编**:`redraw_with_panel`/`reanchor_prompt_and_redraw`/`retire_idle_chrome` 整段经 `RunUiSync`(排干+统一提交锁内落笔);Ctrl+L/resize 走 `rebuild_screen`:布局翻版+`RunUiSync` |
| 收口 chrome(PrintTurnFooter/统计行/尾分界线/FinalizeRepaint) | RunTurn 线程(StopUiPump 之后) | 持统一提交锁(`CommitMutex`)贯穿收口;按页可见性让路 |
| 全局通知区(`SessionGlobalNotices`) | 任意线程 Push,chrome 组帧现拉 | **P3 新增**:无法归属页面的诊断进底栏帧顶的显式通知区(有界+过期自收),不落正文;非交互走原输出合同 |

## 三、审批/交互菜单

| 入口 | 调用线程 | P3 归属 |
| --- | --- | --- |
| 工具确认(async 主路 `on_tool_confirm_async`) | ~~工具执行线程占屏~~ → 监听线程出菜单 | **已改通道(P2)**:`SessionApprovalChannel`——工具线程 Submit 挂 future,监听线程 `TakeForViewer`(按 owner 页归属)跑 presenter 出菜单,Resolve 送回;别页的请求在底栏固定通知位标"main/#N 待审批"(不进正文) |
| 工具确认(前台子代理 `on_tool_confirm` 转发) | 子代理任务线程 | **P3 已绑 owner**:通道有服务者时 Submit(owner=任务号,世代随会话),用户看着那只子代理菜单才开;任务退场/清除经 `DenyPendingForOwner` 按拒收口;无服务者(单发/单测)就地问,行为与旧路一字不差 |
| 工具确认(同步回落:单发/管道/旧装配) | 工具执行线程 | 就地问(无服务者);显示半边仍过 `RunUiSync` |
| `ask_user` 选择菜单 | 工具执行线程 | **P3 已收口**:开屏前排干(`RunUiSync` 空体),问题打印/拒答回显/答完确认行全过 `RunUiSync`(统一提交锁内落笔);菜单读键留在本线程(与工具确认同款取舍) |
| ESC 打断 | 监听线程 | `DenyAllPending` 收口悬着的审批,future 不悬死 |
| 回合收口/换代/退场 | RunTurn 线程 / 会话主循环 / 面板动作 | **P3 绑定收口**:`DenyPendingForTurn`(按 canonical turn 号)、`DenyStaleGenerations`(/clear、/resume 换代)、`DenyPendingForOwner`(面板 x 停止/清除)——确认只解一笔,旧按钮随退场失效 |

## 四、Hook/外部进程/旁路输出

| 入口 | 调用线程 | P3 归属 |
| --- | --- | --- |
| PreUser/PreTurn 阻断行 | RunTurn 线程 | `TermErr`(stderr)——错误面不走 stdout 锁,天然不串正文 |
| hooks 记录归并告警(`AdoptBackgroundHookRecordNotices`) | RunTurn 线程(轮起/轮收安全点) | **P3 归区**:交互终端进显式全局通知区(不落正文);管道/重定向保持旧合同(可见档 stdout、否则 stderr) |
| 外部进程 stdout/stderr(工具子进程) | 工具执行线程 | 经各自管道收账后走 TermOut/TermErr;`StreamScreenPrintHook` 对齐行数账 |
| `TermErr()` 全部调用点 | 多线程 | stderr 无锁直写(错误面);其中"无法归属的诊断"类调用点 P3 起经 `ReportDiagnosticLine` 分流:交互进全局通知区,非交互原样 stderr |

## 五、异常/最终统计

| 入口 | 调用线程 | P3 归属 |
| --- | --- | --- |
| 回合异常收口(`error.unexpected`/传输错误) | RunTurn 线程 | stdout 锁内 TermErr + flush 三保险(在 finish 提交锁内) |
| 输出预算耗尽结构化失败页 | RunTurn 线程 | 同上(按页可见让路) |
| usage 统计行(`stats.line`) | RunTurn 线程 | 按页可见(`main_chrome_visible`,收口锁内现查) |
| `UpdateStatusLineContext`(状态行数据发布) | 调度消费线程(usage 一到) | 只改数据不落笔;重画归 footer/composer 拍 |
| 排队消息回显(`EchoDeliveredQueuedMessages`) | RunTurn 线程(请求边界) | **已收编**:`RunUiSync` 提交 |
| 回流通知/上下文耗尽保留(`MakeNoticeItem`) | 会话主循环 | 查看态不打裸行(事件照进 main 台账);交互终端的诊断行 P3 起归全局通知区 |

## 六、P3 后的写屏纪律(现行)

1. **先记账,再通知画屏**:`TerminalTurnSink::HandleEvent` = 收账(恒跑,
   登记簿锁内,修订号 +1)→ 绘制闸判定(当前页 + 打印水位,提交锁内)→
   绘制半边(闸关时"只记账不上屏")。
2. **换页事务五步**(`RunUiSync` 内):核对目标仍在 → 存旧页草稿/滚动档、
   纪元+取快照(不持终端锁)→ 作废旧正文/footer/painter 锚点/diff 缓存
   → 擦净受管区域铺目标帧、恢复草稿光标 → 期间到达事件照常收账、按水位
   补画;旧 epoch 的绘制被闸,丢画面指令不丢事件。
3. **帧令牌**:`{AgentViewKey, view_epoch, layout_revision}`——resize/
   Ctrl+L/Ctrl+O/换页都翻要素,锁外算好的帧写屏前 `TokenCurrent` 核对;
   /clear、/resume 换代整册作废,旧世代的帧一律失配(不写新会话)。
4. **过渡批已清零(P3)**:P2 名单上的四枚直笔 writer 全部收编——
   footer 心跳走 `PostUiCommand`(丢拍闸防囤积);空闲 composer 整帧/
   重锚/让位走 `RunUiSync`;ask_user 的显示半边走 `RunUiSync`(读键
   留调用线程);TermErr 旁路里"无法归属的诊断"经 `ReportDiagnosticLine`
   分流进显式全局通知区(交互)或原样 stderr(非交互)。
5. **目标绑定(P3)**:排队消息绑定提交时 target key(切页不改收件人,
   队列区按当前页过滤、别页条数点行);审批请求绑定 request id/owner/
   turn/generation(确认只解一笔;任务退场/回合收口/换代按各键拒收);
   停止动作按稳定 task id 分派并显示目标。
6. **生命周期(P3)**:退出按次序收口——调度排干(Flush)→ 审批悬账
   拒收+服务者注销 → 通知区清板 → cli 侧槽位全摘(投递入口关闭,迟到
   写屏查无此口)→ 成员逆序析构(调度器先停、登记簿后走),终端最后
   释放;析构不补画复活旧页。
7. **取证开关**:`LUBANCODE_UI_TRACE=<路径>` 开诊断台账(接收/应用/
   提交三关卡,带 owner/世代/修订号),默认关,不刷屏。
