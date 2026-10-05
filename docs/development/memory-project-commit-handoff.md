# Memory 项目提交的同进程交接

基 `3e6fac1c`。真实 `57d70c91` Windows combined focused 中，四场案仅同项目第二场报 `memory.commit.lock_refused`，活 pid9556，零 stage；同项目首场及异项目两场 committed。原件缺锁释放回执，不能唯一断定持盘慢或释放残留。本笔修交接缝，不增加原20×100ms重试，不改 SDK Memory 12案和旧 project_commit 12案。

`project_commit.cpp::Run` 增内部 move-only 项目 lease。按实际 canonical/equivalent memory 位置共用 FIFO；memory 尚未建时，用已核 canonical workspace 下固定 memory 末段，所有目录准备和实际写入均在 lease 内。错误不当缺件，大小写／别名不造第二个槽。拿 lease 后仍须过原 `OwnerLock::TryAcquire`，不凭同 PID 偷锁。

registry 只存 weak 槽。取 strong 快照后锁外查 filesystem 身份，发布新槽前复核代际；waiter 和 holder 持 strong，不能释放前票时按路径删表，把旧等待者和新调用分成两队。表只在短锁下复制／修剪／发布；等候、取消谓词、写回调和 AtomicWrite 不持 registry 或排队锁。不同项目各自前进。

每票排队后再在锁外问取消，短 CV 等候可取消；取消或谓词异常须撤本票并唤醒后票。同线程已持或已排本项目，返回 `memory.commit.reentrant`，零本笔目录／Intent／Topic 写入，不能无限等自己。其他错误仍交 Run 原异常边界。命中取消回原 `NotStarted + memory.commit.cancelled`；写后取消与未知结果照旧，不升级成功。

lease 声明先于 OwnerLock，退出时先释放真实磁盘锁（包括原删除重试），再交进程内棒。整段 mutation、旧件 Confirm/Inspect、收据确认、异常退场都在租约内。lease 不伪造任何 native stage 或耐久回执。原坏 owner、外部活锁、未知 token、死锁隔离、foreign-token Release 全留；磁盘锁若真留残件，下一票照实拒。

仅同一实际 project_commit 实现实例共享该表；SDK 多场共用同一 DLL。其他直接 OwnerLock 路及独立静态／DSO 副本仍过原磁盘锁，不宣称跨模块全局队列。没有新公共 API、SDK core 改动、调度线程或生产测试口。

新册 `test_memory_project_commit_handoff.cpp` 用真 Commit 路和既有 `commit_testing::WriteFile` 停住真实写阶段。验同目录／别名交接、等待取消零 Intent/Topic、异项目独立、坏／外部活锁不偷、异常退票、同线程已持／已排重入；真线程均 join。原12+12案、所有预算与断言不改。本地只查源码／纯数据，三平台与 ASan 交远端；不本地配置、编译、CTest 或原生执行。

六 CASE／六标记 `[memory-project-handoff-path]`：`same-directory`、`aliases`、`waiting-cancel`、`independent`、`retirement`、`external-lock`。首案验两笔实际交接；别名案在 Windows 验大小写拼写，POSIX 验父目录 symlink，未据此声称多票公平或 Windows 短名已实测。
