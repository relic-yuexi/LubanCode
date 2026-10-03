# SDK MemorySave：四场真实报告诊断

基 #310 `e210e2f4`。Windows combined job `111195389522` 实际跑原 MemorySave 十二场，十一过、一败。scene-1 报告 `memory_id` 空，随后按报告空路径读文件失败。原 log SHA256 `ea867141a665b6e0ce507087f31cc713b236a2d6c2443d3ea1281e46aeac87c7` 另封。它没有打印实际报告状态、错误码与阶段；主 Operation 成功、error 空，不能代替单次写入提交确认。本笔不推断 Busy、负载或操作系统为唯一原因。

只改 `tests/integration/sdk/test_lubancore_memory_save.cpp` 四场 CASE。各场真实 WaitResult/GetMemorySaves 已返回后，在报告数量和 Memory 断言前打印已持有值：scene ordinal、宿主 SID、Receipt operation、cwd、主 Operation 完整状态、模型调用数，以及公开 SaveReport 全部元数据和 stage/outcome。目标路径须注明由哪份值派生；未查盘就不说文件存在。报告引用 ID 不是原生 WriteReceipt，不补 seq/hash/status，也不把已引用说成已提交。

报告向量不是一份时也打印实际向量，再执行原 REQUIRE。诊断只吃返回值，不 Pump、不再查询报告、不加文件读取，不改 gate 或线程调度。打印函数不抛；分配、编码或输出失败另留诊断失败行，原异常与断言继续走原路。失败可继续保留未知态；不靠诊断补成功，不重试副作用。

原十二 CASE、全部 CHECK/REQUIRE、同项目二场与异项目二场、三十秒 WaitResult、原取消/Close 路和生产、CI 名册均保全。静核须从新源剥除诊断增量，恢复基源逐字相同；本地只查文本/AST/文档，不配置、编译、CTest 或运行原生。新头远端再收完整原件；旧失败账不覆盖。
