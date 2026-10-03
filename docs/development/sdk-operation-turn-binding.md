# SDK 主 Operation 与 turn：真实锚前置

本笔基 `bfaf7795ef8a9e6ce3cb152475cd9c033d0d2935`，私有分支 `codex/sdk-operation-turn-binding`；旧 `codex/sdk-job-operation` 留在原头。先提交本合同，再写代码。公开 Jobs、Job Operation/registry/dispatcher、Close、父取消与许可域都不在本笔。本地不 configure、编译、CTest 或运行原生程序，也不下载 GitHub 原件。

## 只开内部 producer 门

冻结新事实 `sdk.operation.turn.bound`，payload 布局 `sdk_main_operation_turn_v1`、版本 1。它是 statusless 原生事实，必带真实 turnId；不造 step/action/request 身份。payload 只含 layout/version、实际 operationId/inputId/payloadHash；SID/run/seq/eventId/lineHash 沿真实 Writer。payloadHash 是原受理输入规范载荷 SHA，不冒称最终 prompt 或日志全文摘要。

入口暂名 `BeginMainOperationTurn(SessionService&)`，由内部 SDK 模块提供。它只取这份 Service 当前实际队首：真实 Pop 一次，核本账实际 accepted/dispatched，再让同份 Service Writer 分配 NewTurnId，随后 PowerLoss append。调用方不能传 operation/input/turn DTO，不能挑同 run 旧锚。核实际 queued 原文及图片的共用 `SessionService::CanonicalInputPayload` 摘要，不能复制规范化器。

当前主 SDK `Run` 与 CLI 不调用此门，不添公开 enable 选项或 TLS 开关；默认零新事实。以后真正 SDK Job 装配才显式迁入该共用门。原 Session/Agent/Backend 资源图照留，不新造一套执行栈。新六册显式走真实 Service/Writer/已有 Agent 执行路径，不宣称公开 SDK 已接。

## 真实窗口与首次回执

结果保存实际被取输入、分配 turn、首 native receipt（若真有）及独立 knowledge。至少区分：未取输入；输入已 dispatched/被取但尚无锚；append 未确认；锚已 Committed 而 owned 发布失败；绑定与发布完成。Rejected+broken 仍未确认，不能只看 receipt enum 宣称零写。

锚落稳、owned 发布完整，才准 caller 进入真实模型/工具。失败保原 error/receipt/material，不调主轮 Complete 补 final、不重写锚、不换 turn/op，也不重排已取输入。后续新输入走自己的真实新 turn。可用内部 publisher 接收真实 immutable facts；其异常只表示提交后发布 gap，不伪称 Writer I/O 失败，不授权限。

旧 received/dispatched 与锚不是跨文件原子事务。Pop 后、锚前崩溃是合法前缀：输入已取、执行尚未开始；材料不足便 Unknown/Incomplete。读到成功行不能反签首次 append 返回确认。历史锚只作可校材料，不给 live lease，不触模型、工具、repair 或重新派工。

## 一份共用校准

V3 envelope enum/字符串/statusless、C++ schema 与 Python validator 同笔加精确 shape。V3 Reader 实际读取入口调用共用锚校准：版本/ID/SHA、同 SID/run、turn 与 operation 各唯一、锚先于本 turn 已存消息/请求/工具事实。旧无锚账照旧可读；没有锚不能拿最近 operation 或消息角色补关系。

SDK strict 读面再用原 operations.jsonl 校验器产出的同份 owned accepted/dispatched/final 投影。核 operationId/inputId/payloadHash、确已派工及后来 main final 的 turn。主 final 仍一 turn 一 op；不把新行塞进旧 operations.jsonl，也不改主结果索引。V3 层不反向 include SDK，不复制 schema/hash parser。严格关系失败须给 `sdk.resume.operation_turn_invalid`；原 operations 错误保原码。

strict helper 是本笔内部入口；默认公开恢复行为及未来冻结选项装配不冒称已完成。新事实的原生 shape 与跨行校准确须进入实际 V3 Reader，不能只留未调用 helper。关系材料分 NotApplicable/Incomplete/Rejected/Validated；Validated 只证历史关系，不证现场确认或执行成功。

## 六路原生验收

固定新来源 `tests/integration/sdk/test_lubancore_operation_turn_binding.cpp`，六 CASE，各案最后独立 `[sdk-operation-turn-binding-path]` marker：`actual`、`source`、`gap`、`history`、`relation`、`isolation`。最终以真实源码定名/计数，根代理接 CMake、focused、ASan 与精确门。

真实输入受理/派工→同 Writer 分号→native 锚→原 Agent/Backend/工具路径是正路。另核缺/错源零 append、真实 Writer Rejected+broken、已提交 publisher 异常零 Generate/工具、Close/Continue 合法前缀只读不重派、重哈希合法 shape 但错 accepted/final 关系精准拒、同项目两场加异项目两场归属。四场允许本场相同 op/input/run/turn 字串，须按真实场目录与完整原生材料核值，不能拿字符串不等当隔离证据。

原 source、CASE、预算、主 final/普通结果与所有旧断言保留。不拿源码计数代远端实跑；新鲜 source/tree/checkout、完整 argv、非零断言、JUnit/LastTest 与 marker 另收，旧失败 seal 不动。
