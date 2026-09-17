# v3 消费者清册与测试分类(T00/T16 基线)

- 状态:活文档。B1(2026-09-11)建册;随各批迁移逐条更新,清零即退役门开。
- 来源:SessionV3 旧设计清理单(对应设计单,不入发行包)§四 T00(消费者清册/夹具)、§九 T16(测试分类)。
- 基线:HEAD `62b69d44`(`src/app/version.hpp` = 0.26.251)。符号命中为静态盘点;"可落入分支"与"已实测复现"分开记。
- 用法:B2 各消费域(usage/Telemetry/Insights)迁移前先对表;B4(T02-A 依赖清册验空)以本册"退役阻塞"列为准。

## 一、判定法(不以目录名代替真实路径)

一册测试真实所走的格式路径由三件事决定,核对按此顺序:

1. 进程环境:`tests/CMakeLists.txt` 注册循环按 `LUBANCODE_TESTS_V3_DEFAULT_BOOKS`
   撤 0 清单分账(T16 批起)——清单内册(已迁域,42 册)不注入格式变量
   跑生产默认,清单外仍统一注入 `LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0`
   保 v2 老册;
2. 册内覆盖:引用该变量的册用 `EnvGuard`/`EnvUnset`/`_putenv` 在进程内
   改写(构造设值/摘除、析构还原),册内覆盖优先于进程环境;
3. 是否经建场开关:`NewSessionV3WriteEnabled` 只在
   `SessionManager::LaunchSession` 与 resume/clear 分派两处读取;
   绕过 SessionManager 直接用 `V3Writer`/`ReadV3Ledger` 的册与开关无关。

核对命令(只读,不 build):

```bash
python scripts/tests/ctest_format_registry.py        # 全表(册×环境×证据)
ctest --test-dir build/release -C Release -N         # 实际注册名对表
grep -rl "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS" tests/unit tests/integration
```

## 二、测试分类表(T16;B1 建册,T16 批撤 0 分账落地)

分类四档:**default-v3**(ctest 不注入格式变量,环境=生产默认)、
**explicit-v3**(册内 EnvGuard 显式开)、**legacy-only**(吃注入 0 的
v2 行为断言)、**format-neutral**(不经会话建场/格式开关)。

册名一律用 CTest 注册名(`unit.<域>.<stem>`/`integration.<域>.<stem>`,
**stem 不带 `test_` 前缀**——文件 `test_x.cpp` 注册为 `…x`;各批结案
记录里带前缀的写法是口语形态)。

完整分账表(551 册)由机器产出,不再手工维护:

```bash
python scripts/tests/ctest_format_registry.py            # 打印全表
python scripts/tests/ctest_format_registry.py --check    # CI 门
```

判定按三件事归证据(不以目录名代替真实所走路径):ctest 注入(解析
`tests/CMakeLists.txt` 的 `LUBANCODE_TESTS_V3_DEFAULT_BOOKS` 撤 0 清单)、
册内覆盖(guard 1/guard 0/unset 的引用计数)、是否经建场口
(`TrajectorySessionLedger::Open`/`LaunchSession`/`SpawnSubagent`)。

### T16 批(2026-09-16)撤 0 分账:42 册先行

ctest 注入的 `LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0` 按单 §九 T16 勾三
口径"随各批迁移"逐步撤——本批只动已迁完域(usage/telemetry/insights/
memory 桥/lifecycle/compact/T11 + 默认冒烟)。撤 0 = ctest 不再注入该
变量,册跑真实产品默认(未设=开 v3)。CI 门(registry --check)保证清单
内每册"册内自证格式或完全不经建场口",防笔误把 v2 断言册推上 v3。

