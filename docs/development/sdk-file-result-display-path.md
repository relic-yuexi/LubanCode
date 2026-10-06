# File 结果显示路径兼容

旧预览重建先拼 File 路径，再调用 `lexically_normal()`，最后转 UTF-8。
新 NamedResultCapability 的 File 分支漏了中间一步。
Windows 拼入 `artifacts/res-*` 后可能留两种分隔符；历史子 Agent
验收仍走旧拼法，渲染预览时会逐字比错。POSIX 看不出这处差别。

这笔只给 File 显示路径补回旧规范化。外部仓仍返回原
`host-result://<session>/<artifact>`，不改逻辑名、读写路径、引用、
存储身份、预览算法或准入规则。

原私有 File 验收内另存一份真实大结果，用实际引用分别走实时
capability 和旧历史重建，再比路径及完整渲染文本。两份都须真截断，
不能拿短文本避开路径。外部仓也核实际引用对应那条原 URI。
原八 CASE、路径见证和预算照留。

源码可证这处兼容缺口；本轮 Windows 子场失败尚缺下层错误和主账，
不能称它唯一根因。失败诊断仍保留。修正和验收都交新源远端 CI，
本地不配置、不编译、不跑原生程序。
