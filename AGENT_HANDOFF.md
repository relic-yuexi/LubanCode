# Agent Handoff — 端云 Telemetry 首批（**已 stalled，有未提交真实进度，别丢弃**）

**写入时间**：2026-08-31（主控会话批次续接期间的安全网）
**写入原因**：本任务已因基础设施瞬断（600 秒无进展，stream watchdog 没救回来）被系统判 failed。**但本 worktree 里有真实的、未提交的代码改动**——用户明确要求保留现场，不许清空，留给后续接手人直接续做。

## 当前状态（最重要，先看这个）

`git status` / `git diff --stat` 显示三个文件有未提交改动（working tree 干净地停在这一步，没有半写坏的语法错误，均已用 WSL 语法逻辑核对过是合法的增量）：

```
 CMakeLists.txt        | 12 ++++++++++++
 src/config/config.cpp | 17 +++++++++++++++++
 src/config/config.hpp | 10 ++++++++++
 3 files changed, 39 insertions(+)
```

### 已完成的工作

1. **`src/config/config.hpp`**：`Config` 结构体加了 `bool features_telemetry = false;`（紧邻已有的 `features_trajectory`），`FileConfig` 结构体加了 `std::optional<bool> features_telemetry;`。注释已写明设计口径：默认 false 内部预览；只从配置文件来（项目级压全局）；环境变量 `LUBANCODE_TELEMETRY=1/0` 显式压一头（由未来的 `telemetry::ResolveTelemetryActivation` 合成）；紧急总闸 `LUBANCODE_DISABLE_TELEMETRY=1` 只关采集发送不改本字段；首版收窄只支持 `trajectory=true && telemetry=true` 完整路，`telemetry=true && trajectory=false` 由 Activation 报 `telemetry.requires_trajectory`，不暗开 trajectory。

2. **`src/config/config.cpp`**：
   - `ParseFileConfigJson` 里加了 `features.telemetry` 字段解析（布尔校验，非布尔报错）。
   - `MergeConfig` 里加了 telemetry 的项目级压全局合并逻辑，照抄 `features_trajectory` 的同款模式（`telemetry_file` 变量，project_ptr 优先、global_ptr 兜底）。

3. **`CMakeLists.txt`**：在 `lubancode_engine` 静态库的源文件列表里**预注册**了六个尚未创建的新文件（这是计划，不是已完成——**这六个 .cpp 文件在磁盘上还不存在**，先注册是这个 agent 的工作习惯，接手人需要先补齐这些文件 CMake 才能过 configure）：
   ```
   src/telemetry/activation.cpp
   src/telemetry/contract.cpp
   src/telemetry/identity.cpp
   src/telemetry/redactor.cpp
   src/telemetry/projector.cpp
   src/telemetry/otlp_json.cpp
   ```
   注释写明了这六个文件的分工：`features.telemetry` Activation、trace/span/resource/data class 合同、确定性 trace/span id、纯 `TelemetryProjector`（只吃 Journal，不联网）、D1 allowlist-first Redactor、离线 OTLP/HTTP JSON encoder。纯库，不 include app/cli/exporter/云端 SDK；只读 trajectory 合同与 platform 抽象（依赖方向见该单 §25.2）。

**接手时第一件事**：`src/telemetry/` 目录还不存在，CMake 目前会因为找不到这六个文件而 configure 失败。要么先把这六个文件的最小骨架（哪怕只是空的合法 .cpp）建出来让构建先跑通，要么先把 CMakeLists 那段注释掉/移到后面某一步再接——接手人自行判断，别被这个卡住。

## 原始任务 Prompt（逐字保留）

你在 LubanCode 仓库（C++ AI 编程 CLI，主仓 D:\lubancode）的隔离 worktree 里干活。本单落《端云协同可观测架构与 Telemetry 插件设计》的**第一个可落地批次**。开工先 `git merge main --no-edit`，然后**通读 todos/端云协同可观测架构与Telemetry插件设计.todo 全文**，按单子自己的实现分期挑第一个批次做掉——不越过单子定的次序。这份单子状态是"调研与详细设计完成，待实现"，是全新领域，第一批大概率是合同冻结/最小骨架一类。

### 通用背景（派活须知）

