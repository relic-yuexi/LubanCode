# Agent Handoff — 使用洞察 Token 账本 A1(事实接线)

**写入时间**：2026-08-31（主控会话批次续接期间的安全网）
**写入原因**：主控会话本轮已连撞两次基础设施瞬断（其它并行 worktree 600 秒无进展被判 stalled）。为防止本 worktree 也被清空丢失进度/原始任务，先落一份存照。若你看到这份文件时任务已完工或本 worktree 已被移除，忽略即可。

## 原始任务 Prompt（逐字保留）

你在 LubanCode 仓库（C++ AI 编程 CLI，主仓 D:\lubancode）的隔离 worktree 里干活。本单落《LubanCode 使用洞察：Token 账本与 Prompt 审计设计》的 **A1：事实接线**（阶段 A0 已在本地 main 落地——UsageSample/PromptManifest/Finding/SessionSummary/Report schema 冻结，十二场合成夹具）。开工先 `git merge main --no-edit`——注意本地 main 现已包含 P0-2/P0-3 轨迹接线（SessionRuntime 持 Recorder、AgentLoop 四道边界、v2 usage 事件、ReplayState）。通读 todos/LubanCode使用洞察_Token账本与Prompt审计设计.todo 全文，尤其 §十六批次划分（A0→A1→A2/A3→A4→A5→A6/A7）与 A1 清单本身。

### 任务

按单子自己 A1 批次的清单做（先读单子原文找到 A1 具体条目，多半是）：
1. `ResolvedPromptBuilder`：把 system/tools/prompt profile 的组装过程接进 UsageSample/PromptManifest 的真实数据源（此前 A0 只有 schema + 合成夹具，本批要接上真实运行时数据）。
2. `ModelRequestRecorder`：接进 P0-2 已落的 AgentLoop LoopBoundaryRecorder / v2 usage 事件（`model.usage.recorded`）——不重复造轨迹层，UsageSample 从 Trajectory Journal 投影而来（P0-3 的 ReplayState 已有这类折叠模式，抄它的路数）。
3. purpose 全接线：A0 冻的 12 值枚举要在真实调用点（main turn/subagent/compact/title 等）标注正确 purpose。
4. v2 usage owner 真落账：确保每个 model.usage.recorded 事件有唯一 owner（P0-1a 状态机约束 2 已管，这里是消费侧对账）。

### 关键定夺

- **flag 依赖**：Token 账本的真实数据源是 Trajectory Journal（P0-2 的 features.trajectory 默认关）。若 flag 关，A1 的投影自然拿不到数据——这是合理行为，不是 bug，报告里说明。
- **先读现状**：本地 main 现在 accounting/insights 相关代码在哪，先 `grep -rn "UsageSample\|PromptManifest" src/` 摸清 A0 落的文件结构，A1 是在这基础上接线，不是另起。
- 若单子对 A1 的清单描述比我上面猜的更具体，以单子原文为准。

### 构建与验证（本仓铁律）

- 自家 worktree configure + build + `ctest -C Debug`（-C 必带；VS 生成器；configure 约 10 分钟）。ctest 设临时 USERPROFILE（Windows 路径）。main 基线以 merge 后实测为准；你单内必须全绿；A0 的十二场夹具册与 P0-2/3 轨迹册不许红。
- **三平台验收**：WSL `g++-13 -fsyntax-only -std=c++23 -include src/pch.hpp -Isrc -Iinclude -Ibuild/release/_deps/nlohmann_json-src/include -Ibuild/release/_deps/yaml-cpp-src/include <file>` 过新/改文件；禁例：NSDMI 默认实参、atomic<shared_ptr>、CAPTURE 单参。
- 冒烟：flag 开，跑一轮真实（假后端）turn，投影出真实 UsageSample/PromptManifest，与合成夹具的 schema 对得上。
- 解冲突先 `grep -c '<<<<<<<'`；合并后亲核 exe 时间戳。

### 规矩

