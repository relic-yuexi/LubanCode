# Memory worker 有界争锁验收

## 本笔范围

基线为 `bbce45aff6da839ec07c622118d969002b1d6408`。先交这份合同，再改原生测试。生产代码、OwnerLock 裁决和 RunPendingMemoryJobs 退避预算都不动。

本地只查文本和纯数据。三平台原生结果交远端 CI；Draft 不算验收。

## 原合同与实证

RunPendingMemoryJobs 持有真实 `memory-jobs/worker.lock`，串行处理一批任务。对手遇到活持有者，按原 50 次、每次 50ms 计划退避。预算耗尽后返回 Busy 错误，不把它折成成功计数 0。底层文件操作也会花时间，这不是严格 2.5 秒截止。

原并发册要求两次调用都成功。Windows 源 `b4b95c53` 的全量日志显示：首调用约 3.282 秒，处理 8 条；另一调用约 2.703 秒，返回“等待 memory worker 锁超时: 持有者活着: pid 5924”。后者返回时还有两条 pending，首调用最终捞净队列。

原日志 SHA-256：`487affc560464666de09aa9550d729b9d09c67792b69587de9d7cdb621e752ee`。这证明旧双成功断言与有界 Busy 合同冲突；不证明文件系统根因，不称随机故障，也不称本笔修过生产。

## 原并发册收口

保留 8 条真实排队任务、两次真实异步 RunPendingMemoryJobs，以及原输入、退避和诊断。

- 至少一次调用真实成功。另一调用可成功，也只可返回当前进程活 owner 的完整 Busy 错误。
- BrokenLock、建锁窗口提示、一般 I/O 错误和其他超时原因都不能冒领 Busy。
- 只累加成功回执计数，合计仍须为 8。正式条目仍须为 8；pending、failed 均须为 0。
- 两次调用结束后再真实调用一次，须成功且计数为 0，证明锁已释放。

## 新增确定 Busy 册

新册使用唯一临时 home。先排两条真实项目任务，再用真实 OwnerLock 握住这份 home 的 `worker.lock`。调用生产 RunPendingMemoryJobs，任其走完原退避，核完整 live-owner Busy 错误。不给它伪造错误，不额外套重试。

Busy 返回后，原 pending 文件名、完整字节和正式条目都须原样。没有 failed 任务，也没有提交回执。OwnerLock 显式释放后，再调用真实 worker；两条任务都须经 ProcessJob 正式提交，回执各为 committed，内容、身份和计数逐项对齐。pending、failed 均须为 0。最后再调用一次，须成功且计数为 0。

锁归局部 RAII 对象。断言或异常退场，句柄也会释放。测试不传 worker 可执行文件，不旁起后台进程。

## 远端验收线

`test_project_memory.cpp` 原有 46 册，保留其余 45 册断言，本笔新增一册后共 47 册。三平台全量须实跑这份来源并留下原日志；新增册还打印唯一 `memory-worker-busy.bound-and-release` 标记。不得用旧头绿色或空册替代。

首推 `37c54cc4` 发现路径门漏登记这份测试，Linux 全量跳过，仅 macOS 出场。原分类与 skip 事实单独封存。这轮不能算三平台验收。后续只补现成 cross_platform、memory_worker 两处 Memory 敏感模式，专门原件门须核 47 册和本来源恰一枚 Busy 标记。纯数据另拒 46/48 册、缺标、重标和借别来源标记。ASan 原筛选不改，不能声称这 47 册受插桩覆盖。

本笔只把验收对齐现有有界 Busy 合同。任务公平交接、每条任务换锁、延长预算另开合同，须另议。
