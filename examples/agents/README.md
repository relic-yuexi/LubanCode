# Agent 定义示例(散装)

这目录如今放**散装 Agent 定义**的示例:一个 Agent 一份 YAML,单文件,
不套二层目录,不收藏脚本与资料。契约全文见 `docs/reference/agents.md`。

| 示例 | 讲什么 |
| --- | --- |
| [code-reviewer.yaml](code-reviewer.yaml) | 只读评审员:tools.allow 收白名单、requires 断言、runtime 逐项取舍,各演示一遍 |

要**整包能力**的示例——Skill、Plugin、MCP 打包分发,带清单与信任门——
去 [examples/packages/](../packages/README.md)。GUI 与 Browser 两只旧示例
已改造成 Package,分别住在那边:

| 旧示例 | 去处 |
| --- | --- |
| gui-agent(process 插件,Windows 桌面) | [../packages/gui-agent/](../packages/gui-agent/README.md) |
| browser-agent(Skill + 练习页,配 browser MCP) | [../packages/browser/](../packages/browser/README.md)(阶段 5 归拢为官方包 luban.browser,添了 Agent 与四只 Workflow) |

## 装一只试试

```text
拷 code-reviewer.yaml 到 ~/.lubancode/agents/(用户级)
或 <项目>/.lubancode/agents/(项目级,优先级更高)
```

重启 LubanCode 后:

```text
/agents                        → 清单里应见 code-reviewer [user] 可用
/agent doctor code-reviewer    → 静态预检:定义、工具引用、runtime 逐项过
```

派活走 `agent` 工具,类型名即 `name`(code-reviewer);description 那句
就是主 Agent 派活的依据,写清楚何时出场。

## 写自己的 Agent,几条捷径

1. 最小定义三行:`schema: 1`、`name`(小写 kebab-case)、`description`
   (1 到 1024 字符,写何时派它,不写宣传话)。
2. 要只读:不用另造权限档,`tools.allow` 列白名单就行(本示例即此做法)。
3. `allow` 是过滤,`requires.tools` 是断言;两份名单与 `tools.deny`
   交叠的规矩见契约第 4 节。
4. 文件名须与 `name` 一致(去 `.yaml`),不一致给 warning。
5. 稳定提示词归 Prompt Profile,可复用做法归 Skill——Agent 文件里
   不收 Markdown 正文,也不内联 MCP 启动命令。
