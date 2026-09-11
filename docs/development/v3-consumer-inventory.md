# v3 消费者清册与测试分类(T00/T16 基线)

- 状态:活文档。B1(2026-09-11)建册;随各批迁移逐条更新,清零即退役门开。
- 来源:SessionV3 旧设计清理单(todos/SessionV3旧设计清理_消费迁移缺口补齐与旧接口退役.todo,不入发行包)§四 T00(消费者清册/夹具)、§九 T16(测试分类)。
- 基线:HEAD `62b69d44`(`src/app/version.hpp` = 0.26.251)。符号命中为静态盘点;"可落入分支"与"已实测复现"分开记。
- 用法:B2 各消费域(usage/Telemetry/Insights)迁移前先对表;B4(T02-A 依赖清册验空)以本册"退役阻塞"列为准。

## 一、判定法(不以目录名代替真实路径)

一册测试真实所走的格式路径由三件事决定,核对按此顺序:

1. 进程环境:`tests/CMakeLists.txt` 注册循环给每册统一注入
   `LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0`(全局钉 0,保 v2 老册);
2. 册内覆盖:引用该变量的 7 册用 `EnvGuard`/`_putenv` 在进程内改写
   (构造设值、析构 unset);
3. 是否经建场开关:`NewSessionV3WriteEnabled` 只在
   `SessionManager::LaunchSession` 与 resume/clear 分派两处读取;
   绕过 SessionManager 直接用 `V3Writer`/`ReadV3Ledger` 的册与开关无关。

核对命令(只读,不 build):

```bash
ctest --test-dir build/release -C Release -N   # 实际注册名先对表
grep -rl "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS" tests/unit tests/integration
grep -rl "NewSessionV3WriteEnabled" tests/unit tests/integration
```

## 二、测试分类表(T16,B1 时点)

分类四档:**default-v3**(不设变量走产品默认)、**explicit-v3**(册内
EnvGuard 显式开)、**legacy-only**(吃全局 0 的 v2 行为断言)、
**format-neutral**(不经会话建场/格式开关)。

| 册(环境变量相关全集) | 档 | 说明 |
| --- | --- | --- |
| `unit.trajectory_v3.test_v3_default_smoke`(B1 新增) | default-v3 | 册内 unset 变量,真入口链(ledger 开场/turn/封口/SessionManager 建场)走默认 v3 |
| `unit.trajectory_v3.test_v3_write_wiring` | explicit-v3 + 守门 | 多数案 EnvGuard("1");末案 unset 钉"未设=开"、显式 0 回 v2 |
| `unit.trajectory_v3.test_v3_clear_switch` | explicit-v3 | EnvGuard("1") 为主;一案 EnvGuard("0") 钉 v2 老路回归 |
| `unit.trajectory_v3.test_v3_resume_chain` | explicit-v3 | EnvGuard("1") |
| `unit.trajectory_v3.test_v3_export_copy_projection` | explicit-v3 | EnvGuard("1") |
| `unit.trajectory_v3.test_v3_stream_wiring` | explicit-v3 | EnvGuard("1") |
| `unit.trajectory_v3.test_v3_verify_doctor_tree` | explicit-v3 | EnvGuard("1") |
| `unit.app.test_session_service` | legacy-only + 对照 | 含 EnvGuard("0") 显式钉 v2 的服务面用例 |
| 其余 435 册 | format-neutral 或 legacy-only | 不引用开关变量;与建场相关的册在全局 0 下跑 v2 形状(具体哪些册的断言绑定 v2 行为,随 B2 各域迁移逐册归档,不在 B1 一口吞) |

**v3 域内不经开关的册**(直接 V3Writer/ReadV3Ledger,format-neutral):
`test_v3_compact_block`(B1 新增,EnvGuard("1")/("0") 只为场格式)、
`test_v3_shared_fixtures`(B1 新增)、`test_v3_compact_runtime`、
`test_v3_compact`、`test_v3_writer`、`test_v3_envelope_schema`、
`test_v3_hooks`、`test_v3_preview_reduction`、`test_v3_reader_*`、
`test_v3_restored_history_view`、`test_v3_result_store`、
`test_v3_session_resume_bridge`、`test_v3_streaming`、`test_v3_subagent`、
`test_v3_system_chain`、`test_v3_tool_action`、`test_v3_transcript_page`、
`test_v3_usage_owner_hooks`、`test_v3_fixtures`。

**trajectory 域**(v2 Journal 语义册):`test_session_admin_v3_delete`
(B1 新增,含 v2 老门回归案)、`test_session_manager_*`、
`test_session_lock`、`test_session_status_lifecycle`、`test_session_verifier`
等为 legacy-only 或 format-neutral 混合;B4 撤全局 0 前须逐册归档。

撤全局 0 的门(B4):上表"legacy-only"逐册或迁或改断言,不许一刀换 1
后删失败册(单子 §3.3)。

## 三、消费者清册(T00;T02-A 迁移依据,B4 验空)

按"源符号 → 直接调用方 → 最终入口 → 现行输入/输出 → 替代 → 退役阻塞"
记账。命中以 `grep` 为准;"待分类"列不因近名符号先定罪。

