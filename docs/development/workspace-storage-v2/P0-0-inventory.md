# Workspace 统一存储 P0-0 盘点:旧写口与身份钥匙对照表

- 日期:2026-09-01
- 状态:P0-0 冻结(后续波次照此表干活,不许"实现时再看")
- 单子:`todos/Workspace统一存储_旧Session退场与分级Memory迁移.todo`
- 盘点基准:main 分支 0.26.148(merge 后 worktree HEAD)
- 性质:纯诊断。本批不改任何生产行为。

读法:每张表列"文件:行 / 内容 / 谁在用 / 迁移归宿"。归宿里的批次号
(P0-1..P0-6)指单子 §十 的分期;`tools/legacy-storage-migrator/` 指迁移器
隔离区(P0-5 建,P0-6 后封存)。

---

## 表一:旧写口(SessionStore / sessions_dir / features.trajectory / 旧 artifact 路径 / 旧 SessionCatalog)

### 1A. `sessions::SessionStore` 本体(旧 JSONL 落盘句柄)

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `src/sessions/session_store.hpp:385-454`、`session_store.cpp:1343-1520` | `SessionStore` 类:Begin/ResumeAt/AppendMessage 与 12 只 Append*Event 写口(compact、compact_v2、title、cwd、queue、tool_trace、raw、goal、goal_evidence、mode、plan、plan_review、think_history) | 表 1B 全部持有者 | P0-6 删类与 CMake 项;序列化/解析纯函数(Serialize*/Parse*/RepairToolPairs)P0-5 迁 `tools/legacy-storage-migrator/`,不留在生产 |
| `src/sessions/session_store.hpp:471`、`session_store.cpp:1535` | `ListSessions(sessions_dir,...)` 平铺目录扫描 | 见 1G | 同上 |
| `src/sessions/session_store.hpp:503-510`、`session_store.cpp:1733` | `ExtractLivePromptTail`/`persisted_count` 尾巴账 | interactive_session.cpp:526-562(提问历史) | P0-2 改读 trajectory 投影;纯函数随迁移器走 |

### 1B. `SessionStore` 的实例化与持有者

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `src/runtime/session_runtime.hpp:111-112,201` | 成员 `store_` 与 accessor | 装配层别名绑定、tool_trace_hub、goal ledger、标题账 | P0-6 删成员(P0-2 起 trajectory 路已不用) |
| `src/app_server/server.cpp:724` | `record->store = make_unique<SessionStore>(sessions_dir_)`(thread/start) | app-server thread 会话 | P0-2:flag 摘除后这条只剩死分支,P0-6 删 |
| `src/app/interactive_session_assembly.cpp:431` | `session_store(session_runtime_.store())` 引用别名 | interactive_session_controller.hpp:426、commands 上下文 | P0-2 换 Trajectory SessionLedger 接口,P0-6 删 |
| `src/runtime/tool_trace_hub.hpp:53-54,143`、`tool_trace_hub.cpp:15-18` | 构造持 `SessionStore*`(旧路落盘);挂轨迹口的会话不写(:52 已短路) | interactive_session_assembly.cpp:450、turn_runner.cpp:807 | P0-2 持久账全走本轮边界桥(轨迹路),P0-6 删旧指针参数 |
| `src/runtime/goal_coordinator.cpp:1341`(声明 goal_coordinator.hpp:288) | `MakeSessionLedgerSink(SessionStore&)`:goal 事件折进旧账 | goal 装配(wirings/goal_session_wiring.hpp:60) | P0-2 改接 trajectory ledger sink(goal_v1 → typed 事件) |
| `src/app/session_title_account.hpp:43-75`、`session_title_account.cpp:13` | 标题账写 `AppendTitleEvent` | interactive_session_assembly.cpp:441 | P0-2 接 trajectory title 事件;P0-6 删 |
| commands 上下文的 `session_store` 指针字段:`app/commands/command_registry.hpp:169`、`goal_commands.hpp:82`、`loop_commands.hpp:74`、`memory_commands.hpp:64`、`session_commands.hpp:171`、`trace_commands.hpp:27` | 各命令组拿旧账句柄 | /title、/export、goal、loop、trace 命令 | P0-2 换 workspace SessionManager 句柄;P0-6 删字段 |
| `src/app/interactive_session_controller.hpp:426` | 控制器持 `SessionStore&` | 会话层薄壳(EnsureSessionBegun/PersistNewMessages) | P0-6 删 |