- **上游不 push**：完工 commit 落本地即可。
- 代码与夹具不写真实密钥、绝对路径。
- 不做 A2 起的批次（/usage 命令、/prompt audit 等）。
- 不做 git 操作，唯一例外：完工后单笔 commit，中文提交信息，末尾加 Co-Authored-By: Claude <noreply@anthropic.com>。版本号不动。

### 报告

A1 清单逐条落法、与 A0/P0-2/P0-3 的接线关系图、flag 关时的行为说明、purpose 接线点清单、冒烟输出、ctest 通过数、WSL 验结果、遗留疑问。

## 当前状态（2026-08-31 更新：已 stalled，有真实未提交进度，别丢弃）

Agent 已因基础设施瞬断（600 秒无进展，stream watchdog 没救回来）被系统判 failed。**working tree 里有真实进度，不是半写坏的语法错误**，`git diff --stat` 显示 11 个既有文件改动（258 行）+ 2 个新文件（228 行）：

```
 CMakeLists.txt                           |  4 ++
 src/agent/agent.hpp                      | 15 ++
 src/agent/loop.cpp                       | 54 +++---
 src/agent/loop.hpp                       | 32 +++
 src/agent/prompt_assembler.cpp           | 64 +++----
 src/agent/prompt_assembler.hpp           | 13 ++
 src/app/interactive_session_assembly.cpp |  9 +-
 src/runtime/trajectory_session.cpp       | 74 +++++--
 src/runtime/trajectory_session.hpp       |  5 +-
 src/tools/agent_tool.cpp                 | 10 ++
 src/workflow/host_executors.cpp          |  8 ++
?? src/agent/resolved_prompt_builder.cpp   (169 行,新)
?? src/agent/resolved_prompt_builder.hpp   (59 行,新)
```

### 已完成的工作

**`ResolvedPromptBuilder`**（对应清单第 1 条）已成型，设计说明写得很清楚（见 `resolved_prompt_builder.hpp` 头注）：
- 两段式：`BuildResolvedPromptBase`——构造期/`/clear` 重建时跑一次 `AssembleSystemPrompt` 五层回路，产原始文本 + `PromptSourceLedger`；`ResolveFinalPrompt`——每次请求现叠三层后叠（deferred tool index → model instructions → soul，次序与 `agent/loop.cpp` 原有三行调用一字不差）。
- 明确"manifest 从真实拼装现场直接产出，不靠 analyzer 事后拆字符串猜模块边界"（§6.4 原话）。
- `agent/loop.cpp`（+54/-改动）、`agent/loop.hpp`、`agent/prompt_assembler.{hpp,cpp}`、`agent/agent.hpp` 都有配套改动，看起来是把 ResolvedPromptBuilder 接进了实际请求路径。

**purpose 接线**（对应清单第 3 条）疑似在推进：`src/tools/agent_tool.cpp`（+10 行）、`src/workflow/host_executors.cpp`（+8 行）都有改动，与派活 prompt 里点名的"workflow node 的 purpose 接线"方向一致。

**`ModelRequestRecorder`/usage 接线**（对应清单第 2/4 条）：`src/runtime/trajectory_session.{hpp,cpp}`（+74/+5 行）改动量不小，疑似在接 `model.usage.recorded` 事件与 owner 账，具体做到哪一步需要接手人细读 diff 确认。

`src/app/interactive_session_assembly.cpp`（+9 行）估计是接线组装入口。

**尚未验证**：agent 死在这一步，没能确认这批改动能否过 configure/build/ctest——接手人第一件事应先跑一次构建，看有没有半接的接口不匹配。

## 接手须知

## 接手须知

- 本 worktree 从 `git merge main --no-edit` 起步，隔离分支，主仓在 D:\lubancode。
- 完工后按老规矩：单笔 commit，中文提交信息，末尾 `Co-Authored-By: Claude <noreply@anthropic.com>`；**不 push**，落本地即可，由主控会话统一验收合并。
- 验证铁律：`ctest -C Debug` 全绿 + WSL `g++-13 -fsyntax-only` 语法验 + 解冲突先 `grep -c '<<<<<<<'` 点数、解完全仓验零残留 + 合并后亲核 exe 时间戳防陈旧假绿。
