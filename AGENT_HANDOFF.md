# AGENT_HANDOFF —— Workspace P0-5/P0-6 封机存照(2026-09-01)

分支 `worktree-agent-ade42dd8de3c3d8be`，基线 `ad721e6d`（main，已 merge）。
本单《Workspace统一存储_旧Session退场与分级Memory迁移.todo》P0-5+P0-6
两批连做，**均未完**。本文件是接手现场。

## 一、已完成（可编译、自洽）

1. **通读与摸底全毕**：单子 P0-5/P0-6/§七、P0-0 contracts/inventory、
   P0-2/P0-3 交接要点全部消化。旧 project_key 算法（FNV-1a 64 + SafeName，
   见 `git show 9d006066:src/memory/project_memory.cpp:1811`）已复刻。
2. **SessionManifest 补合同 §三两键**：`src/trajectory/directory.hpp/.cpp`
   增 `subagent_detail`/`training_policy`（空串不落键，legacy_import 场必填）。
3. **迁移器主体写完并编译过**：
   - `src/workspace/storage_migrator.hpp/.cpp`（约 1800 行）：plan/run/status
     三口；intent/progress/result 回执照合同 §五；单场导入器
     （turn/request/tool 全事件链、tool_trace_v1 四档结论融进终态、
     rich 块无损投影、mcp-artifacts/images 字节搬运改内容寻址、compact/
     title/cwd/queue/mode 映射、goal/plan/think 照实列缺口）；旧 Memory
     主题迁 schema 3 + source_sessions 裸 id 升四段引用 + migrated_from 登记；
     `--delete-source --yes` 二次确认逐件复验；故障注入 hook（fault 回调
     按耐久点抛 MigrationInterrupted）。
   - 幂等/续跑已实现：committed SHA 索引去重（already_imported）、
     progress 逐文件落账、半截目标 session 目录删除重建、
     run 续跑最近未 committed operation。
   - 复验内置：每场 imported 前过 VerifySessionDir + FoldStreamReplay。
4. **命令面接好**：`lubancode migrate-storage <plan|run|status>
   [--operation <id>] [--project-root <路径>] [--delete-source --yes]`
   （cli_options.cpp/cli_app.cpp/src/cli/migrate_storage_command.*）。
5. **独立封存体**：`tools/legacy-storage-migrator/main.cpp` + CMake target
   `lubancode_storage_migrator`（独立静态库，链 engine；`legacy-storage-migrator`
   exe 不随发行包安装）。app 链了 migrator（过渡窗；收官摘链即“移出生产构建”）。
6. **构建状态**：worktree build/release（VS2022 Release）全目标编译通过
   （migrator 库、legacy-storage-migrator、lubancode_app、lubancode）。
   **未跑 ctest**（封机）；**未跑 WSL 语法验**（侧机无依赖头，MSVC 全编译过，
   接手后补）。

## 二、未完成（P0-5 余项）

- [ ] **测试一册未写**：计划落 `tests/unit/workspace/test_storage_migrator.cpp`
      （夹具直接用 `tests/fixtures/workspace/legacy/*.jsonl` 九件套 + memory/）。
      验收线“打断一百次”用 fault hook 循环注入（点序见 hpp 注释：
      session_created/event_committed/session_closed/file_imported/
      memory_topic/result_committed/source_deleted），每轮后断言：
      ① 旧源逐字节不变 ② 续跑不重样 ③ 最终 result committed 且目标
      verify+replay 全过。另加真进程 kill 折算（10-20 次）如实记录方法。
- [ ] plan/run/status 的单测断言（幂等、already_imported、sha_mismatch、
      delete-source 无 --yes 拒、memory unmappable 列账）。
- [ ] `docs/getting-started/storage-migration.md` 未写（迁移说明 + 最后兼容
      版本边界 + 独立工具用法）。
- [ ] **已知实现风险（接手先看）**：
  - `ImportOneSession` 里 `identity_cache` 已挪进 MigratorContext（无 static），
    但 ImportOneSession 失败路径会留下半截目标目录（设计如此：无回执即删）。
  - CompactApplied 的 old/new_state_hash 用导入器自维护的
    `effective_history_key_`（角色+content dump 拼串）做锚，不是生产
    HistoryStateHash 口径——已注释说明，若要换口径改 SessionImporter。
  - 混排 user 消息（text+tool_result 同条）走 ImportToolResultMessage 的
    lead_in 路径；旧档罕见，夹具没有此形状。
  - `std::rand()` 起 operation_id 尾随机（非加密需求）。
  - SessionImporter 的 turn 收口对悬空 call 补“[迁移导入:结果缺失]”错误
    结果（与旧 RepairToolPairs 语义对齐）。
- [ ] 勾单子 P0-5 checkbox + 状态行（未动）。

