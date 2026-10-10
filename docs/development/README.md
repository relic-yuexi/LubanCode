# 开发手册

[文档首页](../README.md) · [架构说明](../architecture/README.md) · [参考手册](../reference/README.md)

- [构建与发行](build-and-release.md)
- [LubanCore C++ SDK（实验版）](lubancore-sdk.md)
- [SDK 显式网络搜索](sdk-web-search.md)：逐场服务、凭据、预算与取消；#335 同源三平台已验。
- [SDK 开场线程失败清理](sdk-opening-start-cleanup.md)：真实开场失手、资源退场与 Shutdown 交错；#336 同源三平台已验。
- [SDK 采样参数与终止原因](sdk-model-sampling.md)：真实请求与回复；#337 同源三平台已验。
- [旁路采样看门狗关场](sdk-sampling-lifetime.md)：启动失败、Backend 异常与真实线程收讫；#338 同源三平台已验。
- [Memory 抽取共享底座](sdk-memory-extraction-core.md)：共用同步抽取、保住来源与 CLI 入口；#339 同源三平台已验、已合。
- [Memory 抽取线程启动失败](sdk-memory-worker-start-cleanup.md)：失败槽保身份、原 CLI 收账、创建边界清理；#340 同源三平台已验、已合。
- [Memory 后台异常收场](sdk-memory-worker-exception-cleanup.md)：保住已归还用量、丢弃部分候选、接真实收账；#341 同源三平台已验、已合。
- [Memory 持久触发身份](sdk-memory-durable-trigger-identity.md)：评估归旧触发轮，回合间写回执不借旧号；#342 同源三平台已验、已合。
- [旁路短借 owner](sdk-memory-bypass-lease-owner.md)：前台冻身份，关场退旧借；#343 同源三平台与实际 TSan 已验、已合。
- [标题精炼启动失败清理](sdk-title-start-cleanup.md)：绑定及创建失败一次收场；#344 同源三平台与 ASan 已验、已合。
- [标题后台函数体异常](sdk-title-worker-execution.md)：保住归还用量，失败一次收货；#345 同源三平台与 ASan 已验、已合。
- [SDK Backend 回调与最终析构](sdk-backend-callback-owner.md)：回调、最终析构与分配失败回滚；#346 同源三平台与 ASan 已验、已合。
- [Memory 共用词法门控](sdk-memory-learning-gates.md)：共用文本、证据与稳定原因名；#347 同源三平台已验、已合。
- [SDK 用量快照](sdk-memory-usage-snapshot.md)：返回事实先记账，取消与正文拒收不造成功；合同先交，待本源验收。
- [Windows 超时进程映像](sdk-windows-timeout-process-images.md)：#345 同源现场材料已验、已合；旧超时根因未定。
- [Memory 批量提交失败诊断](sdk-memory-batch-failure-diagnostic.md)：失败结果带回阶段观察；原断言照留，不改写入语义。
- [SDK main Action 中间件合同](sdk-action-middleware.md)
- [SDK 显式 Skills 合同](sdk-skills.md)
- [SDK 项目 Memory 只读召回合同](sdk-memory-recall.md)
- [Memory CAS 能力合同](memory-cas-capability.md)
- [本卷同 ID 恢复读面合同](session-recovery-view.md)
- [项目 Memory 提交与回执合同](memory-project-commit.md)
- [Memory worker 争锁与更新失败诊断](memory-worker-contention-diagnostics.md)
- [Memory worker 有界争锁验收](memory-worker-busy-acceptance.md)
- [SDK 项目 Memory 正式保存合同](sdk-memory-save.md)
- [SDK 子 Agent 首批合同](sdk-subagents.md)
- [子代理终态回执合同](child-terminal-receipt.md)
- [父 V3 子终态观察与未知止损](child-parent-observation.md)
- [子结果严格历史采用合同](child-history-adoption.md)
- [Runner 就绪探针与失败原件](runner-probe-evidence.md)
- [SDK 存储、事件与策略 SPI 合同](sdk-storage-spi.md)：场束与独占租约、CAS/named 映射、有界事件流、策略归属及逐口交付门。
- [Session named 结果存储](sdk-named-result-blobs.md)：真实结果读写、同 ID 恢复、写 lease 与独立只读句柄；保留默认 File。
- [主场 V3 Journal 所有权](sdk-journal-owner.md)：归拢真实 File 账、追加借用、首次未知与捕获恢复；公开存储替换口尚待后批。
- [Command Jobs 缺会话预检补正](sdk-command-job-missing-resume.md)
- [Command Jobs 探针产物选择](sdk-command-probe-artifacts.md)
- [后台子代理线程启动收口](agent-thread-start-cleanup.md)
- [线程 capture 收尾夹具](thread-capture-owner-fixture.md)
- [前台子 Agent 本次调用上下文](child-foreground-context.md)
- [可取消审批与逐票 lease 合同](scoped-approval-lease.md)
- [前台子轮异步审批接线合同](child-async-approval.md)
- [SDK 依赖瘦身：更新器归宿主](sdk-updater-boundary.md)
- [SDK 依赖瘦身：Release 查询归 CLI](sdk-release-query-boundary.md)
- [测试指南](testing.md)
- [分布式工程协作与远端验收](cluster-collaboration.md)
- [文档规范](documentation.md)
- [命名与计数](naming.md)
- [安全模型](security.md)
- [界面多语言](i18n.md)
- [UTF-8 编码关口](encoding.md)
- [TUI 排版约定](tui_style.md)