### 1C. `PersistNew` / `persisted_count`(旧消息增量落盘)

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `src/runtime/session_runtime.hpp:133-141`、`session_runtime.cpp:90-141` | `PersistNew()`:history 里 `persisted_count_` 之后逐条 append;trajectory_enabled 时短路(:93) | PersistNewMessages(下行) | P0-6 删(单子 §7.4 点名) |
| `src/runtime/session_runtime.hpp:205-208` | `persisted_count_` 基线 | resume/compact 收缩逻辑 | P0-6 删 |
| `src/app/interactive_session.cpp:372-395` | `PersistNewMessages()` 薄壳;调用点 :677、:952 | 会话层每轮收口 | P0-6 删 |
| `src/app/commands/session_commands.cpp:882,1082,1615,1839,2055` | resume 后置基线、compact/clear 后收缩基线 | /resume、/compact、/clear | P0-2 改由 Replay 投影接管,P0-6 删 |
| `src/runtime/command_service.cpp:283` | trajectory resume 成功后 `persisted_count = messages.size()` | --continue/resume 服务 | P0-2 改轨迹路专属账 |
| `src/agent/loop.cpp:1339-1340`、`loop.hpp:255` | 老路"整轮收口后 PersistNewMessages 兜底"注释与挂点 | agent loop | P0-2 换轨迹路后成死注释,P0-6 清 |
| `src/app/interactive_session_controller.hpp:430` | `std::size_t& persisted_count` 别名 | 同上各处 | P0-6 删 |

### 1D. `sessions_dir` 的产生与传递(`~/.lubancode/sessions` 根)

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `src/app/cli_app.cpp:374` | `options.sessions_dir = <luban_dir> + "/sessions"`(app-server 面) | app_server Options | P0-2 删(该字段退役) |
| `src/app/interactive_session_assembly.cpp:407-408` | 终端面同一拼法进 SessionRuntime::Options | SessionRuntime | P0-2 删 |
| `src/app_server/server.hpp:95,291` + `server.cpp:255-261,445-455` | Options 与成员;thread 查询/搬删执行体 | thread/list、thread 冷回放 | P0-2 换 workspace 会话索引 |
| `src/runtime/session_runtime.hpp` Options 的 `sessions_dir` 字段 | runtime 档案目录 | 1E 各 artifact 挂点 | P0-6 删 |
| `src/app/one_shot.cpp:354` | 单发不落盘,显式给空串 | one_shot | P0-6 随字段删 |
| `src/app/commands/command_registry.hpp:170` + `evolve_commands.cpp:108-109` | evolve 数据源 | evolution/collector | P0-2 后 evolve 只读新账投影 |
| `src/runtime/command_service.hpp:144` + `command_service.cpp:194-267` | ListThreads/FindThread 冷查旧档 | runtime 命令服务 | P0-2 换 SessionManager/索引 |
| `src/runtime/session_command_service.hpp:62-97` + `.cpp:84-106` | 查询/搬删执行体(平铺扫描) | app-server、session_commands | P0-2 换;P0-6 删 |
| `src/trajectory/usage_gc.hpp:52`、`usage_gc.cpp:90-100` | **同形参名,不同物**:指新根下 workspace 的 `sessions/` 子目录 | usage 报告 | 不删,P0-1 改名 `workspace_sessions_dir` 防混 |
| `src/cli/trajectory_command.cpp:84-91` | 同上,trajectory 根下 sessions 扫描 | /trajectory list | P0-2 挪到新根,逻辑保留 |

