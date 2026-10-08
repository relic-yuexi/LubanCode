# SDK 模型采样参数与终止原因合同

2026-10-09。第 2 步自动 Memory 前置片，基线 8a19edf4（#336 三平台验收已合）。本合同先提交，再落代码；当前不记采样接口或自动 Memory 已交付。

## 公开什么

给 ModelRequest 末尾补 reasoning_effort，给 ModelReply 末尾补 stop_reason。都用 std 类型。空 effort 沿旧默认；空 stop_reason 沿旧 text/tool 推导，非空须原样传入内部 MessageDone。请求与返回值不带内部 Backend、nlohmann::json、wire transport 或 CLI 配置类型。

公开 Backend 应收到内部实际 Request 的 effort。内置 Connection 保住各协议现有映射；这片不重写四家协议。抽取器会据真实 stop_reason 拒绝截断文本。未知 provider 合法终止名只留作终止名，不能自行猜成成功、用户取消或工具执行。

两个新增字符串都限 256 字节，须为 UTF-8，不能夹 NUL。空值合法。effort 不设 provider 名称白名单，未来协议档位可直接穿过；stop_reason 也不猜别名。请求非法值在 Generate 前拒绝，回复非法值在任何 MessageStart/TextDelta/MessageDone 前拒绝，分别报 sdk.backend.invalid_reasoning_effort、sdk.backend.invalid_stop_reason。只检新增字段，不借这片改旧正文合同。

已核 src/agent/sample_model.hpp、sample_model.cpp：output_schema 归 SampleRequest，SampleModel 返回后解析正文，再用 ValidateInputAgainstSchema 复检；api::Request 没有 schema，wire 也不发 schema。这片不往 ModelRequest 塞个空转 output_schema_json。后续 Memory 复用这条本地复检路，声明 schema_ok/schema_error，不冒称 provider 原生结构化输出。将来真要公开 wire schema，另写协议能力合同，逐家实现。

本片先闭合已有内部旁路请求经过公开 Backend 时丢 effort、丢 stop_reason 两处缺口。Session 开场是否公开采样配置，须另查冻结、恢复与各协议模型档案，再定实际入口。新增值不自动开启 Memory、不替宿主创建抽取连接、不借主 Backend 并发采样。完整异步学习、候审与写回仍须后续合同和实现。

## 谁拥有

SDK ModelRequest/Reply 都持值。Backend 借请求，只在 Generate 期间访问；回复返回后由 SDK 持有。实际模型输入投影须逐字段反映 Generate 所见值，不能先投影旧窄请求、发送时才偷偷加参数。

旧模型输入 scope 保留原定义。src/api/model_input_snapshot.hpp 已写明采样参数不算输入 token；effort 不应混进正文 token 账。须查哪些回执确实记录采样参数，另定版本化审计字段，不能为了新字段无端把旧正文 scope 改名。旧会话与旧投影不能挪名、改字段或补造来源。首轮、工具继续轮、子 Agent 和旁路各沿实际请求来源核对，不能只让一份构造假的 ModelRequest 单测过门。

## 怎样关场

这片不加线程或 provider owner。取消链仍归 Session/Generate；请求和回复捕获仍在原 Close 生命周期内退休。非标准 Backend 异常、非法 UTF-8、超限参数与非法工具回复都沿现有明确错误口；检查发生在不可信值进入内部请求或 journal 前。预算按实际字节计，不借 UTF-8 字符数放宽。schema 复检沿 SampleModel 原路，不借这片改旧契约。

## 必需验收

原聚合初始化与原默认值、空工具表/含工具表、显式 max tokens 及全套原生 wrapper 仍过。补实际 Generate 所见 effort、原生内置 wire 原行为、stop_reason 与 length/max_tokens 截断传回、UTF-8/超限值、取消、Backend 标准及未知异常。实际 SampleModel 经公开 Backend 的抽取路径须覆盖本地 schema 复检，不得拿仅构造 ModelRequest 的案子顶替。模型输入正文投影与恢复仍守旧 scope；新审计字段若落账，须对照实际发送材料，明确版本与缺省。JSON 字符串不得自动拆回另一种猜测形状。

独立 SDK Lua ON/OFF、安装移位纯公开消费、完整 CLI wrapper/模型输入/抽取原案、Windows/Linux/macOS 同源 CI 才算验收。不跑本地 CI、configure、build、原生或 HTTP 验收。

## 已核实际路径

`src/sdk/adapters.cpp` 的 ConvertRequest 供 PrepareModelInput 和 send_stream 共用；两处须一起收到 effort，不能只补发送分支。现有 ProjectModelInput 只收正文、消息和工具，模型名与输出上限本就排除。`tests/integration/sdk/test_lubancore_model_input.cpp` 已逐项核 Generate 实参与投影，新增 effort 断言沿这份实际适配器测试补，不另造假协议。

回复目前在 send_stream 中按工具表硬推 end_turn/tool_use。新增非空 stop_reason 要先验，再交 MessageDone。验收须让合法 JSON 正文带 length、max_tokens、max_output_tokens，穿过 SampleModel 后交 FinishMemoryExtraction；三者都须归 OutputTruncated，不能入库。未知或空终止名沿原解析规矩。原 `tests/unit/memory/test_memory_extract.cpp` 和 `tests/unit/app/test_turn_memory_extractor.cpp` 一并保住。

另有已核欠账：SampleModel 的预算看门狗创建与异常退场尚未兜齐，CLI 异步抽取线程也缺异常出口。这片不加后台入口，不拿 Session 开场验收抵旁路账。自动 Memory 启用前，须另交看门狗创建失败、异常收场与线程体退场证据。

