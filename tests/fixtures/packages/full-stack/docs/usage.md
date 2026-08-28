# 用法

## 跑一趟冒烟验收

```text
/workflow run smoke-test --html "<页面源码>" [--url https://example.com/page]
```

collect 节点调 Plugin 的 `inspect` 采摘要;verify 节点唤起同包 Agent `browser-tester`,
照 `assets/smoke-checklist.md` 核对;结果落在 `result.verdict`。

## 单独派验收员

```text
让 browser-tester 验收这份页面源码: <粘贴 HTML>
```

Agent 预装 `browser-testing` Skill,工具收窄到摘要、导航、截图三件,禁 shell。
它给证据,不下空结论。

## 手动走一遍工具

```text
/plugin inspect moontide.full-stack:dom-analyzer
/mcp
```

`inspect` 喂 HTML 出摘要;MCP 的 `navigate` 记账翻 generation,`screenshot` 须带
最新 generation,旧了报 `stale_ref`——这是设计,不是故障。

## 排错

- 工具不见:先 `/package doctor moontide.full-stack`,再看信任状态;项目包未信任时
  代码组件不注册。
- `agent: browser-tester` 解析失败:自定义 Agent 单落地前,Workflow 的 `agent:` 字段
  尚未生效,属已知未接线,非本包错。
- 中文摘要变问号:Windows 管道代码页问题,runner.py 已自 reconfigure;若仍现,查 Python 版本。