### 1E. 旧 artifact 路径写口(旧会话目录的旁挂文件)

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `sessions/<id>/context/`(index.jsonl + blobs/ + chunks/) | 渐进上下文仓:`src/agent/artifact_store.hpp:6-32` | `interactive_session.cpp:360-366`(开仓)、`interactive_session_controller.hpp`(OpenArtifactStore 薄壳) | P0-2 迁 `workspaces/<key>/sessions/<id>/artifacts/sha256/`(内容寻址),旧仓 blob 随 P0-5 迁移器搬 |
| `sessions/<id>/mcp-artifacts/` | MCP rich result 落盘:`src/mcp/rich_result.hpp:49-51`、`rich_result.cpp:180-196`(LandToolArtifact)、`mcp_tool.cpp:43-45`、`client.hpp:75-120`、`client.cpp:316` | `interactive_session.cpp:900`(挂目录)、`turn_runner.hpp:165`、`tools/agent_tool.cpp:1764`(子代理继承)、`plugin_tool.cpp:58-97`(插件图片)、`browser_service.cpp:538,661`(截图,另一根) | P0-2 统一进 session `artifacts/`;BrowserService 的独立根 `browser-artifacts` 不在本单 |
| `sessions/<id>/images/` | 模型输出图片:`src/agent/model_image_store.hpp:56` | `interactive_session.cpp:897`、`turn_runner.hpp:159` | P0-2 进 session `artifacts/`(model_image 引用块照旧只存引用) |
| `sessions/archive/` | 归档目录:`src/sessions/session_lifecycle.cpp:157-375`(archive/unarchive/rebuild);`session_lifecycle.hpp:94-132` | session_commands Archive/Unarchive/Delete | P0-2 改 workspace tombstone/索引投影;P0-6 删平铺实现 |
| `src/tools/tool_content.hpp:26-32`、`tools/tool.hpp:61-64` | 相对路径引用块形状("mcp-artifacts/art-<sha8>.<ext>") | 序列化与 history | P0-2 换 artifacts/sha256 引用形状(新事件 `tool.result.committed` 补无损投影) |

### 1F. `features.trajectory` / `LUBANCODE_TRAJECTORY` 双路开关

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `src/config/config.hpp:691-696`(全局 bool)、`:903-904`(项目级 optional) | 配置字段 | 合并与装配 | P0-6 删字段与配置文档 |
| `src/config/config.cpp:1847-1849`(解析)、`:2716-2721`(项目压全局合并) | 配置读写 | config 层 | P0-6 删 |
| `src/runtime/trajectory_session.hpp:52-53` + `trajectory_session.cpp:35-42` | `ResolveTrajectoryEnabled`(flag + 环境变量合成) | assembly:416、cli_app:397、app_server:721/754 | P0-2 改恒真;P0-6 删函数与环境变量 |
| `src/app/interactive_session_assembly.cpp:411-426` | 终端开轨迹分支(含 trajectory_workspace_root=current_path()) | 交互会话 | P0-2 无条件开;workspace_root 换 WorkspaceIdentity(P0-1) |
| `src/app/cli_app.cpp:396-398` | app-server 面同款 | server options | P0-2 删分支 |
| `src/app_server/server.hpp:122` + `server.cpp:721,754-762` | thread 开档分支与"开不出账 thread 明败" | thread/start | P0-2 删 flag,失败合同保留 |
| 文案/诊断消费者:`app/commands/usage_commands.cpp:431`、`prompt_audit_commands.cpp:498,545`、`doctor_commands.cpp:1150`、`cli/i18n.cpp:702-706,2487-2491`、`telemetry/activation.hpp:38`、`insights/prompt_auditor.cpp:588` | "flag 关则无账"提示语 | /usage、/prompt audit、/doctor | P0-6 随双路删除,文案改"账永远在" |
| `src/runtime/session_runtime.hpp:65-68,103,128` + `session_runtime.cpp:17,51,92` | Options.trajectory_enabled 与短路点 | SessionRuntime | P0-6 删(短路永真化) |

