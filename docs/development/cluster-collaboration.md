# 分布式工程协作与远端验收

[开发手册](README.md) · [测试指南](testing.md) · [文档规范](documentation.md)

本文约定分支、分工与验收流程。协议、存储和隔离方案仍待讨论，不凭这页定稿。
核查日期：2026-09-26。源码基线：`893e2b1a`；路线图 rebase 后：`457e9cf5`。
本轮只读源码、整理文档，不在本地编译。

## 1. 分支怎样走

`feature/distributed-support-roadmap` 承接分布式工程。各项工作从它分出子分支，
各开一条 PR，目标都选这条集成分支。总 PR [#234](https://github.com/relic-yuexi/LubanCode/pull/234)
继续指向 `main`，保留 Draft。子 PR 过门，不等于总工程过门。

```text
main
  └─ feature/distributed-support-roadmap ──────────── 总 PR #234 → main
       ├─ codex/cluster-d0-kickoff ────────── 子 PR ──┤
       ├─ codex/cluster-d0-contracts ──────── 子 PR ──┤
       ├─ codex/cluster-ci-gates ──────────── 子 PR ──┤
       ├─ codex/cluster-d1-protocol ───────── 子 PR ──┤
       └─ 后续状态机、存储、传输分支 ──────── 子 PR ──┘
```

图中除当前启动分支外，其余名称只是分工建议。先定每批范围和接口，再开实现分支。
不要先铺满分支，等接口一改，整排返工。

- 一条子分支只交一批。PR 写明目标、依赖、验收项和未过项。
- 每个智能体独占一处工作树。共享目录只读；公共文件指派一名维护者。
- `CMakeLists.txt`、测试登记和 CI 规则由维护者收口，避免多人同时改线。
- 依赖尚未合入时，PR 标清前置 PR。前置合入集成分支后，再同步、重验。
- 不把启动文档当作 D0 合同冻结，也不把 D1 协议库当作分布式功能已交付。

## 2. 先同步，再开工

本轮已把路线图分支 rebase 到 `main` 基线。子分支尚未铺开，适合这次整理。
待多个智能体从集成分支开工，后续同步优先把 `origin/main` merge 进集成分支，
保留共享提交号。确需重写共享历史时，先停下依赖工作，逐条安排后继分支。

新工作从远端集成分支起步。以下命令在干净、专用工作树内执行；分支名按批次替换：

```powershell
git fetch origin
git switch -c codex/cluster-d1-protocol origin/feature/distributed-support-roadmap
```

先讨论并冻结协议字段、错误码、存储接口等共用合同。实现分支只在冻结范围内并行。
若还缺裁决，先交样本、方案对照和待答问题，不擅自写成默认行为。

共享基线更新后，各子分支合入新基线。每次只读最新 PR 头提交对应那轮检查。
合并前还要核对集成分支头部；目标分支已变，旧结果不能独自证明当前组合通过。

## 3. 当前 CI 真正会做什么

下表按现有工作流核查。规则若改，以对应提交为准。

| 场景 | 当前行为 | 边界 |
| --- | --- | --- |
| 子 PR 指向集成分支 | `ci` 会触发。`pull_request` 没有限定目标分支 | 无需先改 CI 才能开子 PR |
| Draft PR | `ci` 没有按 Draft 跳过 | Draft 表示还不能合，不表示省掉检查 |
| 只 push `codex/*` 子分支 | 不触发 `ci` 的 push 入口 | 开 PR 后，后续 push 由 PR 同步事件触发 |
| 只 push 集成分支 | 不触发 `ci` 的 push 入口 | 当前总 PR #234 开着，头提交更新会触发 PR 检查 |
| PR 差异范围 | 取实际 `base.sha`，与检出的 PR 合并结果比较 | 子 PR 看相对集成分支差异；总 PR 看相对 `main` 差异 |
| 纯文档或 TODO | `changes` 仍跑，编译与测试通常跳过 | `docs` 另按路径触发；TODO 不在它的路径范围内 |
| 普通源码 PR | 默认跑 macOS；特定敏感路径加 Windows | 不能把普通 PR 全绿说成三平台全绿 |
| Linux 全量构建与测试 | 只在 `main` push 且命中代码变化时跑 | 目前要等进 `main` 才验，不适合本工程最终验收 |
| 手动派发 `ci` | 跑 Windows、macOS；可填测试过滤式 | 当前不会打开 Linux 全量腿，也不会打开 ASan、TSan 腿 |
| 只改 `schema/cluster/**` | 不命中当前 `code` 路径规则 | 目前没有集群 schema 样本校验任务 |

源码入口：[`ci.yml`](../../.github/workflows/ci.yml)、
[`docs.yml`](../../.github/workflows/docs.yml)、
[`shells.yml`](../../.github/workflows/shells.yml)。

`ci` 按来源仓库与头分支共享并发槽，新一轮会取消旧一轮。
同一头分支不要同时向集成分支与 `main` 各开一条待验 PR；两条会争同一槽。
总 PR 与各子 PR 头分支不同，互不抢槽。`docs` 和 `shells` 按各自 ref 分槽。

ASan 只在既定路径改动时触发，目前只测 `unit.agent` 和 `unit.runtime`。
TSan 只在 Workflow 路径改动时触发，目前只测 Workflow。
新增集群代码不会自动得到集群并发检查；后续要单独接线。

## 4. CI 先补哪几处

首条实现 PR 前，先商定并补齐这些门。本文只列改法，尚未改工作流。

1. **集群 schema 校验。** 独立校验正、反例，覆盖 schema 变更。
   单靠文档链接检查，验不了字段合同；只把 schema 算成代码变化，也不会自动校验样本。
2. **合入 `main` 前验三平台。** 给集群相关子 PR 或集成验收增加明确入口，
   打开 Windows、macOS 和 Linux 全量测试。若用手动全量开关，Linux 构建与各发行版运行任务要一起接线。
3. **新增代码有对应测试。** 编译开关若默认关闭，CI 必须显式打开；
   测试过滤式要匹配实际名称，并在零测试时失败。不能只编现有内核，就算新模块过门。
4. **并发与故障注入另验。** D1、D3.5 起逐批接状态机、租约、仿真种子与失败轨迹。
   D3 数据库合同另跑真库；内存模型不能顶替崩溃恢复验收。

不用为每条 `codex/*` 分支再添 push 触发。子 PR 已有入口，贸然加 push 会重复跑。
若新增 push 入口，须同时复核差异基点：现有非 `main` push 以 `origin/main` 为基点，
它与子 PR 的集成分支基点不同，共享并发槽时会相互替换。

2026-09-26 通过 GitHub API 核查：`main` 未配置分支保护，仓库 rulesets 为空。
检查失败并没有远端硬门禁拦住合并。本轮不改这些设置。是否给 `main` 与集成分支
配置保护、固定哪些必需检查，留待讨论。动态跳过任务不能直接拼成一套固定必需项。

## 5. 本地边界与远端验收

本轮和后续分布式开发，**本地不编译**。构建、CTest、原生测试和跨平台验证交给远端 CI。
本地可读源码、查 Git 状态、比差异、查文档链接，或跑不触发编译的格式检查。
不要借测试、安装、打包或配置脚本间接启动构建。

本地至少查差异格式：

```powershell
git diff --check
```

提交后 push，再开目标为集成分支的 Draft PR：

```powershell
git push -u origin codex/cluster-d1-protocol
gh pr create --draft --base feature/distributed-support-roadmap --head codex/cluster-d1-protocol --title "cluster: 协议库首批" --body-file pr-body.md
```

`pr-body.md` 由该批负责人先写好。至少包含本批行为、前置 PR、测试范围、
实际验过项、未验项，以及需要用户裁决之处。命令仅示范流程，不代表相关分支已经创建。

每条子 PR 合入前，依次核对：

1. 用户已经裁决该批产品与架构问题；评审范围明确。
2. PR 目标仍为集成分支；前置已合入，基线没有漏同步。
3. 记录最新头提交 SHA、CI 链接和实际执行任务。改动后重新跑，不借上一版绿灯。
4. 必需检查全过。取消、跳过、未触发、零测试都不能冒充通过。
5. 测试产物能复查。故障用例保存种子、事件轨迹或数据库恢复证据。
6. 合入后核查总 PR 最新组合；只在本批真实通过时回填路线图勾选。

子 PR 合并策略优先选 squash，使一批对应一条集成提交；这仍是待讨论默认值。
若后继分支引用前置未合入提交，合并前先约定如何重接，避免 squash 后重复带入补丁。
主 PR 保留 Draft，不自动合入 `main`。最终合并要另做整体验收，再由用户决定。

## 6. 当前基线与首批分工

当前 `main` 头部 `893e2b1a` 的 [CI 运行](https://github.com/relic-yuexi/LubanCode/actions/runs/36170438626)
尚不全绿：Windows 测试失败，远端注释指向 `unit.trajectory.session_status_lifecycle`。
同轮 macOS、Linux 全量与 Linux ASan 已过。这里只记录基线，不认定失败原因，也不归责于集群工作。

首批先交启动单与待裁决清单。D0 复核通过后，再拆出三路：协议与纯状态机、
传输与双向 TLS、存储接口与 SQLite。CI 门禁由独立小批补齐。
三路碰到身份、租约、错误码或序列合同，共用 D0 文档，不各写一套。
完整 D4 节点桥接仍受 LubanCore B/C 与 D1—D3.5 验收门约束。首用远端模式可沿公开进程协议另拆小批，见[远端部署草案](../architecture/remote-agent-deployment.md)；不能借首用验收销掉完整集群的前置门。

分期依据见[分布式支持路线图](../../todos/分布式支持路线图_从单机宿主到集群控制面的分期落地计划.todo)。
待裁决项写回这张单或本轮启动单；裁决落定后，再把有效合同移入架构文档与 schema。
