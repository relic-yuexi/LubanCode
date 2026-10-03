# 开发手册

[文档首页](../README.md) · [架构说明](../architecture/README.md) · [参考手册](../reference/README.md)

- [构建与发行](build-and-release.md)
- [LubanCore C++ SDK（实验版）](lubancore-sdk.md)
- [SDK 显式 Skills 合同](sdk-skills.md)
- [SDK 项目 Memory 只读召回合同](sdk-memory-recall.md)
- [项目 Memory 提交与回执合同](memory-project-commit.md)
- [Memory worker 争锁与更新失败诊断](memory-worker-contention-diagnostics.md)
- [Memory worker 有界争锁验收](memory-worker-busy-acceptance.md)
- [SDK 项目 Memory 正式保存合同](sdk-memory-save.md)
- [SDK 子 Agent 首批合同](sdk-subagents.md)
- [子代理终态回执合同](child-terminal-receipt.md)
- [父 V3 子终态观察与未知止损](child-parent-observation.md)
- [子结果严格历史采用合同](child-history-adoption.md)
- [Runner 就绪探针与失败原件](runner-probe-evidence.md)
- [后台子代理线程启动收口](agent-thread-start-cleanup.md)
- [前台子 Agent 本次调用上下文](child-foreground-context.md)
- [可取消审批与逐票 lease 合同](scoped-approval-lease.md)
- [前台子轮异步审批接线合同](child-async-approval.md)
- [SDK 依赖瘦身：更新器归宿主](sdk-updater-boundary.md)
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
- [SDK 前台子 Agent 装配](sdk-subagent-assembly.md)
- [结果仓 Windows 原生路径入口](result-store-windows-io.md)
- [SDK Memory 写计划失败诊断](sdk-plan-atomic-diagnostic.md)