### 1G. 旧 `SessionCatalog` / `SessionLifecycle` / `SessionCommandService` / 旧档只读消费

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `src/sessions/session_catalog.hpp:112-136` + `.cpp:286-388` | 平铺目录台账(scan_dir 根 + archive) | `session_commands.cpp:523`(/sessions)、`:737`(resume picker)、`:1160`(候选)、`:1288`(搬删);`runtime/session_command_service.cpp:106`;`evolution/collector.cpp:67` | P0-2 数据源换 workspace SessionManager + 可重建索引;查询/分页/筛选语义保留(改造成纯投影);P0-6 删扫描实现 |
| `src/sessions/session_lifecycle.hpp:94-132` + `.cpp:117-375` | archive/delete 的 Windows 句柄账与根内校验(PathInsideRoot) | session_commands、session_command_service | P0-2 换 workspace 桶(tombstone);越根校验逻辑保留迁移 |
| `src/runtime/session_command_service.hpp/.cpp` | thread.list/archive/delete 执行体 | app_server、session_commands:1271,1358,1407 | P0-2 换新账服务 |
| `src/cli/history_search.cpp:93-226` + `history_search.hpp:24-34` | 读旧 jsonl 抽提问历史(Ctrl+R) | 交互搜索 | P0-2 改读 trajectory indexes/derived 投影(提问历史事件) |
| `src/app/commands/session_commands.cpp:652-655`(MakeTranscriptExcerpt)、`:983-999`(export 写 sessions_dir 下 .md) | resume 摘要、/export 出档路径 | /resume、/export | P0-2 export 出档进 session `exports/`;摘要走 Replay |
| `src/app/commands/session_commands.cpp:2176-2259` | 轨迹路 export(折叠 main.jsonl 投影)与旧路并存 | /export | P0-2 删旧路分支 |
| `src/evolution/collector.hpp:34` + `.cpp:66-67` | evolve 扫旧档 | evolve 命令 | P0-2 换新账索引;旧档源随 P0-6 删 |
| `src/insights/prompt_auditor.cpp` | 只读 trajectory 账(不读旧档;":588" 仅提示语) | /prompt audit | 无迁移动作,列此防漏 |
| `tests/unit/sessions/`:test_session_store、test_session_lifecycle、test_session_catalog、test_session_command_service、test_session_picker;`tests/integration/loop/test_loop_persistence.cpp`、`tests/unit/evolution/test_evolution_collect.cpp` 等只验旧格式的用例 | 旧格式专属测试与夹具 | CI | P0-5 迁移器测试改吃 `tools/legacy-storage-migrator/`;P0-6 删只验"旧格式继续可写/可恢复"的用例;查询层测试随投影改造保语义 |

### 1H. 旧账上的 goal / loop / plan / think 事件写口(随 SessionStore 退场)

| 位置 | 内容 | 谁在用 | 迁移归宿 |
|---|---|---|---|
| `SessionStore::AppendGoalEvent/AppendGoalEvidence`(`session_store.cpp:1453-1488`) | goal_v1 族与 goal_evidence_v1 行 | MakeSessionLedgerSink(goal_coordinator.cpp:1341) | P0-2 goal 账接 trajectory typed 事件;goal_session.hpp/goal_catalog 纯数据先判归宿再动(单子 §十二) |
| `SessionStore::AppendRawLine`(loop_task_v1/loop_tick_v1 族) | loop 事件行 | loop 装配 | P0-2 loop 账接 trajectory |
| `SessionStore::AppendModeEvent/AppendPlanEvent/AppendPlanReviewEvent` | plan_v1 族 | plan 模式 | P0-2 plan 账接 trajectory |
| `SessionStore::AppendThinkHistoryEvent` | think_history_v1 | /think | P0-2 接 trajectory |
| `SessionStore::AppendCompactEvent/AppendCompactV2Event` | compact/compact_v2 行 | 会话压缩 | P0-2 接 trajectory compact 事件;v1/v2 回放语义由迁移器保真 |
| `SessionStore::AppendCwdEvent/AppendQueueEvent` | cwd/queue 快照 | /worktree 切房、排队账 | P0-2 接 trajectory(cwd.changed;queue 投影) |

