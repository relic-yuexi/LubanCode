# Lua Hook 示例

每只目录是一枚完整 hook 包(`hook.json` + `main.lua` + `fixtures/`),拷走改
名即可用。校验与试跑:

```bash
lubancode hook test examples/hooks/prompt-clean
```

fixtures 用真实 Lua runtime 跑,HTTP/文件/工具走 fake adapter——零网络零
业务文件;真实集成要装进会话沿既有授权观察(报告会明列未验项)。

| 目录 | 挂点 | 演示什么 |
| --- | --- | --- |
| [prompt-clean/](prompt-clean/) | PreUser | 改写候选:清洗首尾空白,经 `next(candidate)` 提交宿主校验 |
| [business-deny/](business-deny/) | PreUser | 业务拒绝:`ctx.deny` 短路,本轮不接纳,后续项记 skipped |
| [http-recall/](http-recall/) | PostUser | 受控 HTTP 召回:`luban.http.request` 取材料,`luban.context.append` 提交隐藏上下文 |
| [postaction-refine/](postaction-refine/) | PostAction | 结果加工:`result.supplement`/`result.filter` 效果加工已保存的工具结果 |
| [replace-estimator/](replace-estimator/) | PreRequest/estimate | 同名替换内置槽位:压过 `context.token_estimate`,阶段边界不动 |
| [mcp-knowledge-recall/](mcp-knowledge-recall/) | PostUser | MCP 组合:Lua 调已注册知识库工具,子执行留账,取消/结果未知分支齐全 |

三册手册(挂点 / 中间件配置 / Host API)见 `docs/features/hooks/`。
让 LubanCode 代写 hook 的官方指引见 `skills/lubancode-hook-authoring/`。
