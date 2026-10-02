# 前台子审批与父观察组合合同

[开发手册](README.md) · [执行组合](child-sdk-integration.md)

状态：先落合同，再组合两笔内部接缝。公开 SDK 子 Agent 入口与严格历史采用另交。
本地只静读、查文档、跑纯数据门；不 configure、编译、CTest 或运行原生夹具。

## 本笔接什么

以执行组合 `4413b315` 为基线，接真实子会话审批 #281 与父终态观察 #283。
保留共用 ExecutionOwner、当次取消、typed 终态、逐票 lease 与普通 CLI 路径。
两笔各自 CI 不能代组合 CI；本组合独立开私有分支，不写共享功能分支或 main。

父 producer 只生成一份真实 SubagentSpawnProvenance。审批所需直接父 SID/run、
声明 turn/action/message 从这份 owned 值取，不再另存一份可能漂移的母来源。
主场 operation 与真实执行 attempt 仍沿当次调用 cause；不能拿 task ID、模型
provider ID 或孩子 SID/run 裸串冒充归属。SID/run 跨父可同串。

trajectory_spawn 带实际已解析 Foreground/Background。首批 scoped 主机能力只供
main foreground；CLI background 保原自有取消与钩子路，不借父票或父 grant。
真 foreground producer 完成 Request/Bootstrap/Link 后，才 Attach 到当前父 action。
fixture 也走原 producer、原 bridge 和原 typed Finish，不添假成功回执。

lease 实现须归 Agent 循环能单向链接的中立层。不能反向链接上层 runtime，
不能靠整包强拉、忽略 unresolved symbol 或复制实现遮住目标归属错误。
SDK DLL、SDK-only、组合 Host 与 CLI 都只消费同一实现。

## 拥有者与退场

子票、当前孩子 grant 与关闭标记仍归原 SessionApprovals；不另建 pending 表。
AcceptForSession 只管当前子会话，按宿主/operation、直接父、孩子、cwd/floor 隔开。
硬拒、PreToolUse Ask 和 PermissionRequest 每回照裁；父 ordinary allowed 不递孩子。
缺真实归属或 capability 便拒。迟票、取消票、重复回答不能复活已关孩子。

孩子持 wrapper、overlay、Agent、turn 与子账；父 Backend/工具/MCP 只借到真退出。
Finish 与 checked Close 分列，未知不重试。回调须先于其借用销毁，wholechild
返回前撤子 scope/grant；取消与 Close 等真调用、监督及取消合流线程退出。
owned 父观察与终态报告不延长 Writer、hub 或工具宿主原始借用。

父观察先核真实终态与同份有界子卷，再存 observation 和原始 capture。
正式结果仍由本场 ResultStore、selected、Tool 消息和上下文准入原路收口。
未知观察或捕获阻止后续模型与摘要调用；raw/effective 两份材料不可混称一份。
这笔不新增恢复采用结论；下笔只读严格校准另核历史 admission 和实际 prepared。

## 验收

先静读合并差异与全部 producer/fixture 调用；冲突保两笔语义，不删旧断言。
新增行为或后续公开入口另写合同与验收；这笔不靠重复夹具添虚数。

组合头保原 14 场真实子审批、8 场父观察、14 场 scoped 底座、7 场 typed 终态、
2 场执行组合与 10 场前台上下文。父观察 12 条共用路径标记照留，Unix 真目录
alias 拒绝另核专属标记，Windows 不冒记未获权限的 symlink 路。
focused 真实来源并集应为 21，ASan 必需来源并集应为 26；从本头脚本与登记
重算，不抄旧摘要。安装 consumer、Host、Worker、两种 Runner、九份真实
依赖图、三平台全量、必需 ASan 都认同一源码与同树受测 merge。

原失败日志、原始 run 与文件摘要留存。新头全绿只能证明本轮，不倒推旧错根因。
普通 SDK 四态审批、CLI 子 Agent/后台、Memory/Skills、结果投影与恢复原册照跑。
工具原件留执行端；远端 Full 仍须 Node 许可与本場参数双开。