| 域 | 册(ctest 注册名) | 撤 0 依据(逐册证据) |
| --- | --- | --- |
| usage(T06,#53) | `unit.accounting.pricing_cost`/`usage_aggregate`/`usage_contracts`/`v3_session_usage`、`unit.trajectory_v3.v3_usage_owner_hooks` | 5 册均不经建场口(夹具直写 V3Writer/价目表);撤 0 无行为差 |
| telemetry(T07,#102) | `unit.telemetry.telemetry_*` 12 册、`integration.telemetry.telemetry_otlp_export` | 13 册均不经建场口(夹具直写盘账) |
| insights(T14,#101) | `unit.insights.*` 13 册 | 均不经建场口;`insights_v3_pipeline` 另册内 guard1×10(防御,断言按账面格式分派不按环境变量) |
| memory 桥(T08,#105) | `unit.trajectory_v3.v3_memory_recall_bridge` | 册内全案 guard1(7 案);v2 老册 `unit.app.memory_ledger_bridge` 是旧路回归册,**不在撤 0 范围** |
| lifecycle(T15,#107) | `unit.trajectory.session_admin_v3_delete`/`session_admin_v3_lifecycle` | 册内全案 guard(v3 案钉 1×5/×13,v2 回归案钉 0×1);撤 0 无行为差 |
| compact(T12,#109) | `unit.trajectory_v3.v3_compact`/`v3_compact_block`/`v3_compact_gates`/`v3_compact_runtime` | `v3_compact`/`runtime` writer 直驱不经建场;`block`(guard1×3+guard0×1)、`gates`(guard1×2,第三案纯函数)册内自证 |
| T11 五域(#108) | `unit.trajectory_v3.v3_t11_title`/`v3_t11_approval_env`/`v3_t11_verification_budget` | 3 册册内全案 guard1 |
| 默认冒烟(B1/T16) | `unit.trajectory_v3.v3_default_smoke` | 册内 EnvUnset×3 摘变量走真入口;本批增子代理与 CLI compact 管理入口两案(勾二补齐) |

撤 0 后上述册在 CI 无注入环境下全绿(本批 PR 的 CI 记录为准)。后续批次
迁完新域(gateway/app_server/goal/channel 等 EnvGuard 册所在域)再扩
清单;B4(T01 收口)关掉全局 0 注入时,清单、CI 门与分账机制一并退役。

### 显式格式册(explicit-v3/legacy-only 对照,未撤注入)

`unit.trajectory_v3.v3_write_wiring`(多数案 guard1,末案 unset 钉
"未设=开"、显式 0 回 v2)、`v3_clear_switch`/`v3_resume_chain`/
`v3_export_copy_projection`/`v3_stream_wiring`/`v3_verify_doctor_tree`
(guard1 为主)、`unit.app.session_service`(含 guard0 显式钉 v2 的服务面
用例)、`unit.app_server.operation_idempotency`(v2 案钉 0、v3 案钉 1,
T16 批另增未设变量默认-v3 冒烟案)、`unit.gateway.gateway_automation_v1/v2`、
`unit.app.goal_v3_commands`、`unit.app_server.app_server_detached` 等——
册名与证据以 registry 脚本产出为准。其余册 format-neutral 或 legacy-only
混合;B4 撤全局 0 前须逐册归档,不许一刀换 1 后删失败册(单子 §3.3)。

## 三、消费者清册(T00;T02-A 迁移依据,B4 验空)

按"源符号 → 直接调用方 → 最终入口 → 现行输入/输出 → 替代 → 退役阻塞"
记账。命中以 `grep` 为准;"待分类"列不因近名符号先定罪。

| # | 源符号/文件 | 直接调用方 | 最终入口 | 现行合同 | 替代 API | 退役阻塞 |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | `accounting::ListSessionStreams`/`ParseStream`(src/accounting/session_usage_reader.*) | usage_projector、usage 命令、insights(integrity_gate/session_analyzer 的 v3 半场) | `/usage`、workspace 汇总、insights 报告 | v3 已分派(T06,2026-09-12):`ReadSessionUsage` 先 `ProbeV3SessionStream`,v3 走 `ReadV3Ledger`+`WalkSessionTree` 递归子 session,`ProjectV3Usage` 吃 assistant usage owner;v2 老路(main.jsonl/平铺子流/EventEnvelope)保留给旧档。T14(2026-09-16)起 insights 的 v3 半场复用同一读面(`AnalyzeSessionV3` 逐 `ReadSessionUsage`) | v2 老路归 T02-B 随旧档退役 | T02-B |
| C2 | `telemetry::TelemetryService::DiscoverStreams`/`projector::Fold`(src/telemetry/*) | TelemetryService 自身(spool/cursor/补投) | OTLP 导出链 | v3 已分派(T07,2026-09-16):发现先 `ProbeV3SessionStream`,v3 走递归 `<id>.jsonl`+`subagents/*/<cid>.jsonl`,`ProjectV3LedgerFile` 吃 v3 消息/事件(usage owner 与 T06 同源),cursor 固定末 recordId/seq/hash;v2 老路(EventEnvelope Fold/旧流身份)保留给旧档 | T02-B(删 v2 Fold;投影版本已升 `telemetry-projector-v2` 另开 generation) | T02-B |
| C3 | `app::MemoryLedgerBridge`(src/app/memory_ledger_bridge.*) | MemoryAccounting 装配 | 主入口 memory-on 场 | v3 已分派(T08,2026-09-16):`v3_main_writer()` 优先——召回注入落 display=hidden 的 user 消息(origin=context_runtime)+`AdmitMessages` 接纳 + `memory.recall.injected` 事实(memoryId/revision/hash/messageRef),prepared 的 inputMessageRefs 沿链带上;派工冻结走父账事实(targetRunId+快照)不进链;写入因果边 `memory.save.requested`(requested/queued/committed 三态按真实回执分账)。v2 场照旧 recorder 老路 | 已迁;v2 老路归 T02-B;MemoryStore 版本/CAS/遗忘与子账侧完整冻结链归 §4.71(T13-M) | T02-B;T13-M |
| C4 | `app_server/server.cpp` thread 记录(`session_main_path` 拼接、thread/resume 只读预览) | AppServer 协议处理 | AppServer 2.0 WS | 硬拼 `.../main.jsonl`;resume 为只读恢复视图预览 | SessionService 统一描述对象 + 真恢复(T09,AppServer 单实施) | T09 |
| C5 | `insights::integrity_gate`/`prompt_auditor`/`friction_classifier`(src/insights/*) | insights 聚合入口 | `/insights`、`/prompt audit`、`/doctor insights`、workspace 报告 | v3 已分派(T14,2026-09-16):`GateSession` 先 `ProbeV3SessionStream`,v3 走 `WalkSessionTree`+领域读模型(`v3_facts.*`:prepared 引用/inputView 指纹/usage owner/工具折叠);坏来源/缺 blob/子账缺在 notes 标 partial 不跳整场;封口按账面 `session.ended`(无 session.json);不认的格式标 `Unsupported` 不迁旧档。prompt 审计吃 prepared 持久请求视图(R01 不再要求 v2 manifest);摩擦 v3 半场(`friction-v2`)按 actionId/attempt 取材,无审批/验证事实的类别不判分;usage 走 C1 同一读面(父子树/resume 祖先不重算)。v2 老路保留给旧档 | 已迁;v2 半场归 T02-B 随旧档退役 | T02-B |
| C6 | `trajectory::ScanStreamFacts`(v2 状态机骨架) | session_manager(恢复器/删除门/归档) | 会话管理命令面 | 只认 v2 事件状态机;v3 场由 B1 起新增 v3 分派(删除门) | v3 首行/终态/锁推导(T15-B 扩全生命周期) | T15-B |
| C7 | `runtime::TrajectoryTurnBridge` v2 分支(`Put`/EventKind) | loop 边界接线 | 所有模型/工具边界 | v2 recorder 事件;v3 场已走 V3* 系 | v3 桥已并行;T02-A 收窄剩余消费者 | T02(双分支缩减) |
| C8 | `tools::task_ledger.*`、`tools/agent_tool.cpp`、`runtime/event.hpp`、`runtime/runtime_contract.cpp`、`app/memory_extract.cpp` | (已分类,2026-09-16)task_ledger/agent_tool 的 `RecordRequestOutcome` 是 TaskLedger 领域账方法(`AgentTaskEventKind` 自有枚举),不经 trajectory 信封;`runtime/event.hpp` 的 `EventEnvelope` 是 runtime/AppServer 协议信封(thread_id/seq/timestamp_ms,同名不同物),`runtime_contract.cpp` 是它的序列化契约;`app/memory_extract.cpp` 是真实双格写口(v3 writer 优先、v2 recorder 保底,`memory.extraction.assessed` 的 v2 同名事件) | agent 任务账、AppServer 事件流、memory 抽取调度 | 近名命中已逐一追到生产调用:前四者非 v2 信封消费者(保留);memory_extract 的 v2 老路归 T02-B | 不需替代;memory_extract v2 分支挂 T02-B | T02-B(memory_extract);其余无阻塞 |

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
| T12-A(compact 投影失败执行阻断,P0) | `V3SessionBooks::execution_blocked`(ledger 会话级);`V3RequestPrepared` 准入门;`BlockV3Execution`/`V3ExecutionBlocked`;`RunV3CompactBranch` 拆 `persisted_applied`/`runtime_ready` 两笔账 | `unit.trajectory_v3.v3_compact_block` |
| T15-A(v3 删除封口门,P0) | `DeleteSessionDir` v3 分派(验卷 + session.ended 封口 + 末行 hash 入 tombstone);格式歧义拒;`LooksLikeV3SessionStream`/`DeleteV3SessionDir` | `unit.trajectory.session_admin_v3_delete` |
| T16 默认-v3 冒烟(B1) | 见 §二表首行 | `unit.trajectory_v3.v3_default_smoke` |
| T16 撤 0 分账(T16 批) | `tests/CMakeLists.txt` 的 `LUBANCODE_TESTS_V3_DEFAULT_BOOKS` 清单 + 注册循环分账;`scripts/tests/ctest_format_registry.py`(分账表 + CI 门);`scripts/tests/report_baseline.py`(勾五基线记账,CI 步骤消费);default_smoke 增子代理/CLI compact 管理入口两案、app_server operation_idempotency 增未设变量冒烟案 | 见 §二"撤 0 分账"表 |
| T00 夹具/清册 | 本文档 + `unit.trajectory_v3.test_v3_shared_fixtures` | — |
| T14(Insights 接入 v3,B2) | `src/insights/v3_facts.*`(领域读模型)、integrity_gate/prompt_auditor/friction_classifier/session_analyzer 的 v3 半场、summary format/limitations、EvidenceItem.seq、redaction allowlist | `unit.insights.insights_v3_pipeline`(gate 分型/partial/摩擦 v3/prompt v3 规则/分析去重/字节稳定/schema 往返) |
