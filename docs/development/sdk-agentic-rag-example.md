# 公开 SDK Agentic RAG 参考例合同

基线 `4da222e358f982a220422a268fa395ab0011001f`，分支 `codex/sdk-agentic-rag-example`。先收合同，再写示例与验收。架构单第九节在 2026-10-01 明列此例为近期目标；它不是 #332 当前收口门，也不把 SDK 完整化其余缺口勾完。

## 公开什么

只交 `examples/` 参考例，不增 Core、检索 SPI 或 C++ ABI。宿主以公开 `Tool` 交检索器，模型经真实 SDK 工具回环取证、补查，再给出带来源的答案。模型适配器只出调用；验收答案必须取实际工具回复，不准把预写答案当检索证据。参考例用有限本地文字资料，无向量库、embedding、网络知识库或真实 ACL 保证。替换检索后端仍归宿主。

共用实现为 `examples/sdk-consumer/agentic_rag.cpp`，只含安装后的 LubanCore 头与 STL。入口：

- `void lubancore_consumer::AgenticRag(const std::filesystem::path& base)`：依次跑六条验收，最后印完成标记。
- `void lubancore_consumer::AgenticRagCase(const std::string& name, const std::filesystem::path& base)`：跑一条真实路径。
- `int lubancore_consumer::AgenticRagDemo(int argc, char** argv)`：参考程序入口；显式给数据根、资源根、项目根与模型连接，或选择明标的本地模型夹具。

检索器持有有限、按值资料。每份资料有稳定 ID、来源与正文；命中返回 owned 证据与来源。模型只看检索返回文本。参考协议明确列查询 JSON 的字节帽、字段和结果帽；坏输入返错误，不补猜查询，不改资料。引用只能指这次实际返回的来源；示例显示答案与证据，不宣称能阻止任意真实模型幻觉。

## 谁拥有，怎样关场

宿主造检索器；`Tool::execute` 捕获其共享 owner。SDK 持该工具到实际在途调用退完，`Close` 取消、等回调退出，再销工具和模型。检索器不留 `Session`、Writer、路径柄或取消旗借用。取消旗只在当前调用内读。参考检索器合作取消，不强杀 C++ 线程。

每场显式交自己的资料、模型与 cwd。同目录可开两场；本例只验证宿主注入与会话隔离，不称租户授权。同 ID 恢复先交相同宿主检索器，再核真实旧消息、调用与回复；不重放旧检索。任意宿主工具配置尚无通用冻结计划，本例不声称 SDK 能验证检索器身份或恢复外部服务状态。

不设置 `SessionOptions.result_policy`，沿默认 Preview/v1。示例向外展示检索证据也须经过已核保存材料与公开 `ResultProjector`，Node 不许 full。可信本地保存结果与模型上下文不因此删减，preview 不是资料授权。

## 验收与编译归属

新原生来源 `tests/integration/sdk/test_lubancore_agentic_rag.cpp` 固定六 CASE；实际路径末标 `[sdk-agentic-rag-path]` 各一次：`retrieval`、`sources`、`preview`、`isolation`、`recovery`、`lifetime`。安装消费者走同份公开 SDK/STL 实现，六路完成后印 `[sdk-agentic-rag-consumer] complete`。

六路分别核：模型至少两次实际查询与结果驱动续查；实际 owned 证据、来源和坏查询；默认 preview 投影与 full 拒绝；同项目两场重叠、资料/模型/审批/取消不串；Close 后同 ID 真实历史、旧检索零重放；在途检索合作取消与关场、最后 owner 退尽。实际检索与回环走库内运行栈；确定模型夹具仅控制查询顺序，不验真实 LLM 的检索质量。

参考程序为 `examples/sdk-rag/{main.cpp,README.md,CMakeLists.txt}`。源码树外构建要复制这三件和共用 `agentic_rag.cpp`，只 `find_package(LubanCore)`；安装消费者复制自己的完整目录，不借仓库源码。六案与参考程序可在 Lua ON/OFF 画像运行；不调用 Lua Hook。

本作者只写这份合同与五件新源码/说明。Root 管现有 CMake、CI、目录、consumer main 路由及闭包门。公开头、Core、结果/恢复代码、四件 Todo 与二十件 Lua 封件不动。三平台原生与安装迁位验收只交远端 CI；本地只查源码、Git 与 Python 纯数据。不 push，不添 PR，不借旧绿。
