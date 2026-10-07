# SDK 阶段进度与验收边界

核查日期：2026-10-08。功能分支已正常合入 `5aaad8b5`，与已验源码
`80bb443c` 同树。前批代码验收对应这枚源码。后续装配、托管派发与公共头文件
已组成待验批次，尚未取得新源 CI；公开托管文本入口另片开工。

目标：新宿主只调用公开 SDK，就能运行完整会话，不必复制内部运行栈。
基础会话闭环已交付。CLI 全部能力、托管执行、完整存储替换与网络服务仍未收齐。

## 本轮已合

仓库只留总 Draft [#234](https://github.com/relic-yuexi/LubanCode/pull/234)，未添 PR。
Worker #245、Runner #246、SDK #325、Job 日志 #332 和组合 #333 已合；
本轮沿原 CI 分支验完 `80bb443c`，再正常合进功能分支。

- 显式命令 Job 与 Named 结果仓保首次未知事实，挡住后续输入、排队派发和后台启动。
- 场内十处 live 读取取真正 Writer 捕获；开场五模块共用一次惰性 File 捕获。
  锁后重查实际材料与来源指纹，保原模块首错、原生锚与 EOF 条件。
- Policy 调用、通知、退订和最后一份捕获销毁共用原回调屏障。
  阻塞重入明确拒绝；宿主仍须留住在用对象。
- CLI 单次提问和交互主场共用 ExecutionOwner。交互 Agent 在稳定槽重建，
  保旧命令指针、引用与闭包；失败留空，原串行纪律照留。
- AgentLoop 每次发送前走内部门，覆盖首发、恢复重试和后续步。
  本轮尚未绑定 Policy 原发起者；摘要、Compact、采样与后端内部重试另接。
- Managed 底层准入把真实 SessionLock 与原持久回执交给 Manager，首条 V3 写完整创建身份。
  失败和 Close 后禁 Local 回落。这笔属内部存储链，未开放公开 Managed 执行或 ACL。
- 三份纯测试的九处 YAML 读取显式用 UTF-8，旧失败门、CASE 与预算保留。

## 同源远端验收

`80bb443c` [首轮 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37468467971/attempts/1)
十五项十四过，整轮失败。Windows 常规腿下载 zlib 和 curl 均报 SSL35，
Configure 退出，产品未编译、原生未运行。七处缺件上传另记连带失败，
原日志未判明网络根因。首轮失败事实与原件封存不改。

仅对同一 SHA [重跑失败项一次](https://github.com/relic-yuexi/LubanCode/actions/runs/37468467971/attempts/2)，
整轮成功。实际新执行只有 Windows 常规腿；十四项首轮成功按原执行沿用，
不能称三平台重新跑了一遍。Windows 新全量 789/789，SDK 69 来源／552 CASE／
37 项安装消费；宿主 64 CASE，Worker 18、Runner 11、runner-only 11 场进程验收均过。

两轮合看，同源已验：

| 范围 | 实际结果 |
| --- | --- |
| 三平台全量 | Linux 787、Windows 789、macOS 790 项全过 |
| Lua ON | 六套各 69 来源／552 CASE／37 项安装消费 |
| Lua OFF | 三套各 67 来源／537 CASE／34 项安装消费 |
| SDK-only 跨镜像 | 三平台各五笔真实命令 |
| ASan | 首轮实跑 169 来源、76 重点门、1869 CASE／77029 断言；26 支持件与实际 PCH |
| TSan | 首轮十四册通过 |
| 全量来源 | 三平台各十组同次原输入 hash join；完整实际登记、argv、JUnit 与 LastTest 已核 |

首轮封账 1896 件／67265988 字节，摘要
`1c5b4d44cd6e766fcc205df13422e6a1260ab7a8f096c38a8307d5b54e77ed9e`。
第二轮独立封账 436 件／38691778 字节、24 份全新 ZIP，摘要
`382ac9eeb0e3b2e72b4d9bfd50dfe648fd2ff0ab038b5658590bdf5585612380`。
根端逐件复核两账；没有把首轮失败改绿，也没有借别枚源码验收。

## 待验组合与后续源码

前轮收尾后继续开发。以下部分已收成组合，未混进 `80bb443c`，也未合功能分支：

- `d3fc1c42`：真实 Managed 开场贯穿 Service、Runtime、Ledger；schema3 来源账；
  公开项目登记、存储 View、身份读取与 Close；Worker v1 版本门。
  源码独审已闭两处阻塞，原生尚未执行。安装消费者的五份 ownership/V3/lock
  stdout 标记可核真实 LastTest；完整 Managed STATE 与 Close 主账原件尚未上传。
- `18f3901a`：原 Local 执行装配拆成五个真正被调用的同步阶段。
  原 Initialize 27944 字节及完整 core.cpp 可机械还原，源码独审通过，远端原生待验。
- `89675f52`：内部 Managed 文本派发与终态接原队列、原提交锁、V3 绑定和实际结果。
  独审查出的关闭分配异常、结果 NUL 读写差异、停机后已派发终态落账三处已修。
  八场原生验收已写入现有全量、SDK 与 ASan 跑道，原生尚未运行。
- `69631da1`：模型与操作值原声明移进独立公共头；原安装消费者先包含两头再包含 core。
  安装原件与源码头逐件取 hash，缺件或复制失败保实际 failed 证据，独审通过，原生待验。
- Boxed、微软 NVX 已查固定源码。[沙箱设计](../architecture/sandbox-platform-design.md)
  收清宿主、guest、共享项目、实例引用与关闭边界；适配器尚未实现。

公开文本开场、Submit、ReadOperation 另片接原装配与当前 Policy；文本响应门也已开工。
这些接口和新验收尚未交付，不能把内部派发合同当成公开托管执行。

这些源码、检查与设计不能借前源绿灯算交付。组合只沿现有 CI 分支收三平台新证据，
保持总 PR 为 Draft，不新添 PR、远端 CI 分支、原生命令或超时预算。

## 八阶段现状

| 阶段 | 已合范围 | 尚缺 |
| --- | --- | --- |
| 收口当前批 | 上述子 PR 与 `80bb443c` 已合功能分支；只留总 Draft #234 | 总功能分支合 main 前仍须核最终组合 |
| SDK 完整化 | 五件基础工具、web_fetch、Todo、显式 Skills、项目 Memory Recall/Save、深度一子 Agent、主 Action、strict Lua、Package 盘点、RAG 示例；显式命令 Job、Named 结果仓 | 其余 CLI 工具、自动 Memory、Skills 管理、Package 挂载、通用 Detached、嵌套后台任务与 CLI 全部迁移 |
| 四口 SPI | Session EventSink、Memory 片段 Blob provider、PolicyProvider 原语 | 完整 JournalStore/BlobStore 与恢复读写替换；本阶段不引数据库 |
| 依赖瘦身 | updater、渠道、Gateway 留宿主；Lua ON/OFF 三平台闭包与安装已验 | 端侧最小装配继续收窄 |
| 身份与治理 | 身份值、动作、Policy 与订阅；内部 Managed 底层归属 | 公开托管存储候选验收、实际执行授权、同 ID 恢复、查询与审批隔离；旧账迁入另批 |
| 公开服务 | AppServer 内部网页与 WebSocket、本地可信 Worker IPC | HTTP/SSE、持久 outbox/ACK、事件游标；gRPC 按需 |
| 分布式与存储 | 本地 Worker、独立 Runner、两端部署合同 | 网络登记、心跳、鉴权和存储/队列适配；池化与 Sandbox 池另守前置门 |
| 扩展与编排 | 可信 C++ 窄中间件、前台子 Agent、主 Action | 动态库、热卸载、沙箱、工作流和多 Agent 公开装配 |

GPU 占用与网络连通交用户处理。同项目可开多场，不加项目独占工作目录。
默认只传 preview；Full 须 Node 许可加本场参数。子 Agent 默认关，宿主显式给预算，
审批许可只管当前子场。本地只查源码、文档和纯数据，未配置、编译或跑项目原生。

## 前轮证据

[#333 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37309659950)
对应 `c522a51c`，已合 `fd76b6a5`；[#332 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915212)
对应 `0066ac3e`，已合 `6e2702b2`；[#325 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357534)
对应 `2c376020`，已合 `756bf02a`。旧证据只说明各自源码。

前源 `a23f98e6` [CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37452164239)
虽跑过全量与 SDK，Windows 后置纯检查 cp1252 解码失败，整轮仍失败。
2211 件／87003089 字节封账摘要为
`99f11a9a9d2c185014565eea5d1fe577e8acea10fa1b29d34a4dc62ad2872e79`。
更早 `e79eea52` [CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37439210845)
暴露未知结果发布后继续执行与 workflow 开场问题，整轮失败；其封账仍保留。
失败材料没有删除，也不混入后续成功结论。