### 1I. docs 旧路径生产说明(P0-6 清)

`docs/architecture/memory/design.md`、`docs/architecture/memory/flow.md`、
`docs/architecture/query-data-flow.md`、`docs/features/app-server/README.md`、
`docs/features/sessions/README.md`、`docs/reference/commands.md`、
`docs/reference/configuration.md` —— 共 7 件含 `~/.lubancode/sessions` 或
`~/.lubancode/projects` 生产路径。P0-6 换新根说明;P0-5 先加迁移指引
(`docs/getting-started/storage-migration.md`,单子 §十二)。

---

## 表二:身份钥匙(project_key / workspace_key 及同形物)

### 2A. `project_key`(memory 侧,`ResolveProjectIdentity`)

生成者:**唯一** —— `src/memory/project_memory.cpp:1811-1860`(声明
`project_memory.hpp:65-66`)。算法:

- 向上找 `.git`(`ResolveGitCommonDir`,linked worktree 解 commondir);
  非 Git 找最近 `.lubancode/config.json`;再退启动 cwd。
- `seed = ("git:"|"path:") + 规范绝对 common_root`(Windows 折叠 ASCII 小写)。
- `key = SafeName(display_name, ≤48B) + "-" + HexHash(seed)`;
  `HexHash` = 16 位 hex 的 **64 位 StableHash(非 SHA256)**;
  display_name 取 common git dir 的父目录名(worktree 裂口已修)。
- `project_dir = <home>/.lubancode/projects/<key>/`;memory 在其下
  `memory/`,候选在 `memory-candidates/`(:2312,:2383)。

| 消费者 | 位置 | 用法 | 迁移归宿 |
|---|---|---|---|
| 终端会话装配 | `src/app/session_stack.cpp:47`(BuildProjectMemory) | 启动解析身份、开记忆 | P0-1 改吃 `WorkspaceIdentity`,memory 根随 workspace |
| 单发(one-shot) | `src/app/one_shot.cpp:198` | 单发召回 | 同上 |
| /worktree 切房 | `src/memory/project_memory.cpp:1915-1921`(SetWorkingDirectory) | 重算身份换记忆根 | P0-1 冻结身份 + checkout 登记,不再每切必算 |
| recall trace / metadata / status | `project_memory.cpp:1930,2202,2285` | 账面回显 key | P0-3 字段改 workspace_key |
| memory job 过滤 | `project_memory.cpp:2300,1380` | job 按 project_key 归属 | P0-3 job 进 workspace memory/.state |
| memory-candidates 落点 | `project_memory.cpp:2383` | `<project_dir>/memory-candidates/` | P0-3 迁 `<workspace>/memory/memory-candidates/` |
| 旧 key 的最后用途 | 单子 §7.3 | 第一次打开 workspace 时算一次旧 key 读旧库 | P0-5 迁移器调用后封存;P0-6 删生产代码 |

**结论:生成者唯一、消费者六处,全在 memory/app 装配域;trajectory 侧不碰它。**

### 2B. `workspace_key`(trajectory 侧,`ComputeWorkspaceKey`)

生成者:**唯一** —— `src/trajectory/directory.cpp:82-101`(声明
`directory.hpp:33-34`)。算法:`SHA256(normalized_root)` 前 **12 位** +
basename;root 由调用方递。**现状 root = 启动 cwd,不是 Git 根**:

