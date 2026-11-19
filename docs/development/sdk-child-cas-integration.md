# 公开子 Agent、Memory CAS 与忙锁验收组合

## 本笔目标

基线为私有组合 `bbce45aff6da839ec07c622118d969002b1d6408`。先交这份合同，再普通合并已审实现；小 PR 各留原证，组合另收同一头证据。

公开 SDK 宿主须能显式配置前台子 Agent，提交真实父场请求，答真实子票，取消、关闭、同 ID 恢复并查询持值报告。Memory 快照沿真实 Session 锁绑定 CAS 能力。队列争锁允许原有有界 Busy，已提交任务不能丢失。

这是当前模块组合，不代表 SDK 八步全数收工。共享功能分支与 main 另行收口。

## 公开入口与所有者

- SessionOptions 中子 Agent 默认关闭。开启后，宿主明填正数步数与整数时长；越过父场同类上限就拒开，每次调用只准缩小，恢复沿冻结原值。
- 首批只接真实 main、前台、depth 1。Explore 用冻结只读工具交集；后台、嵌套与 worktree 不在本批入口。
- SessionResources 持 AgentTool、协调器与工具借用层。父 Backend 串借，工具调用仍走原运行栈。审批许可只归当前孩子，父场、兄弟和恢复不继承。
- live Finish/Close 回执与历史 producer claims 分列。已确认 Cancel 或父步限只准完整已验前缀缺后续 Prepared；报告仍为 Incomplete，不冒报 Validated。
- CAS 能力由 SessionManager 在真实 Session 锁内绑定。Memory 写后核实际回执、耐久、大小、SHA 与有界读回；关闭封写后才释放场锁。工具原件、topic、项目提交账等未迁范围沿小 PR 合同列明。

## 两处验收收口

真实 PostToolUse 可合法追加内容。原生材料需要另存 raw_payload 时，规划器须计入其表示容量；与材料保全共用判断。desired 请求现成帽，water filling 只分现有 available。纯文本旧路、真实原件、最终 wire 重测及装不下时明确拒绝都保留，不加大历史夹具预算。

Memory worker 原退避仍为 50 次、每次 50ms。并发验收至少一次真实成功，只容另一调用返回完整 live-owner Busy；成功计数、正式条目、pending、failed 与放锁后再次调用逐项核对。新增确定交错须握真实 OwnerLock，核排队原字节不动，再放锁、跑生产 worker、核正式 committed 回执。

## 关场与恢复

关闭先发取消，再等真实调用、监督与线程退出；回调清除先于借用层销毁。值报告不留 Agent、writer 或宿主裸引用，关闭后仍可查询。

同 ID 恢复只核已存计划、操作账与实际采用链，不重跑子 Agent 或未知副作用。缺档、坏行、错归属、错误终态、短材料及超帽读取都须在新场写入前拒绝或按合同保留未知态。

## 组合证据

安装消费模式、focused 来源与 ASan 必需来源从最终源码登记重新取并集，不能拿单笔计数硬拼。Windows-only 用例另按实际平台核；源码库存不冒充每平台实跑数。

保留公开子 Agent 12 册、CAS 10 册、历史采用 8 册、父观察 8 册及原审批、上下文、四场隔离与进程验收。Project Memory 来源须实跑 47 册，本来源恰一枚 Busy 标记；原其余 45 册不改。ASan 原筛选不含这份 Memory 来源，不据组合声称它受插桩。

三平台全量、SDK-only 与组合安装消费、Host、Worker、Runner、依赖图及必需 ASan 均收同一 source/tree、实际 CI merge 和原日志。先保存真实失败，再修、再推新头；局部绿和 Draft 不算整轮验收。

本地只静读、查差异、跑纯数据和文档门。configure、编译、CTest 与原生程序全部交远程 CI。