- 相关已落基建可复用：P0 轨迹单的 Trajectory Journal（EventEnvelope/hash chain/canonical json，src/trajectory/）——Telemetry 若是"把本地事实账同步/上报"，别重造事实层，投影/复用轨迹层的数据。
- Lua 受控 HTTP 单落的 SecretResolver/BoundedHttpTransport（src/runtime/plugin_http.*、secret_resolver.*）——若 Telemetry 要发外网请求，复用这套受控 HTTP 与 Secret 管理，不另起。
- Package 体系的 Plugin 挂载事务（若 Telemetry 是"插件"形态，走 manifest v2 Lua 或 process Plugin 同一条路）。
- 先摸清这些哪些已存在，复用什么、新造什么，报告里说明——别重复造轮子。

### 任务

1. 按单子自己的批次划分落第一批（报告里说清挑了哪批、依赖关系、为何它先行）。
2. 单子要求的测试/验收照办；有"先冻结契约"就合同+单测先行（照 Lua HTTP 单、多渠道单的先例）。
3. 与既有账（Trajectory/SecretResolver/Package）的复用关系在报告里逐条写明。

### 构建与验证（本仓铁律）

- 自家 worktree configure + build + `ctest -C Debug`（-C 必带；VS 生成器；configure 约 10 分钟）。ctest 设临时 USERPROFILE（Windows 路径）。main 基线以 merge 后实测为准；你单内必须全绿。
- **三平台验收**：WSL `g++-13 -fsyntax-only -std=c++23 -include src/pch.hpp -Isrc -Iinclude -Ibuild/release/_deps/nlohmann_json-src/include -Ibuild/release/_deps/yaml-cpp-src/include <file>` 过新/改文件；禁例：NSDMI 默认实参、atomic<shared_ptr>、CAPTURE 单参。
- 冒烟照单子验收线（假数据/回环为准，不烧真钱、不连真外部服务）。
- 解冲突先 `grep -c '<<<<<<<'`；合并后亲核 exe 时间戳。

### 规矩

- 不连真外部 telemetry 服务/上报端点——本批合同与本地骨架，不接远端。
- 代码与夹具不写真实密钥、账号。
- **上游不 push**：完工 commit 落本地即可。
- 不做 git 操作，唯一例外：完工后单笔 commit，中文提交信息，末尾加 Co-Authored-By: Claude <noreply@anthropic.com>。版本号不动。

### 报告

单子批次划分摘要、你落的批次与理由、与既有账（Trajectory/SecretResolver/Package）的复用关系表、交付件清单、测试/冒烟账、ctest 通过数、WSL 验结果、后续批次排队表、遗留疑问。

## 接手须知

- 本 worktree 从 `git merge main --no-edit` 起步，隔离分支，主仓在 D:\lubancode，**working tree 里的未提交改动就是全部真实进度，别用 `git checkout .` 或类似命令清掉**。
- 先读一遍 `todos/端云协同可观测架构与Telemetry插件设计.todo` 全文（尤其 §25.2 依赖方向、第一批次的具体清单条目），核对上面「已完成的工作」是否踩在单子的第一批范围内，再决定是继续按原计划把六个 .cpp 写出来，还是调整方向。
- 完工后按老规矩：单笔 commit，中文提交信息，末尾 `Co-Authored-By: Claude <noreply@anthropic.com>`；**不 push**，落本地即可，由主控会话统一验收合并。
- 验证铁律：`ctest -C Debug` 全绿 + WSL `g++-13 -fsyntax-only` 语法验 + 解冲突先 `grep -c '<<<<<<<'` 点数、解完全仓验零残留 + 合并后亲核 exe 时间戳防陈旧假绿。

## 接手后收尾记录（2026-08-31，第二任 agent）

- 已按序完成：封存 WIP（commit 8f1f167）→ `git merge main --no-edit`（自动合并成功，唯一 `<<<<<<<` 命中是本文件引用的命令文本，代码零冲突残留）→ 通读设计单全文。
- T0 六件已落齐：`src/telemetry/{activation,contract,identity,redactor,projector,otlp_json}.{hpp,cpp}`，测试六册在 `tests/unit/telemetry/`，OTLP golden 夹具在 `tests/fixtures/telemetry/v1/`。
- 构建细节：本 worktree configure 时 curl 下载被网络卡死，已把主仓 `build/release/_deps/*-src` 与 `_vendor/curl-curl-8_10_1` 拷进 `build/debug/`，配 `FETCHCONTENT_SOURCE_DIR_*` 指过去（见 build/wsl_telemetry_syntax.sh 旁的 configure 命令史）。后续重新 configure 记得带同样参数。