| # | 源符号/文件 | 直接调用方 | 最终入口 | 现行合同 | 替代 API | 退役阻塞 |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | `accounting::ListSessionStreams`/`ParseStream`(src/accounting/session_usage_reader.*) | usage_projector、usage 命令、insights(integrity_gate/prompt_auditor) | `/usage`、workspace 汇总、insights 报告 | 主流只认 main.jsonl,子流平铺目录;EventEnvelope 解析;找不到 v3 样本仍 ok=true 空集 | T00 统一读面(v3 reader + 子 session 递归)+ v3 usage owner | T06 全迁 + T14 insights 迁完 |
| C2 | `telemetry::TelemetryService::DiscoverStreams`/`projector::Fold`(src/telemetry/*) | TelemetryService 自身(spool/cursor/补投) | OTLP 导出链 | EventEnvelope Fold;游标按旧流身份;发现按 SessionIndex | T00 读面 + v3 事件/usage 投影(T07,含投影版本) | T07 |
| C3 | `app::MemoryLedgerBridge`(src/app/memory_ledger_bridge.*) | MemoryAccounting 装配 | 主入口 memory-on 场 | 走 `ledger.main()`(v2 recorder);v3 场 main()==nullptr → 召回失败 | v3 受管 writer/context 服务 + 正文快照/refs(T08) | T08 桥迁 + §4.71 排期 |
| C4 | `app_server/server.cpp` thread 记录(`session_main_path` 拼接、thread/resume 只读预览) | AppServer 协议处理 | AppServer 2.0 WS | 硬拼 `.../main.jsonl`;resume 为只读恢复视图预览 | SessionService 统一描述对象 + 真恢复(T09,AppServer 单实施) | T09 |
| C5 | `insights::integrity_gate`/`prompt_auditor`/`friction_classifier`(src/insights/*) | insights 聚合入口 | `/doctor` 类分析、workspace 报告 | 查 main.jsonl + EventEnvelope 集合;损坏跳过整场 | v3 reader/verify + partial 报告(T14) | T14 |
| C6 | `trajectory::ScanStreamFacts`(v2 状态机骨架) | session_manager(恢复器/删除门/归档) | 会话管理命令面 | 只认 v2 事件状态机;v3 场由 B1 起新增 v3 分派(删除门) | v3 首行/终态/锁推导(T15-B 扩全生命周期) | T15-B |
| C7 | `runtime::TrajectoryTurnBridge` v2 分支(`Put`/EventKind) | loop 边界接线 | 所有模型/工具边界 | v2 recorder 事件;v3 场已走 V3* 系 | v3 桥已并行;T02-A 收窄剩余消费者 | T02(双分支缩减) |
| C8 | `tools::task_ledger.*`、`tools/agent_tool.cpp`、`runtime/event.hpp`、`runtime/runtime_contract.cpp`、`app/memory_extract.cpp` | 待分类 | 待分类 | 近名命中(RecordRequest/RecordRequestOutcome 等),未逐个追到生产调用 | 待 T00 深挖(B2 起) | 待分类完成 |

**保留共用件**(不因目录删除):canonical JSON(`trajectory/canonical.*`)、
SHA-256 哈希链算法、BlobStore、SessionLock、Durability 三档、JournalWriter
落盘内核、`platform::` 时间/路径件。理由:与格式无关的持久化底座;
v3 writer 明文承继(见 `src/trajectory/v3/writer.hpp` 头注)。

## 四、夹具冻结清单(T00)

合法账一律经 `V3Writer` 现场生成;错误形状在合法账上注入(writer 造不出
错误账,避免 writer/reader 同一错误互相自证)。静态九份
(`tests/fixtures/trajectory_v3/`:startup/soul_switch/tool_round/compact_full/
stream_interrupted/hook_effects/preview_reduction/subagent_parent/
subagent_child)由 python 生成器产出、`scripts/validate_trajectory_v3.py`
校验,C++ 侧 `test_v3_fixtures` 互证。

| 形状 | 载体 | 消费方 |
| --- | --- | --- |
| 主账多轮、缺 usage(null 不补 0) | `test_v3_shared_fixtures`(B1 新增) | T06/T07/T14 |
| 迟到 usage(model.usage.appended 观察,不倒改 owner) | 同上 | T06 |
| compact applied 全链(链重接 system+摘要+保留) | 同上 + 静态 compact_full | T06/T12 |
| 工具逐项结果(pending→started→finished→persisted→selected→tool 消息) | 同上 + 静态 tool_round | T14/T04 |
| 缺 blob(artifactRef 指向不存在的文件 → missing_blob/complete=false) | 同上 | T14 |
| 两层子代理(父→子→孙,WalkSessionTree 两层) | 同上 | T06/T14 |
| 两次 resume 来源链(C←B←A,ProjectResume 验 hash 无重复) | 同上 | T06/T07 |
| 坏尾(截断)/坏中段(改字节) | 同上(注入) | T14/T15 |
| 锁态(活锁可探、释放即消) | 同上 | T15-B |
| 归档态 | **缺口**:v3 场生产归档形状未发行(T15-B),不伪造 | T15-B 补 |

## 五、B1 已落的止损改动索引

| 任务 | 落点 | 测试 |
| --- | --- | --- |
| T12-A(compact 投影失败执行阻断,P0) | `V3SessionBooks::execution_blocked`(ledger 会话级);`V3RequestPrepared` 准入门;`BlockV3Execution`/`V3ExecutionBlocked`;`RunV3CompactBranch` 拆 `persisted_applied`/`runtime_ready` 两笔账 | `unit.trajectory_v3.test_v3_compact_block` |
| T15-A(v3 删除封口门,P0) | `DeleteSessionDir` v3 分派(验卷 + session.ended 封口 + 末行 hash 入 tombstone);格式歧义拒;`LooksLikeV3SessionStream`/`DeleteV3SessionDir` | `unit.trajectory.test_session_admin_v3_delete` |
| T16 默认-v3 冒烟 | 见 §二表首行 | `unit.trajectory_v3.test_v3_default_smoke` |
| T00 夹具/清册 | 本文档 + `unit.trajectory_v3.test_v3_shared_fixtures` | — |
