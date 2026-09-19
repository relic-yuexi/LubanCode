# TUI 写屏入口清单(正文/footer/审批/Hook/异常/最终统计)

> 按代理状态投影单(`todos/TUI主子代理视图彻底隔离_单写者与按代理状态投影.todo`)
> §七 P0 的取证基建:把"谁能往终端上写字"全部列出来,注明调用线程与
> 锁顺序。P1(身份与状态分账)已落地;P2(收拢写者、原子换页)收编
> 这张清单时,一处一处对号入座。清单以取证基线 `d20547e4` 为底、随
> P0/P1 落地同步更新。

## 锁的 vocabulary

| 锁 | 住处 | 管什么 |
| --- | --- | --- |
| `StdoutWriteMutex()` | `cli/console_input.cpp` | 终端字节流互斥(锁内只写字节,不等输入) |
| 泵画笔锁 `UiEventPump::render_mutex()` | `app/ui_event_pump.cpp`(P1 起递归) | 串行化一切渲染:消费线程按帧渲染、控制路就地画、换页/重铺事务 |
| 登记簿锁 `AgentViewRegistry::mutex_` | `app/agent_view_registry.hpp`(P1) | main 活回合账(视图账/修订号/水位)与查看页身份 |
| `ConsoleReadMutex()` | `platform/console.hpp` | 逐键输入权(composer 与监听线程错峰;与画屏路径只单向相交) |

**锁序铁律**:泵画笔锁 → StdoutWriteMutex(渲染闭包内拿 stdout,无倒置);
泵画笔锁 → 登记簿锁(事件应用);换页事务持泵画笔锁后经钩子再拿 stdout。
`ConsoleReadMutex` 与 stdout 只在 DSR 光标查询处短暂相交(get/释放,限时)。

## 一、正文/思考/工具条目(回合内)

| 入口 | 调用线程 | 路径与锁 |
| --- | --- | --- |
| `TerminalTurnSink::DrawEvent`(P1 拆出) | 泵消费线程(流内 delta,33ms 帧批)或产生事件的线程(控制路,`DispatchInline` 就地) | 画笔锁 → `ToolDisplay/StreamBodyTracker`(各自锁内拿 stdout)。P1 起:绘制闸(`PageVisible`,登记簿按当前页+水位)关闭时走"只记账不上屏"支路 |
| `StreamBodyTracker::OnDelta/OnBlockBreak/FinalizeRepaint` | 同上 | 锁外拼装(条 4)→ stdout 锁内落笔;锚点账 `start_row_` 只在自家 |
| `ToolDisplay::OnToolStart/OnToolDone/OnThinkingDelta/OnThinkingDone/OnBuiltinToolDone/OnBatchSkipped` | 同上 | stdout 锁内 `TranscriptPainter::PaintNew/Repaint`(锚点原地改写);pipe 模式稳定纯文本两行 |
| `TurnInputListener` 插行([已排队]/[已打断]/粘贴截断) | 监听线程 | stdout 锁;插行后 `RunStreamScreenPrintHook()` 作废正文块锚点 |
| 换页/查看帧重铺 `print_view_frame`(忙路+空闲两份) | 监听线程 / 主线程(composer) | P1 起整段包进画笔护栏(`SetViewSwitchGuard` → 登记簿 `WithMainRenderLock`):擦旧帧(stdout 锁)→ 视图切换钩子铺新帧 → 记跨读取账 |
| `TranscriptUiController::PrintViewedTranscript` | 上述钩子内(会话装配) | stdout 锁;main 页活回合走登记簿成对协议(`TakeMainLedgeForRepaint` → 打印 → `MarkMainPrinted` 钉水位) |

## 二、footer/底栏

