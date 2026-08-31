# Agent Handoff — P0-4(环境与证据)

**写入时间**：2026-08-31（主控会话批次续接期间的安全网）
**写入原因**：主控会话本轮已连撞两次基础设施瞬断（其它并行 worktree 600 秒无进展被判 stalled）。为防止本 worktree 也被清空丢失进度/原始任务，先落一份存照。若你看到这份文件时任务已完工或本 worktree 已被移除，忽略即可。

## 原始任务 Prompt（逐字保留）

你在 LubanCode 仓库（C++ AI 编程 CLI，主仓 D:\lubancode）的隔离 worktree 里干活。本单落《P0 新轨迹记录、可重放与训练投影设计》的 **P0-4：环境与证据**。开工先 `git merge main --no-edit`——P0-1a/1b/2/3 已落（本地 main：核心仓、SessionManager 换账、单写口接线、Replay/resume/verifier）。通读 todos/P0新轨迹记录_可重放与训练投影设计.todo 全文，尤其 §9.1（run.environment.captured 字段清单）、§9.2（重现等级）、§9.3（side effects 细账）、§五 5.5（verification 事件与 invalidation）、P0-4 清单（§十七）。

### P0-4 清单（照单干）

- [ ] 环境快照、git/dirty patch、工具集、system/config refs。
- [ ] 文件/命令/MCP side-effect 细账。
- [ ] verification 事件与 stale invalidation。
- [ ] outcome.assessed 引 evidence。
- [ ] user-only 文件权限、路径 containment 与 reparse/symlink 防逃逸。
- [ ] 有界队列、背压、blob/配额/磁盘 reserve 与 storage_exhausted 收口。
- [ ] usage、derived-only GC 与 workspace lifecycle/tombstone。
- [ ] recorder/replay/export 聚合指标与 `/doctor trajectory`。

验收（照 todo）：文件修改任务能说明"用什么环境、改了什么、拿什么验过"。

### 关键注意

1. **flag 照旧**（features.trajectory 默认关，P0-2 落的）——全部新事件走既有 recorder 路径，flag 关零红。
2. **P0-2/3 已落的基建先摸清**：LoopBoundaryRecorder、tool 三栅栏、TrajectorySessionLedger、replay 折叠的 evidence 账——你的 verification/outcome 事件是它们的补充，别另起第二套。
3. **安全侧复用**：P0-1a 的 BlobStore 越界校验、IsProcessAlive、目录权限规矩（§12.1 user-only）——Windows DACL/POSIX mode 照 §12.1 落；磁盘 reserve 与 storage_exhausted 按 §12.2。
4. P0-3 遗留顺接：queue 事件入 Journal（steering queue 的 control.queue.item.*——CancelQueuedItems 现回空清单）。
5. `/doctor trajectory` 照 §13.1 只读聚合，不造第二本账。

### 构建与验证（本仓铁律）

- 自家 worktree configure + build + `ctest -C Debug`（-C 必带；VS 生成器；configure 约 10 分钟）。ctest 设临时 USERPROFILE（Windows 路径）。main 基线以 merge 后实测为准；你单内必须全绿；flag 关全部既有册零红。
- **三平台验收**：WSL `g++-13 -fsyntax-only -std=c++23 -include src/pch.hpp -Isrc -Iinclude -Ibuild/release/_deps/nlohmann-json-src/include -Ibuild/release/_deps/yaml-cpp-src/include <file>` 过新/改文件；禁例：NSDMI 默认实参、atomic<shared_ptr>、CAPTURE 单参。
- 冒烟（flag 开）：合成 run.environment.captured 全字段、工具 side-effect 细账（file 的 pre/post hash、command 的 argv/exit）、verification 记录→改文件→invalidated→outcome.assessed 引 fresh evidence；GC dry-run 只碰可重建物。
- 解冲突先 `grep -c '<<<<<<<'`；合并后亲核 exe 时间戳。

### 规矩

- **上游不 push**：完工 commit 落本地即可。
- 代码与夹具不写真实密钥、绝对路径。
- 不做 P0-5（训练导出）/P0-6（发布门）。
- 不做 git 操作，唯一例外：完工后单笔 commit，中文提交信息，末尾加 Co-Authored-By: Claude <noreply@anthropic.com>。版本号不动。

### 报告

环境快照字段全账、side-effect 细账各工具落法、verification/invalidation/outcome 链证据、安全三件（权限/containment/磁盘）落法、GC 次序实测、/doctor trajectory 样例、queue 事件顺接、ctest 通过数、WSL 验结果、遗留疑问。

## 当前状态（2026-08-31 更新：已 stalled，有真实未提交进度，别丢弃）

Agent 已因基础设施瞬断（600 秒无进展，stream watchdog 没救回来）被系统判 failed。**working tree 里有真实进度**，`git status --short` 显示：

```
 M CMakeLists.txt
?? src/trajectory/environment.cpp   (207 行)
?? src/trajectory/environment.hpp   (112 行)
?? src/trajectory/metrics.cpp       (191 行)
?? src/trajectory/metrics.hpp       (76 行)
?? src/trajectory/usage_gc.cpp      (194 行)
?? src/trajectory/usage_gc.hpp      (86 行)
```

