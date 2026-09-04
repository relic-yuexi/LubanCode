# 实验 A:工具检索退化阈值(P0 装置)

单子 §三。五档工具表(总 12/22/40/70/120,核心 12 直挂余延迟)× 三模式(disabled 全量直挂 / legacy_expand / proxy_reference)× 三任务(T1 核内单步 / T2 延迟工具单步 / T3 先不中再搜到再调)× 5 次,固定假 backend。记账:首 token 延迟、`tools`/`system`/总请求字节、模型决策正确率。

## P0 范围(已通)

只跑 **A2 档 × disabled × T1**:装置转得动、账落得下。

- 工具表生成器最小版:40 只 = 真 `read_file` + 11 只合成核心 + 28 只合成延迟(`SyntheticTool`,`deferred()` 为 true——disabled 下它们照样直挂,这正是"全量直挂"的题义;装配层不设 `tool_filter`,`Agent::BuildToolDefinitions` 全量进 `Request.tools`)。
- T1:假 backend 两步脚本(第一步 `ToolUseScript` 调 read_file 读探针文件 → 第二步文本收口),`read_file` 是真执行。
- 5 次重复,一行一 JSONL:`results/raw_a2_disabled_t1.jsonl`。
- collect:`summary_a2_disabled_t1.{csv,md}`(pandas/pyarrow 齐时 `.parquet`)。

## 跑法

ctest(在 eval 构建树):`ctest -L eval.tool_search_threshold`。手动:

```sh
build/eval/.../eval_tool_search_threshold [输出目录]      # 缺省落源码树 results/
python tests/eval/tool_search_threshold/collect.py --raw results/raw_a2_disabled_t1.jsonl --out-dir results
```

## 口径

- 首 token 延迟 = 假 backend `send_stream` 进入到首个事件回调的毫秒数——量的是宿主组装与回调路径,不是真网络;P2 报告注明。
- 字节 = `api::Request` 中立投影的 JSON dump 长度(driver 里 `MeasureRequest`);四家 wire 各有方言,档间可比靠同一投影。
- 决策正确率 = T1 第一步调对 read_file 且 `tool_result` 非 error 的占比;零分母记 `unavailable`。
- `should_defer_at_default_threshold` 随账落盘:A2=40 只按默认阈值 20 本该启用延迟;disabled 模式即装配层不做延迟,40 只全直挂——P2 对三模式时口径可复算。

## P2 扩展点

五档生成器参数化(`BuildToolTable(core, deferred)` 已参数化)、三模式(legacy/proxy 要接装配层 tool_filter 与 ToolSearchTool 两副构造)、T2/T3 任务脚本。阈值结论(任一账变差 ≥30% 即给)在 P2 报告 `docs/evaluation/tool_search_threshold.md`。