| 入口 | 调用线程 | 路径与锁 |
| --- | --- | --- |
| 流式 footer 画/擦(`RedrawStreamFooterLocked/EraseStreamFooterLocked`) | 泵消费线程、监听线程、心跳线程(200ms) | stdout 锁;帧账(`StreamFooterState`)同锁内 |
| footer 心跳 `StreamFooterHeartbeat` | 独立心跳线程(只活 Run() 一段) | stdout 锁;活动条秒数随帧 |
| `BeginStreamFooter/EndStreamFooter` | RunTurn 线程(回合起收) | stdout 锁 |
| 空闲 composer 整帧(composer 输入框/队列区/代理坞/状态栏) | 主线程(ReadLineKeyByKey 的 100ms 拍) | stdout 锁;帧 diff(`BottomChromeFrame`)锁内计算 |
| 收口 chrome(PrintTurnFooter/统计行/尾分界线/FinalizeRepaint) | RunTurn 线程(StopUiPump 之后) | P1 起持泵画笔锁(`finish_render_hold`)贯穿收口;按页可见性(`MainVisibleForChrome`)让路 |

## 三、审批/交互菜单

| 入口 | 调用线程 | 路径与锁 |
| --- | --- | --- |
| 工具确认(ConfirmToolUse 的 y/a/N 菜单) | 工具执行线程(产生事件的线程) | 先 `RepaintSuspendScope` 挂起 footer 重画,`ConsoleReadMutex` 读键;确认块擦除经 painter `TrimBelow` |
| `ask_user` 选择菜单 | 工具执行线程 | 同上;菜单退出前 `RepaintSuspendActive()` 让监听线程让路 |
| 审批待决提示 | 工具执行线程 | stdout 锁内追加;P3 收口:审批请求绑定 request_id/owner,不进当前页正文 |

## 四、Hook/外部进程/旁路输出

| 入口 | 调用线程 | 路径与锁 |
| --- | --- | --- |
| PreUser/PreTurn 阻断行 | RunTurn 线程 | `TermErr`(stderr)——错误面不走 stdout 锁,天然不串正文 |
| hooks 记录归并告警(`AdoptBackgroundHookRecordNotices`) | RunTurn 线程(轮起/轮收安全点) | P1 起按页可见:main 在屏走 stdout+TermOut,否则 TermErr |
| 外部进程 stdout/stderr(工具子进程) | 工具执行线程 | 经各自管道收账后走 TermOut/TermErr;`StreamScreenPrintHook` 对齐行数账 |
| `TermErr()` 全部调用点 | 多线程 | stderr 无锁直写(错误面,允许与正文交错——P2 的显式全局通知区收编) |

## 五、异常/最终统计

| 入口 | 调用线程 | 路径与锁 |
| --- | --- | --- |
| 回合异常收口(`error.unexpected`/传输错误) | RunTurn 线程 | stdout 锁内 TermErr + flush 三保险(本地兼容端 Effort 诊断单) |
| 输出预算耗尽结构化失败页 | RunTurn 线程 | stdout 锁外逐行 TermOut(错误面,当前未按页分账——P3) |
| usage 统计行(`stats.line`) | RunTurn 线程 | P1 起按页可见(`main_chrome_visible`);详细态才出 |
| `UpdateStatusLineContext`(状态行数据发布) | 控制路线程(usage 一到) | 只改数据不落笔(stdout 锁内改 `StatusDataSlot`),重画归 footer/composer 拍 |
| 回流通知/上下文耗尽保留(`MakeNoticeItem`) | 会话主循环 | stdout;查看态不打裸行(事件照进 main 台账) |

## 六、P1 分账后的写屏纪律(现行)

1. **先记账,再通知画屏**:`TerminalTurnSink::HandleEvent` = 收账(恒跑,
   登记簿锁内,修订号 +1)→ 绘制闸判定(当前页 + 打印水位)→ 绘制半边
   (闸关时 ToolDisplay/StreamBodyTracker 走"只记账不上屏")。
2. **换页事务**:擦旧帧 + 铺新帧整段持泵画笔锁;main 页重铺用成对协议
   (关闸取快照 → 打印 → 钉水位),离屏期间的事件不丢不重。
3. **仍在缝里的(P2 收编)**:确认菜单/ask_user 由工具线程直接占屏(挂起
   footer,但不清页);外部进程直写;TermErr 旁路;监听线程插行靠
   print-hook 对账而非统一调度。这些是 P2"收拢写者"的粮草。
4. **取证开关**:`LUBANCODE_UI_TRACE=<路径>` 开诊断台账(接收/应用/
   提交三关卡,带 owner/世代/修订号),默认关,不刷屏。
