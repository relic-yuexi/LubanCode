# 开发手册

[文档首页](../README.md) · [架构说明](../architecture/README.md) · [参考手册](../reference/README.md)

- [构建与发行](build-and-release.md)
- [测试指南](testing.md)
- [文档规范](documentation.md)
- [命名与计数](naming.md)
- [安全模型](security.md)
- [界面多语言](i18n.md)

改用户可见行为时，先查[文档同步矩阵](documentation.md#10-改动同步矩阵)。
提交前再跑：

```powershell
bash scripts/check_docs.sh
git diff --check
```