CMakeLists.txt 已把 environment.cpp/usage_gc.cpp/metrics.cpp 三个新文件接进 `lubancode_engine` 静态库源列表（`git diff -- CMakeLists.txt` 只 5 行新增，非破坏性）。

### 已完成的工作（对照 P0-4 清单逐项核对）

三份头文件设计完整、注释详尽，是**认真设计过的骨架**，不是占位符（.cpp 也各有 190~207 行实现，非空壳）：

1. **`environment.hpp/cpp`**（对应清单第 1 条"环境快照、git/dirty patch、工具集、system/config refs"）：
   - `GitStatus` + `GatherGitStatus`：git HEAD/branch/dirty/dirty patch/untracked 清单，真 shell 出 git，不在仓库内不报错，如实记 `in_repo=false`。
   - `ToolsetSummary`：工具定义集合的规范摘要（sha256 + 计数），trajectory 层不上探 app/runtime，摘要由调用方算好递入。
   - `EnvironmentSnapshotInput`：§9.1 全字段（version/build/os/arch/locale/tz/cwd/repo_root/git/provider/wire/model/model_parameters/system_prompt_ref/toolset/project_instruction_refs/loaded_skill_refs/plugin_refs/config_snapshot_redacted/allowlisted_env）。
   - `ReplayLevel`（Exact/SourceExactEnvironmentPartial/InputOnly/Blocked）+ `DetermineReplayLevel`：纯函数按 source/environment 两轴齐全度推导 §9.2 四档。
   - `BuildEnvironmentCapturePayload`：拼装 `run.environment.captured` 事件 payload（snapshot_ref/replay_level/gaps 三键），大字段（dirty_patch/untracked）超内联量落 blob。

2. **`usage_gc.hpp/cpp`**（对应清单第 6/7 条"blob/配额... 收口"+"usage、derived-only GC..."）：
   - `SessionCapacityUsage`：§12.2 四笔账（journal/referenced_blob/rebuildable/derived）分类字节数与文件数。
   - `ScanSessionCapacity`/`ScanWorkspaceUsage`：纯只读扫描。
   - `GcPlan`/`PlanSessionGc`：§12.2 定死次序（temp→index→checkpoint→derived），canonical JSONL 与 artifacts/ 下 blob **永不进候选表**（候选生成阶段就没这条路，不是扫到跳过）。
   - `GcScope`（DryRun/DerivedOnly）+ `RunSessionGc`：真删逐条容错，不中断其余候选。

3. **`metrics.hpp`**（对应清单第 8 条"recorder/replay/export 聚合指标与 `/doctor trajectory`"）：
   - `StreamHealth`/`SessionDoctorReport`/`WorkspaceDoctorReport`：扫一场 session 全部 stream（main+subagents+workflows+goals+loops）逐份验链，并入容量账。
   - `HasDiskReserve`：磁盘 reserve 检查，查询失败保守判"空间不足"。
   - 明确设计原则：**不持有任何运行期状态、不设计数器、不常驻**，每次调用现扫现折现报，队列高水位/最近I/O错误这类活跃期状态由调用方（runtime/cli）采好递入。
   - **注意**：metrics.cpp 已写（191行）但 .hpp 里只看到声明，未核对 .cpp 是否已把 `/doctor trajectory` 的实际渲染逻辑写完，接手人自行核对 `FormatWorkspaceDoctorReport` 实现。

### 尚未动工（对照清单，接手人从这里接着做）

- [ ] 文件/命令/MCP **side-effect 细账**（清单第 2 条，todo §9.3）——未见相关文件。
- [ ] **verification 事件与 stale invalidation**（清单第 3 条，todo §5.5）——未见相关文件。
- [ ] **outcome.assessed 引 evidence**（清单第 4 条）——未见相关文件。
- [ ] **user-only 文件权限、路径 containment 与 reparse/symlink 防逃逸**（清单第 5 条，§12.1）——未见相关文件，Windows DACL/POSIX mode 两侧都要落。
- [ ] **有界队列、背压**（清单第 6 条前半）——usage_gc 只管容量账，队列/背压未见落地。
- [ ] **queue 事件顺接**（关键注意第 4 条：steering queue 的 `control.queue.item.*` 入 Journal，CancelQueuedItems 现回空清单）——未见相关改动。
- [ ] 三个新文件目前**只挂进 CMakeLists，未跑过 configure/build/ctest**（agent 死在这一步之前，无法确认能否过编）——接手人第一件事应先 configure 一次确认三个新文件语法过关，再继续往下写。
- [ ] 无单测文件（未见 test/ 目录下有新增的 environment/usage_gc/metrics 测试）。

若你接手时它已完工，忽略本文件即可；若它中途也断线，请核对本 worktree 的 `git status`/`git diff` 是否有更新的未提交进度，别无脑丢弃。

## 接手须知

- 本 worktree 从 `git merge main --no-edit` 起步，隔离分支，主仓在 D:\lubancode。
- 完工后按老规矩：单笔 commit，中文提交信息，末尾 `Co-Authored-By: Claude <noreply@anthropic.com>`；**不 push**，落本地即可，由主控会话统一验收合并。
- 验证铁律：`ctest -C Debug` 全绿 + WSL `g++-13 -fsyntax-only` 语法验 + 解冲突先 `grep -c '<<<<<<<'` 点数、解完全仓验零残留 + 合并后亲核 exe 时间戳防陈旧假绿。
