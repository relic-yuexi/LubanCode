# 故障边界复现测试

[测试规范](../../README.md) · [故障结论与源码分析](../../../interview/failure-and-recovery.md)

`test_failure_boundary_audit.cpp` 运行真实 AgentLoop、V3 写桥、结果仓和恢复投影。模型用内存假后端，工具只计数并返回固定内容。故障由 writer 注入回调或临时路径冲突触发，不访问线上服务，不模拟真实断电。

## 两种运行方式

默认模式检查当前表现。FA-01/FA-02/FA-03 已于 2026-09-11(P1 批)按目标断言修复,现为默认回归;FA-04/FA-05 仍是已知缺陷,默认模式钉当前表现,通过只说明复现一致。目标模式(`LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED=1`)检查修复后应有行为;FA-04/FA-05 在该模式下应报失败,不使用 skip、should_fail 或忽略退出码把它染绿。

```powershell
cmake --build --preset release --target lubancode_tests -j 2
ctest --test-dir build/release -C Release -R '^integration.runtime.failure_boundary_audit$' --output-on-failure
```

测试自己临时启用 v3，退出时还原此前环境值；不依赖 CTest 默认使用 v2 还是 v3。

运行修复目标断言，并在结束后还原开关：

```powershell
$previousAuditMode = $env:LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED
try {
    $env:LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED = '1'
    & ./build/release/tests/Release/lubancode_tests.exe '--source-file=*test_failure_boundary_audit.cpp' '--no-colors'
    # 读进程退出码：当前已知缺陷应使它非零；不是构建失败。
    $auditExitCode = $LASTEXITCODE
} finally {
    $env:LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED = $previousAuditMode
}
$auditExitCode
```

只复核一项时，给 doctest 再加 `--test-case=*FA-02*` 等过滤条件。FA-04 与 FA-05 共用一次断流场景。修复落地后，把对应目标断言改成默认回归，移除该项旧表现断言；全部销项后删除双模式开关。

## 审查清单

| 编号 | 注入与观察 | 目标断言及边界 |
| --- | --- | --- |
| FA-01(已修 2026-09-11) | 第一次工具执行时，把结果仓目录换成普通文件；观察下一次发送、磁盘 tool 消息和恢复工作 | 仓未保存时停止后续发送(回执翻 Failed,内存 history 撤回到持久边界),恢复投影以 `result_missing` 列出缺口并保留执行终态;现为默认回归。若另定可靠补存后继续的合同，须同步验证实发/恢复等价，不能仅改调用次数 |
| FA-02(已修 2026-09-11) | 工具返回 `is_error=true`，比较实发与 `ProjectV3ContextHistory` | 错误标记随最终 tool 消息本体落档、投影原样还原;现为默认回归 |
| FA-03(已修 2026-09-11) | 分别在 prepared/sent 写入处注入失败，并核对实际诊断名称 | 两个点都不得调用 backend(sent 经 `OnRequestSent` 回 false 贯通到 `run_one_attempt`);现为默认回归 |
| FA-04 | 第一请求输出部分正文后断网，第二请求成功 | 每个 `requestId` 恰有一个响应终态；不能只核总计数 |
| FA-05 | 同一断流场景接 `TurnEventAdapter`，保留主路当前 wiring | history 为 SUCCESS；当前目标要求录音器正文也为 SUCCESS。若采用失效 item/撤回协议，须扩展录音器到最终可见投影并补 UI 测试，不能把原始 delta 拼接当最终显示 |

还有两类对照：完整 assistant 输出提交失败后工具不得执行；第二份 metadata 保存失败时，主账仍保留结果。后者没有复现“结果全丢”，继续还是停止须另定降级合同，两种模式均保留其观察断言，不硬塞到 FA-01 的修复目标。

writer 注入按本场后续提交次数选择，但测试还核对诊断指向 prepared/sent/completed，防止新增事件后悄悄测错位置。测试改变时若诊断不符，应修夹具，不能顺手更改生产结论。

## 原始产物不入库

每场测试原子创建独立临时目录，关闭 bridge/writer 后清理。设置 `LUBANCODE_FAILURE_AUDIT_KEEP=1` 可保留，输出 `FAILURE_AUDIT` JSON 会给出账本路径；默认不输出已删除路径。需要留报告时，放仓库 `.local-audits/`，该目录已忽略。

2026-09-11 那批旧日志和账本已移到本地 `.local-audits/failure-boundaries/2026-09-11/`。测试不读取它们，克隆仓库无需补下载。Git 保存源码与复现办法，不保存这批运行结果。

## 修复意见该回答什么

每项意见写明：失败发生在哪道提交边界、是否允许继续、恢复时怎样识别、是否会重跑副作用，以及目标断言该怎样调整。已有行为、候选修法和已验证修复分开写。

这些测试覆盖受控注入与事件适配层。真实磁盘写满、断电、强杀、终端画面和真实 provider 行为仍需另测。

## 本轮复跑记录（2026-09-11）

当前工作区 MSVC Release 重建后：默认 CTest 模式 6 项、65 条断言通过；修复目标模式退出码 1，6 项中 4 项失败、65 条断言中 8 条失败。失败对应 FA-01/02/03/04/05，两个对照通过。目标模式结束后，新增未清理临时目录为零。

这份记录只限定本轮源码，不代表后续版本。修复意见应先重跑上面的命令，再逐项核对失败断言。原始输出留在本地 `.local-audits/failure-boundaries/current-run/`。