## 三、未完成（P0-6 整批未动）

单子 P0-6 七条 checkbox 一条未做。摸底结论（接手直接用）：

- **旧路全是“`if (ctx.trajectory != nullptr) {新} else {旧}`”形状的死分支**
  （P0-2 已恒开账本）。重点文件与行号（基线时点）：
  - `src/runtime/session_runtime.hpp/.cpp`：store_/persisted_count_/meta_/
    title_/store_broken_/pending_mode_event_/pending_think_history_ 成员、
    PersistNew/PersistNewWithProvenance/ClampPersisted/EnsureBegun 的死体、
    Options.sessions_dir；
  - `src/app/commands/session_commands.cpp`：ResumeSession(735)、
    HandleExportCommand(917)、HandleClearCommand/HandleTitleCommand/
    HandleResumeCommand(994-1082)、HandleSlashExport(2180) 的 else 分支；
  - `src/app/commands/{command_registry,goal_commands,loop_commands,
    memory_commands,session_commands,trace_commands}.hpp` 的
    `sessions::SessionStore* session_store` 字段；
  - `src/app/session_title_account.*`、`src/app/wirings/goal_session_wiring.*`
    (MakeSessionLedgerSink 消费者)、`loop_session_wiring.hpp`、
    `plan_session_wiring.hpp`、`src/app/interactive_session.cpp`(591-600,
    EnsureSessionBegun/PersistNewMessages)、
    `interactive_session_controller.hpp`(458,430)、`mention_support.cpp`
    (NormalizePathForCompare 当路径工具用——迁到 platform 或 tools)、
    `src/app/one_shot.cpp`、`src/app_server/server.hpp`(80 record->store)、
    `src/evolution/collector.cpp`(86 旧档扫描——evolve 只读新账)、
    `src/runtime/tool_trace_hub.*`(store_ 指针)、
    `src/runtime/goal_coordinator.*`(MakeSessionLedgerSink)、
    `src/runtime/session_command_service.*`、`src/cli/history_search.*`；
  - CMake 删 `src/sessions/session_store.cpp` 等；`src/sessions/` 只剩
    goal_session.cpp（goal 事件纯函数，仍有消费者：goal_coordinator/
    adapters）——按单子 §十二逐件判归宿，不一锅删。
- 静态审计（P0-6 第 7 条）：扫 src/ 与 docs/ 的生产路径
  `~/.lubancode/sessions|trajectories|projects`，白名单：迁移器三件 +
  storage-migration.md + workspace-storage-v2 开发文档。
- features.trajectory 文案残留：`src/cli/i18n.cpp:702-706,2487-2491` 等
  （P0-2 已删开关，文案改“账永远在”）。
- 注意：**迁移器 `storage_migrator.cpp` 合法持有旧路径字样与旧格式解析**，
  删码时别误伤；P0-6 后它仍在（隔离 target），收官发行才摘 app 链接。

## 四、接手顺序建议

1. `git merge main`（若有新提交）；全量 build + ctest 对账基线
   （**main 上 Wave3 有 16 册成片红**（trajectory 边界 prepared 空串一类，
   tests/unit/trajectory/test_runtime_wiring.cpp），主控已派专项缝合
   （agent a17fd50fb44cdfe5a）——撞到同批红不算本单回归，分开记账）。
2. 写 P0-5 测试册（见上），跑绿；fault 注入循环 100 次（hook 法）+
   kill 折算；补 storage-migration.md；勾 P0-5 checkbox（commit 1）。
3. P0-6 删码：按上面清单逐文件拆 else 分支/成员/字段；CMake 收项；
   删 session_catalog/session_lifecycle/session_command_service 的平铺扫描
   （session_commands 的 archive/delete 已走 SessionAdminOutcome 自由函数）；
   `ParseSessionFile` 等消费者（trace_commands:61,134、goal_session_wiring:93、
   tool_trace_hub.cpp:327、collector.cpp:86）全部改吃新账或删；
   goal_session.cpp 判归宿。
4. 静态审计测试（守门式，类似既有源码扫描测试）；勾 P0-6 checkbox
   （commit 2，中文 commit + Co-Authored-By: Claude <noreply@anthropic.com>，
   不 push，版本号不动）。
5. 全量 ctest 对账（删旧册前后各一次）。

## 五、环境备忘（本 worktree）

- build/release 已 configure（VS2022 Release，LUBANCODE_BUILD_TESTING=ON，
  361 册测试）；_deps/_vendor 从主仓 build/release 拷的。
- ctest 记得设临时 USERPROFILE；worktree 缺 rg 从主仓
  build/release/libexec 拷；LNK1104 假锁 taskkill 清孤儿。
- 动了头文件结构（directory.hpp）后续 build 用 --clean-first 防陈旧对象。