- `src/app/interactive_session_assembly.cpp:418`:`trajectory_workspace_root = current_path()`
- `src/app_server/server.cpp:756`:同款
- `src/runtime/trajectory_session.cpp:1268-1270`:空则 `current_path()`
- `FindWorkspaceRoot`(directory.cpp:103,Git 探测)**定义了但零生产调用**
  ——只有 `tests/integration/trajectory/test_trajectory_smoke.cpp:486-494` 在用。

**现状缺陷(冻给 P0-1 的靶子)**:同仓库子目录启动裂 workspace;linked
worktree 各裂;与 2A 的 hash 函数、位数、seed、display 名全不同;`/worktree`
切房重开 session 也不换 workspace_key(cwd 变 key 不变,账仍归启动目录的房)。

| 消费者 | 位置 | 用法 | 迁移归宿 |
|---|---|---|---|
| workspace 目录定名 | `src/trajectory/session_manager.cpp:622-624`、`directory.cpp:214-244` | `<trajectories_root>/workspaces/<key>` | P0-1 key 由新 resolver 出(P0-2 挪根);算法换 SHA256 前 16 位 + seed 前缀(单子 §4.3) |
| 事件信封必填字段 | `src/trajectory/event.hpp:200`、`event.cpp:363,491`、`schema.cpp:430-431` | envelope.workspace_key 校验 | P0-1 换新 key(同一场内不变) |
| recorder 边界检查 | `src/trajectory/recorder.cpp:767` | scope 与 base 不合拒绝写 | 保留,换 key 来源 |
| replay/training/export | `replay.cpp:112,491,794,839`、`training_exporter.cpp:254-1141` | 投影与训练元数据 | 保留,吃新 key |
| metrics/usage_gc/doctor | `metrics.cpp:89-116`、`usage_gc.cpp:91` | 报告 | 保留 |
| accounting | `usage_sample.hpp:64`、`session_usage_reader.cpp:106-130`、`usage_projector.cpp:148` | usage 样本 | 保留 |
| telemetry | `telemetry/projector.cpp:142,790`、`contract.hpp:133,144`、`identity.hpp`(不裸 hash) | 假名属性 | 保留(假名化不动) |
| insights | `report_model.hpp:27`、`integrity_gate.hpp:33`、`redaction.cpp:226` | 报表域 | 保留 |
| session_manager 内部 | `session_manager.cpp:622-1843`(十二处赋值/透传) | manifest、intent、result | 保留结构,来源换 resolver |

### 2C. `NormalizePathForCompare`(第三把 cwd 比较键)

- 生成/消费:`src/sessions/session_store.hpp:283-286`;`interactive_session.cpp:534,543-568`(history_search 过滤)、`ListSessions` cwd_filter、`session_catalog`。
- 与 2A/2B 无血缘:只做 weakly_canonical + 斜杠/大小写归一,不带 hash。
- 归宿:P0-2 随旧档扫描退场;P0-1 的 `checkouts[].root` 比较与 workspace
  索引过滤吃 identity 的规范根,归一逻辑并入新 resolver 部件。

### 2D. `thread_id`(app-server 会话 id,同形物)

- `src/app_server/server.hpp:57`:"thread_id = SessionStore 的会话 id"——
  与 CLI 会话 id 同一命名空间(yyyymmdd-HHMMSS-slug),不是独立钥匙。
- 归宿:P0-2 thread API 直通 workspace session id(`sessions/<session_id>/`),
  旧 id 由迁移器原样带入 `start_reason=legacy_import` 场。

### 2E. 全局 memory(无 key)

- `~/.lubancode/memory/user/`(project_memory.cpp Options.user_enabled、
  `memory/user` 目录、preferences/feedback 两层)不挂任何项目 key。
- 归宿:路径不动,只升级 schema/权限合同(P0-4);禁止项目层借 key 写。

---

