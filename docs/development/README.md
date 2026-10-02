# 开发手册

[文档首页](../README.md) · [架构说明](../architecture/README.md) · [参考手册](../reference/README.md)

- [构建与发行](build-and-release.md)
- [LubanCore C++ SDK（实验版）](lubancore-sdk.md)
- [SDK 显式 Skills 合同](sdk-skills.md)
- [SDK 项目 Memory 只读召回合同](sdk-memory-recall.md)
- [项目 Memory 提交与回执合同](memory-project-commit.md)
- [SDK 项目 Memory 正式保存合同](sdk-memory-save.md)
- [SDK 子 Agent 首批合同](sdk-subagents.md)
- [子代理终态回执合同](child-terminal-receipt.md)
- [Runner 就绪探针与失败原件](runner-probe-evidence.md)
- [后台子代理线程启动收口](agent-thread-start-cleanup.md)
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
