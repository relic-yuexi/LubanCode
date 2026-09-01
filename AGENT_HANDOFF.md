# AGENT_HANDOFF — 16 红合并交叠回归专项（封机存照）

- 分支：`worktree-agent-a17fd50fb44cdfe5a`（隔离 worktree，基于 main ad721e6d，未 push）
- 状态：**363/363 全绿**（worktree 全新构建，`ctest -C Release` 退出码 0）
- WIP 定位：缝合已收完，非半成品。

## 结论一句话

原 16 册红里 **15 册是主仓构建树增量状态毒化的假红，1 册（telemetry_otlp_export）疑同因**；main 源码在全新构建下唯一真病是 `tests/unit/app/test_memory_ledger_bridge.cpp` 的字段改名失配（编译错），本分支已修。

## 根因清单（按类）

### 一类：主仓构建树陈旧 obj 混链（16 册全归此类；源码无病）

机理（实证链）：
- P0-3（12a075b5）在 `src/trajectory/event.hpp` 的 `EventKind` 枚举**中段**插入 4 枚（`ContextInjected` 插在 `ContextDetached` 后、`MemorySave*` 三枚在尾部），并改 `schema.cpp` 的 `kPayloadFields`。
- 主仓 22:50 的"clean"构建实际未重编全部受影响 obj（`trajectory_session.cpp` mtime 20:39 早于 event.hpp 22:36 的改动）：旧 obj 按旧枚举序传枚举值，新 `kPayloadFields` 按新序查表——`ModelRequestPrepared` 错位落到 `ContextInjected` 的九键必填表上，报 `schema.payload_missing_field`；`ControlCwdChanged`/`ControlCommandRequested` 同理错位报同码。
- 铁证：同一份源码（md5 比对一致）主仓 exe 红、本 worktree 全新构建绿；主仓 exe 的报错字段集与错位后的 required 表严丝合缝。
- 这与主控踩过的"失败构建毒化增量状态、动头文件须 --clean-first"是同一家病。

覆盖册（主仓 16 红）：channel_phase3_e2e、telemetry_otlp_export、app_commands、app_server_{approval,progress,turn,ws}、command_service、session_command_service、trajectory.{a1_smoke_wiring,bypass_recorder,evidence_side_effects,resume_wiring,runtime_wiring,training_exporter}、workspace_switch。
- 其中 telemetry_otlp_export 在主仓单跑即绿（全量才红），先按污染/同因记；本 worktree 全量两轮均绿。

### 二类：P0-3 测试没跟 P0-2 字段改名（1 处，真病，已修）

`tests/unit/app/test_memory_ledger_bridge.cpp` 三处（105/190/248 行）：
`ledger_options.trajectories_root` → `ledger_options.workspaces_root`。
P0-2 把 `TrajectorySessionLedger::Options` 的字段改了名，P0-3 并行新加的这册测试没跟上——main 上该文件编译必炸（炸了整只 lubancode_tests.exe 连不出，363 册全 Not Run）。主控手头修过但未进 main；本分支落了这笔。

### 三类：worktree 环境缺件（非源码，构建后手工补）

search 内核只认 `ExecutableDir()/libexec/rg.exe`（不搜 PATH）。全新构建后 `unit.tools.observation_filter`、`integration.ptc.ptc_tool` 红，从主仓拷 rg.exe 到 `build/release/tests/Release/libexec/` 与 `build/release/Release/libexec/` 后即绿。**新机器构建完记得拷**（主仓 `build/release/libexec/rg.exe` 是源）。

## 本分支改动

- `tests/unit/app/test_memory_ledger_bridge.cpp`：三处字段改名跟齐（唯一源码改动）。
- `AGENT_HANDOFF.md`：本文。
- 版本号未动；未 push（主控统一推）。

## 新机器复跑要点

1. `cmake --preset release && cmake --build --preset release`（首次约 25 分钟，FetchContent 编 curl/cpr）。
2. 拷 rg.exe 到 `build/release/tests/Release/libexec/` 与 `build/release/Release/libexec/`。
3. `cd build/release && USERPROFILE='C:\...\temp目录' ctest -C Release`——363/363 为准。
4. 任何时候 merge 动了 `event.hpp`/`schema.hpp`/枚举中段插入，构建用 `--clean-first`，别信增量。

## WSL 语法验

`g++-15 -std=c++23 -fsyntax-only`（复用主仓 `_deps` 的 doctest/nlohmann 头）过 `tests/unit/app/test_memory_ledger_bridge.cpp`，零告警退出。
