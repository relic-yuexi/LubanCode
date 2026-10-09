# 旁路采样看门狗关场合同

2026-10-09。第 2 步自动 Memory 前置。本合同先提交，再落代码；已随 #338 合入 `068db5d3`，与验收源 `639a9dc2` 同树。三平台原件已验，见[阶段进度](sdk-stage-status.md)。自动 Memory 尚未交付。

## 公开什么

公开 SDK 形状暂不扩。复用 agent::SampleModel，补预算看门狗创建失败与 Backend 标准/未知异常出口。失败仍返回 SampleResult；稳定 api_code 区分线程启动失败、Backend 异常与原取消、deadline。自动 Memory 不得借这片提前开启。

原 SampleRequest、SampleOptions、SampleResult、输出 schema 复检、usage 与调用方解析入口照留。原生 wire、预算数值、100 ms 轮询及外部取消归因照留。未知异常给固定诊断；错误不能冒充用户按键、超时或成功。

错误码：`sample.watchdog_start_failed`、`sample.backend_exception`、`sample.backend_unknown_exception`。标准与未知线程创建异常都归启动失败，正文保留各自诊断；Backend 未返回前抛异常，按标准或未知两档收尾。返回过的 api::Error 不重写。覆盖 `ForceMaxOutputTokensOverride`、`GetEffectiveOutputLimit` 与 `send_stream` 三口。输出帽检查抛错时尚未过 prepared，不补造请求账；send_stream 抛错时沿已准入请求收 usage 与失败终态。recorder 自身、内存分配或本地 schema 校验未获新增“不抛”保证；头注释须按这份边界写准，不宣称整个函数 noexcept。

现有 SampleOptions::cancel 注释还称外部取消与预算不合并，已与实现相反。实施时一并改准：仅外部旗不建线程，单预算读本地旗，双取消读合并旗。不得借修注释改取消裁决。

## 谁拥有

SampleModel 栈持原取消旗、done、cancel_fired、assembler 与 watchdog。Backend、外部取消旗和 boundary_recorder 只借到本次调用退场。作用域收场器在 watchdog 起前装好；任何分支离场，先拉 done，收线程，再退栈中捕获与借用。禁止 detach。不暴露宿主自定线程工厂，也不拉 app 或 CLI 目标进 SDK。

创建故障须打在真实 watchdog 的 std::thread 创建边界，单预算和双取消两形都覆盖。私有测试缝不安装、不进入普通模块；来源与实际运行实例须核对，不能拿测试程序另一份槽证明 DLL 路径。

已核编译归属：`src/agent/sample_model.cpp` 归静态 `lubancode_engine`，SDK 经 `lubancode_runtime` 私下链接它。CLI 测试程序和 SDK DLL 各有一份引擎；测试程序直接调用 SampleModel，只能证明程序那份。正式实施须分别核两处：CLI 原调用口用程序内真实引擎；SDK 私有测试桥在 DLL 内调用所链接的 SampleModel，设故障槽与采样必须落在同一模块。桥只随根测试开关编入，不安装头，不出口普通产物。不能把 SDK 专项里一次静态引擎调用算成 DLL 实例验收。

## 账怎么收

已核现有顺序：输出帽检查 → started → prepared → sent 耐久门 → 创建 watchdog → send_stream → join → usage/output 终态。sent 属发送前耐久准入，不能借这片改旧 permit 或 attempted 口径。线程失手发生在耐久门之后，仍须明确失败收尾，证明 Backend 调用为零；不抹既存 sent，不声称已有网络请求，也不造 provider usage。prepared/sent 未确认时沿原硬闸，不能发模型。

Backend 发过半截文本、usage 后抛异常，保留已收到材料与账，输出归失败；不丢半截，不造完整成功。已收到 MessageDone 后的异常也须留明确异常结果；原“完整返回与取消同场”裁决只管既有 Cancelled 路径，不套给任意异常。同一请求只收一笔终态，真实 recorder 失败仍沿现有写账门。

## 怎样关场

正常、错误、标准/未知异常、创建失败、prepared/sent 拒绝及取消都走同一退场规矩。watchdog 收讫后才返回，线程体不再借调用栈或 recorder。Backend 若不响应取消，调用仍受既有同步契约约束；不能用 detach 抹掉 owner。CLI TurnMemoryExtractor 工作线程的异常出口另随实际共享接入补齐，须报失败、立 done、供主线程 join，不能仅改一条注释就说完整异步路已成。

## 必需验收

保留 SampleModel 原案例及 CLI 六条采样调用口。新增真实单/双看门狗创建失败、重复失败后正常、调用线程隔离、标准/未知 Backend 异常（起前与半截后）、实际 recorder 终态、无 provider usage、创建失败零 Backend 调用、取消与完成竞态、外部取消借用退场、原预算与输出上限。测试须观察真正线程收讫，不能只看句柄空或计数归零。

SDK Lua ON/OFF、移位安装纯公开消费、全 CLI、ASan 与 Windows/Linux/macOS 同源远端证据齐了才交付。不跑本地 CI、configure、build、原生或 HTTP 验收。
