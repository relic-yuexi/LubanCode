# Managed 开场目录预留

状态：内部前置候选。只接新目录、持锁归属发布、子目录收尾与 SessionManager 的 LocalTrusted 反向门。没有公开 Managed Session，也没有认证或 Policy 通行证。

## 开场次序与所有权

可信宿主先准备完整 `ManagedSessionOwnership`，再调用内部 `ManagedSessionReservation::Reserve`。只许显式绝对根与安全单段 workspace/session 名；workspace 须已登记。原子 `create_directory` 只占全新 Session 目录，绝不复用旧目录。预留实例持真实 `SessionLock`；此时没有 artifacts、subagents 或 V3。

`PublishOwnership` 调原 a554d4d6 私有 attempt。四份 helper 文件按原 blob 采用，不改首回执、未知留痕或旧场拒认领规则。只有本预留实例亲自取得 `CommittedDurable`，`Finish` 才能继续；调用方不能递一份可伪造的 publication 值换取许可。Finish 仍在原锁下重捕获，核完整归属、原发布字节与目录条目，再逐个创建 artifacts、subagents。成功交出拥有目录与原锁的 move-only 对象。调用方持住它，才可启动现有 V3Writer；这枚内部对象不证明身份、权限或 SDK 就绪。

Finish 只能成功一次。失败立即放锁，实例进入终态；析构也放锁。发布未确认或发布后收尾失败，保 sidecar 与实际残留，不删、不改、不读回升级。未发布的空预留可在放锁后仅以非递归 remove 清理；如出现其他条目则保留。已发布目录绝不清理。任何路径都不递归删除。重新 Reserve 一律拒已有目录，不能把未知归属降成 LocalTrusted，也不提供“同归属便重开”捷径。

空目录清理只尽力而为，不承诺删净。SessionLock 关闭句柄后若删不掉锁文件，原目录连同 session.lock 一并保留；清理不能借失败理由扩大删除范围。AtomicWrite 已受理后，即便回执为 NotCommitted，也保留目录供查验。

## LocalTrusted 与未交边界

旧 `TrajectoryDirectory::CreateSessionV3` 行为原样保留。SessionManager 在本批接到的点名读/恢复/管理入口读正文前核归属；持锁恢复与续场在抢锁后再核。枚举先核 sidecar，遇 Managed、坏件或捕获错误就跳过，不先读 session.json 或主账。首轮捕获和锁后捕获均沿私有 helper，错误不能折成缺件。

本批不接 Runtime、Session、SDK facade 或授权回调。SDK 预读 Soul、通用 raw reader 不在此笔改动范围，不能声称“全部托管读面已封”。真实 Managed 完整接线前，公开 SDK 仍未交 Managed 模式。扩展、Memory、Job 等托管能力也未开放。此原语沿可信宿主稳定父目录的既有前提，不承诺恶意目录替换、OS 沙箱或跨机 fence。

## 验收

保原 helper 六 CASE 与 marker，新增独立 reservation 册。远端须真实走 Reserve → 原 AtomicWrite → Finish → V3Writer → reader，核 durable 前无子目录/V3、同锁持续、成功后真实首行可读。受控文件/目录刷盘失败须保首事实；未知残留不能重写、重开或降级。补收尾失败、目录漂移、重复收尾、资源退场，以及 LocalTrusted 新场/恢复回归、标记/坏件点名拒绝与枚举跳过。

本地只做文本、源码守卫与纯数据检查；不 configure、编译、跑 CTest、原生或 HTTP。CI 接线由后续集成收口，未得本源远端证据前不称验收通过。

## 本笔接点与验收册

`SessionManager` 的点名预检、ResumeAsNew、来源链 resolver、RecordResumeReference 先核 sidecar；NextMainRunId、LatestResumableSessionId、RecoverWorkspace 枚举先核再读。原五处锁获取统一走 LocalTrusted 捕获 → 实际 SessionLock → 锁内捕获与逐字核对。相对存储根先转绝对路径，不改 CLI 配置口径。原 archive/unarchive/delete 共用路径门加归属核对；旧 pending delete 遇标记或坏件保留现场。引用扫描另记 `ownership_unreadable_sessions`，拒删回 `session.delete_references_unknown`，不编造 resume 引用。

新册 `test_managed_session_reservation.cpp` 有十 CASE，逐案打印一次 `[managed-session-reservation-path]`：`actual`、`reserve`、`unknown`、`not-committed`、`finish`、`local`、`refusal`、`locked-refusal`、`ancestor`、`delete-residue`。`locked-refusal` 用现有 Clock 在正文预检后、抢锁前投下一枚坏标记；核开场参与者未调用、主账未改、锁已退。它不冒称在操作系统抢锁指令内部造竞态。真实锁后漂移原语沿保留的六 CASE；生产五处锁后复核另受源码守卫约束。

本笔没有把持续运行的 LocalTrusted 场改成可认领 Managed；没有在每次运行中写入前重问归属。静态 raw reader、Soul 预读、活动场观测、workspace lifecycle 总账与跨项目 Memory 不在这批托管接线内。公开 Managed 保持未交，不能靠这份目录合同宣称执行授权闭合。
