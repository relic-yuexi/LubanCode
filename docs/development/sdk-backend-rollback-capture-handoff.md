# Backend 回滚测试的捕获交接

2026-10-10。#346 修订源 `de6072a6` 已在 macOS 执行生命周期十九案。
十八案过，一案失败；六处断言都指向工具捕获尚未退场。
原日志 694315 字节，SHA256 为
`094d21c012d8dbfb7f6cc6c991a350916639644ace9fd4fa041418b71f2d06db`。
上一轮 TLS 链接错误没有再出现，本轮失败另存，不能把整片算通过。

公开什么：本片只修故障注入测试的交接，不改公开接口或生产代码。
测试把 Tool 移入 SessionOptions 后，显式清空局部 Tool 的 execute。
移动后的 std::function 仍是有效对象，不能假定所有标准库都会清空它。

谁拥有：进入 OpenSession 前，工具捕获只归 SessionOptions。
局部测试工具不能留第二份捕获，再要求 SDK 提前销毁它。
原有 SDK SourceScope、Backend anchor、分配失败及观察口异常都照旧。

怎样关场：局部回调先清空，再交 SessionOptions。
回滚仍须先退 SDK 捕获、后退 Backend；析构重入仍须返回
`sdk.lifecycle.reentrant`。不放宽六处失败断言，不减十九案、七枚标记。

怎么验：新源独立跑三平台全量、SDK ON/OFF、九套安装消费和 ASan。
保留原断言、故障注入和预算；旧源局部通过不借用。禁止本地 CI。