改用户可见行为时，先查[文档同步矩阵](documentation.md#10-改动同步矩阵)。
提交前再跑：

```powershell
bash scripts/check_docs.sh
git diff --check
```
- [前台子执行组合验收](child-sdk-integration.md)
- [前台子审批与父观察组合](child-approval-observation.md)
- [公开子 Agent、Memory CAS 与忙锁验收组合](sdk-child-cas-integration.md)
- [SDK 前台子 Agent 装配](sdk-subagent-assembly.md)
- [结果仓 Windows 原生路径入口](result-store-windows-io.md)
- [SDK 渠道与 Gateway 宿主边界](sdk-channel-gateway-boundary.md)
- [SDK Memory 写计划失败诊断](sdk-plan-atomic-diagnostic.md)
- [SDK 显式 standalone Lua](sdk-lua.md)
- [Updater 活持锁夹具就绪](updater-lock-ready.md)
- [SDK 冻结计划短拒重试](sdk-plan-short-reject.md)
- [SDK Package 根清单值分析](sdk-package-manifest.md)
- [Package 数字预发布号比较](package-prerelease-order.md)
- [SDK 当前模块组合验收](sdk-completion-integration.md)
- [Job 启动事务](job-start-transaction.md)
- [显式 Hold Recovery](job-hold-recovery.md)
- [结果仓夹具目录拥有与清场](result-store-fixture-owner.md)
- [Runner Release 拒绝诊断与失败原件](runner-release-diagnostic.md)
- [子历史采用册阶段诊断](child-history-stage-diagnostic.md)
- [SDK Job 共用准入与严格交单](sdk-job-admission.md)
- [Job owned 暂存注册](job-owned-registration.md)
- [Owned Job 接管与执行](job-owned-adoption.md)
- [SDK 普通 Operation 与 Job 结果边界](sdk-job-result-boundary.md)
- [SDK Job Operation 与宿主收件合同](sdk-job-operation-binding.md)
- [SDK Job 前置：命令执行限额](sdk-command-execution-limits.md)
- [中间件实际 Hook 回执保存](middleware-native-receipts.md)
- [中间件真实派发原因](middleware-dispatch-cause.md)
- [中间件共用延后效果缓冲](middleware-deferred-effects.md)
- [中间件异常出口与 Next 所见](middleware-exception-observation.md)
- [共享 Action 值校验与 Job Post 返回档](middleware-job-post-contract.md)
- [真实 Job Post 调用借用](job-post-live-invocation.md)
- [HealthBus 批次寿命夹具](agent-health-batch-fixture.md)
- [Owned Job 四场探针正文就绪](owned-job-probe-readiness.md)
- [SDK MemorySave 四场报告诊断](sdk-memory-save-diagnostic.md)
- [SDK Job 当前前置组合](sdk-job-current-integration.md)
- [SDK 订阅事件队列 SPI](sdk-event-sink-spi.md)
- [SDK Memory 片段 CAS SPI](sdk-memory-blob-spi.md)
- [SDK 主 Operation 与 turn 真实锚](sdk-operation-turn-binding.md)
- [Journal 原生追加与关闭回执](journal-native-receipts.md)
- [Windows 命令 Job 实际观测](windows-command-job-observation.md)
- [终态面板夹具与重复渲染](agent-transcript-stable-fixture.md)
- [SDK 私有组合与 PR 收口](sdk-pr-consolidation.md)
- [公开 SDK Agentic RAG 宿主参考例](sdk-agentic-rag-example.md)
- [私有 ASan 编译画像](sdk-asan-compile-profile.md)
- [Managed 会话身份合同](lubancore-managed-identity.md)
- [LubanCore 模块边界](lubancore-module-boundaries.md)
- [SDK 授权原语](lubancore-authorization-primitives.md)
- [Owned Job 排队与截止实际观察](sdk-owned-queue-observation.md)
