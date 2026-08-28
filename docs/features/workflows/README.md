# Workflow

[文档首页](../../README.md) · [设计指南](designing-workflows.md) · [Schema 参考](../../reference/workflow-schema.md) · [运行时](../../architecture/workflow-runtime.md)

Workflow 把一件反复要办、步骤与收口规矩都说得清的差事，装成一张可校验、
可记账、可恢复的图。本页管安装与运行。怎样选 Node、Batch、Parallel、Async
等原语，另见[设计指南](designing-workflows.md)。

## 安装位置

一只 workflow 是一只目录，根下至少有 `workflow.yaml`。prompt、模板与 agent
任务文件跟着它走，不得用相对路径越出包目录。

```text
<project>/.lubancode/workflows/<id>/     项目级
~/.lubancode/workflows/<id>/             用户级
```

项目级压过同 id 的用户级定义。将示例装进当前项目，可在仓库根目录运行：

```powershell
Copy-Item -Recurse examples\workflows\sansheng-liubu .lubancode\workflows\
```

若目标目录已经存在，先看内容，不要直接覆盖。

## 验明再跑

```text
/workflow list project
/workflow show sansheng-liubu
/workflow graph sansheng-liubu ascii
/workflow validate sansheng-liubu
/workflow run sansheng-liubu --requirement="给README补一段安装说明"
```

定义若写了 `alias: sansheng-liubu`，也可直呼：

```text
/sansheng-liubu 给README补一段安装说明
```

`/workflow create <描述>` 眼下只打印向导说明。自然语言编译器已有宿主骨架，
模型提取与确认安装尚未接进这条命令。现阶段可让 LubanCode 使用官方
`lubancode-workflow` Skill 设计并写出包，再用 `validate` 守门。

## 运行账

有 `runs_root` 的运行会把账落进：

```text
~/.lubancode/workflow-runs/<run-id>/
  manifest.json
  definition.json
  events.jsonl
  checkpoints/<seq>.json
```

`definition.json` 留当时的归一化定义。`events.jsonl` 只追加。checkpoint 保存
Store 快照。恢复时以内容 hash、已完成节点与最新完整 checkpoint 为准，不捡半份
输出。

## 出错先查

```text
/workflow doctor
/workflow validate <id>
/workflow graph <id> ascii
/workflow history <id>
```

- `找不到 workflow`：检查目录层级，`workflow.yaml` 应直接住在 `<id>/` 下。
- alias 不响应：查 `/workflow alias`；撞内建命令、Skill 或另一份 alias 时，直呼会被禁用。
- `invalid_definition`：按错误给出的字段路径改图，不要绕过 validator 直接跑。
- `not_configured`：节点种类存在，宿主却没装对应 executor 或 Broker；这不是重试能治的错。agent/tool 节点的审批不在此列——已接终端确认链。
- `budget_exhausted`：查 `max_steps`、总时限、tool_calls 与 tokens；loop body 的每一步也吃总账。

Workflow 能调用工具、模型、子代理与交互 Broker，也能写工作区。权限仍由原工具、
Hook 与确认机制守门；workflow 不替用户扩大权限。agent 与 tool 节点的审批已接
终端确认链，写盘、执行一类动作照旧过御前。副作用节点若会重试、循环、异步
重放，须给稳定 `idempotency_key`。