## 表三:用户旧数据的迁移归宿(验收线:每一种都有家)

| 旧数据 | 现位置 | 迁移归宿 | 批次 |
|---|---|---|---|
| 会话消息/事件 JSONL | `sessions/<id>.jsonl` | `workspaces/<key>/sessions/<新id>/main.jsonl`(`start_reason=legacy_import`),缺口标 `legacy_partial`、`subagent_detail=unavailable_legacy`、训练 `exclude` | P0-5 |
| 标题/压缩(v1+v2)/queue/mode/plan/think 事件 | 同上文件内 | 对应 typed trajectory 事件;不可还原的执行边界标 `legacy_partial` | P0-5 |
| 工具结果(含孤儿对) | 同上 | `tool.use/tool.result` 事件 + RepairToolPairs 保真;trace 账(tool_trace_v1)按四档结论导入 | P0-5 |
| MCP rich result 引用块 | `sessions/<id>/mcp-artifacts/*` | `sessions/<新id>/artifacts/sha256/<hash>`,引用块改内容寻址;字节齐的搬字节,缺的照实列缺口 | P0-5 |
| 渐进上下文仓 blob/chunk | `sessions/<id>/context/*` | blob 进 `artifacts/sha256/`(内容寻址天然去重);chunk 索引可重建,不迁 | P0-5 |
| 模型图片 | `sessions/<id>/images/*` | 同 mcp-artifacts 路 | P0-5 |
| 归档会话 | `sessions/archive/*.jsonl` | 同普通会话迁入,`start_reason=legacy_import` + 状态 archived(workspace 索引标注) | P0-5 |
| 项目 Memory 主题(front matter/HTML 头) | `projects/<旧project_key>/memory/` | `workspaces/<新key>/memory/`,逐主题校验 front matter、正文 hash、证据路径;留 source/target hash 对照 | P0-5(§7.3) |
| memory 候选 | `projects/<key>/memory-candidates/*.json` | `<workspace>/memory/memory-candidates/` | P0-5 |
| memory-jobs(pending/failed) | `~/.lubancode/memory-jobs/` | 路径保留(全局件);P0-3 起项目类 job 进 workspace,存量 pending 按 job 内 project_key 路由到对应 workspace | P0-3/P0-5 |
| 用户级 Memory | `~/.lubancode/memory/user/` | 不搬,升 schema/权限合同 | P0-4 |
| trajectory v1 账 | `trajectories/workspaces/<旧key>/...` | 根改 `workspaces/`(P0-2);旧 key 与新 key 算法不同的场,由 `lubancode workspace migrate` 显式并账,不自动改名合并 | P0-1/P0-2 |

---

## 已核验的现状结论(给 P0-1/P0-2 的直接输入)

1. **两把钥匙哈希函数不同**:memory 用 64 位 StableHash 出 16 hex;trajectory
   用 SHA256 出 12 hex。P0-1 统一为 SHA256 前 16 位 + 三种 seed 前缀。
2. **trajectory 的 Git 探测是死码**:`FindWorkspaceRoot` 无生产调用;现状
   workspace_root 恒为启动 cwd。P0-1 接线时注意:不能只"换成 FindWorkspaceRoot",
   要按单子 §4.2 四级裁决(commondir → marker → config → cwd)整体替换。
3. **旧写口的短路已双轨**:trajectory_enabled 开时 SessionStore::Begin 不建
   档(session_runtime.cpp:52)、PersistNew 短路(:93)、app-server 不开旧档
   (server.cpp:721)。P0-2 只删分支,不新增行为。
4. **`trajectories` 根与 `sessions` 根各自独立解析**,没有共享的"主目录根"
   常量;P0-2 挪根时两处都要改(trajectory_session.cpp:1266、cli_app.cpp:374)。
5. **memory-jobs 是全局件**(住主目录),项目 Memory 迁 workspace 后 job 路由
   要跟着改,别漏 pending/failed 两态。
