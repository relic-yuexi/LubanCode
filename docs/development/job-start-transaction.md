# Job 启动事务

本笔只补 `ToolJobCoordinator` 启动债。基线取渠道边界 `e52dc07f`，私有合入 Atomic 夹具 `37ac4b50`。不开放 SDK 后台 API，不接 CLI 全局后台表，不改恢复重派、inline fallback 或 Session Close 产品口径。

## 现有缺口

`DispatchJobLocked` 已写 dispatched/Started、置 running 并占槽；随后复制 JSON/executor、分配 finished 和线程 owner。这些步骤在启动 `try` 外。抛错便可能留下 running 和配额，工具实际没跑。现有 thread catch 也只接标准异常。

## 启动与拥有

登记和接单顺序不动。启动事务接住 Started 后的 clock、资源占用、context/executor 复制、owner 分配与线程启动。失败不回滚已写事实，不重跑工具。未启动线程时，沿原单写者完成信封记 `tool.job.worker_start_failed`，原生执行终态和 Job observed 各写一次。事实未确认时留明确 unknown/pending；不能假报持久 failed 或收场成功。配额与资源槽独立释放一次，不靠落账成功来退槽。

线程 owner 先入协调器，再启动线程。失败留下的 executor 副本也归 owner，锁外清理；不能在 `jobs_mutex` 下销毁完整用户回调。既有 Shutdown/Reap 仍 join 真线程，不 detach；退出后才销回调和宿主借用。关闭和 Reap 争同一寿命锁，不能双 join。

内部 `ThreadStarter` 借一只预存 `std::thread&`，接 owned entry；默认只执行真实 `std::thread`。测试可在启动前抛错，或把真线程放入这只 owner 后抛错。后者有 live thread，按真实 worker 完成信封收账并 join；不能再造“未启动”失败。启动器不得把线程另藏一处，也不得留下未交付线程。这个测试接缝不进公开 SDK。

worker 只持本 Job owned context/cancel/epoch 和稳定协调器 mailbox。Started 后的准备失败一经接住，不派第二回；线程启动后也不移除 live owner。执行器本身抛错仍沿原 `tool.job.executor_exception`，业务成功、取消和原结果存储规则不改。

本笔收可接住的启动异常。内存分配器持续失效、用户析构抛错及任意不守约 ThreadStarter 不在承诺内。账本拒写仍会留下真实确认缺口，不拿内存状态充回执。

## 验收

保原 JobCoordinator 16 册和所有原断言。新来源固定 6 册：真实 executor copy 抛错、启动前标准/非标准拒绝、启动后抛错但真 worker 仍归 owner、失败后配额/资源供下一 Job、失败收账未确认、真实 Shutdown/Reap 与 capture 退场。失败解门先于 future join，等待有界。每条主路末尾唯一标记，三平台 focused 与 ASan 从真实 registration/JUnit/LastTest 核源、六册、非零断言和标记。

本地只读源码、跑纯数据和文档检查。configure、编译、CTest、原生进程全交远端 CI。新头实证单列，不能借旧 CI 验收。
