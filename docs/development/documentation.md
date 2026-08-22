# 文档规范

[文档首页](../README.md) · [开发指南](build-and-release.md) · [测试指南](testing.md) · [命名规范](naming.md)

这页管 `docs/` 怎样分工、怎样写、怎样跟代码同日落地。它不规定中文文风；它规定事实从哪来，页面该收什么，改到什么地步才算交活。

## 1. 文档类型

| 类型 | 回答什么 | 例子 |
| --- | --- | --- |
| 入口 | 我该从哪儿开始 | `README.md`、`reference/feature-index.md` |
| 指南 | 怎样完成一件事 | `features/project-instructions/README.md`、`features/extensions/README.md` |
| 参考 | 字段、命令、参数究竟是什么 | `reference/commands.md`、`reference/configuration.md`、`reference/tools.md` |
| 解释 | 系统为何这样长、数据怎样流 | `architecture/README.md`、`architecture/query-data-flow.md` |
| 规范 | 改动必须守哪些契约 | 本页、`development/naming.md`、`development/security.md` |
| 面试材料 | 项目怎样讲、怎样追问 | 仓库顶层 `interview/`，不随官方文档安装 |

一页先选一种主职。指南可以链接参考页，不要把所有字段再抄一遍；解释页可以画流程，不要替命令页承诺 CLI 语法。

## 2. 权威来源

代码、运行输出、文档、TODO 四者不平级：

1. 当前源码与可复现测试定行为。
2. `--help`、`--config`、slash 帮助定用户可见输出。
3. 专题文档解释行为与边界。
4. `todos/` 只记未来工作，不得当现状引用。

发现冲突时，不在两页之间“取个折中说法”。先查源码与测试，再改错页。

## 3. 页面契约

新页或大改页面，至少有这些东西：

- 一级标题。
- 顶部相关页面导航。
- 一段范围说明：本页管什么，不管什么。
- 成功路径：最小可运行例子。
- 失败路径：常见错误怎样认、怎样查。
- 安全或数据边界；无风险也可明写“只读、不落盘”。
- 相关源码或权威入口；内部设计页尤其要有。
- 与其他页面的交叉链接。

参考页还要写默认值、单位、取值范围与缺省行为。配置字段必须分清“没写”“空值”“false”“未知能力”，不可揉成一个“关闭”。

## 4. 现状、实验与计划

用词要分账：

- **已实现**：主线源码已有，常规构建能得到。
- **条件启用**：须配置、平台能力或外部程序；条件当场写清。
- **实验/手测**：不进默认测试或须真服务；写明启动办法与限制。
- **后续**：尚未实现，只能放“后续”小节或 `todos/`。
- **已废弃**：仍兼容但不推荐；写替代方案与兼容边界。

不得拿 TODO 的目标时态改成功能表的现在时。代码刚合，文档也要等对应路径与测试坐实再改口。

## 5. 版本与易腐数字

长期页面不要反复写“本页对应 vX.Y.Z”。当前版本只认：

```text
src/app/version.hpp
CHANGELOG.md
```

`src/app/version.hpp` 定当前源码版本，CMake 从它读取项目版本；`CHANGELOG.md`
记已备妥的发行说明。不要再往 `CMakeLists.txt` 抄第二枚版本字面量。

也不要在入口、架构、功能总览里硬写测试用例数、断言数、提交数或代码行数。这些数字一过提交便旧。确需展示时：

1. 标日期与 commit。
2. 给出生成命令。
3. 写清是否含跳过项、手测项和外部服务。
4. 放进快照/报告页，不拿来定义产品行为。

## 6. 性能与缓存数据

性能数据须带一只可复测说明块：

| 项 | 必填内容 |
| --- | --- |
| 版本 | commit、编译类型、编译器 |
| 环境 | OS、CPU、内存；模型测试再写服务端与模型 |
| 数据 | 输入规模、轮数、并发、随机种子或夹具 |
| 冷热 | 冷启动、热缓存、两者怎样清理与区分 |
| 指标 | 口径、采集点、单位、原始来源 |
| 样本 | 预热次数、正式次数、P50/P95 或均值与离散度 |
| 限制 | 网络、共享服务、后台负载等干扰 |

供应商 usage、vLLM metrics 与客户端估算不是一把尺，必须分列。单次成功截图只算线索，不算基准。拿不准，宁可留在测试记录，不写进 README。

## 7. 命令与代码块

- Windows 命令用 `powershell` 围栏；POSIX 用 `bash`。
- 跨平台命令分开写，别在一段里混 `\`、`/` 与两套 shell。
- 示例不得含真 key、cookie、用户主目录或线上私有地址。
- JSON 必须能解析；省略字段时用完整注释外说明，不往 JSON 塞注释。
- 输出示例只留能说明契约的几行。超长日志给摘要与定位办法。
- 文件路径、字段、命令、枚举、环境变量都用反引号。

## 8. 链接与标题

- 仓库内用相对链接：`[配置手册](../reference/configuration.md)`。
- 能链专题页便不链首页；能链文件便不写“见相关文档”。
- 改标题前先搜锚点引用。中文标题的自动锚点在不同渲染器间有差异，跨页优先链页面，必要时再链稳定英文小标题。
- 图片放 `docs/assets/`，写有意义的 alt；截图须遮掉密钥、用户路径和私人数据。
- 一级标题每页只放一个。标题层级不跳级。

## 9. 术语与计数

执行层级、计数单位先查[命名与计数规范](naming.md)。几条硬规矩：

- `turn` 是一轮任务，`step` 是一轮内一次模型请求。
- 工具调用次数不叫“轮数”。
- token、字节、字符分开写，不能互换。
- `provider`、`model`、`wire` 各指一层。
- 主代理、子代理、后台命令、PTC runner 不是同一种任务。

新增术语若会跨三页出现，先补词典，再铺正文。

## 10. 改动同步矩阵

| 改动 | 至少检查 |
| --- | --- |
| 启动参数 | `reference/commands.md`、根 README、CLI help、CHANGELOG |
| Slash 命令或键位 | `reference/commands.md`、`features/terminal/README.md`、i18n/help、CHANGELOG |
| 配置字段或默认值 | `reference/configuration.md`、示例配置、`--config` 输出、CHANGELOG |
| Provider/模型 schema | `features/providers/catalog.md`、`reference/configuration.md`、catalog schema/快照 |
| 工具 schema | `reference/tools.md`、`reference/feature-index.md`、prompt 工具方针、测试 |
| Hook 事件 | `features/extensions/hooks.md`、`reference/configuration.md`、扩展指南、事件测试 |
| 会话/compact/memory | 对应专题页、`architecture/query-data-flow.md`、迁移/回放测试 |
| 终端状态机 | `features/terminal/README.md`、`reference/commands.md` 键位表、真终端测试说明 |
| 安全边界 | `development/security.md`、专题页风险段、确认与失败测试 |
| 构建/CI/发行 | `development/build-and-release.md`、`development/testing.md`、根 README |

“至少检查”不等于每页都要改。查过无影响，PR 说明里写一句便成。

## 11. 文档验收

提交前逐项看：

- [ ] 页面主职清楚，没有把指南、参考、未来设计搅成一团。
- [ ] 命令、字段、默认值已向源码或运行输出对账。
- [ ] 成功路和失败路都写了。
- [ ] 安全、密钥、落盘与网络边界没有漏。
- [ ] 没有硬塞易腐版本号与孤零零的性能数字。
- [ ] 相对链接存在，标题锚点没有被改断。
- [ ] 新页已收进 `docs/catalog.txt`；面试材料没有混回 `docs/`。
- [ ] `bash scripts/check_docs.sh` 通过。
- [ ] 中英文 README 若同属用户入口，已判断是否同步。
- [ ] `git diff --check` 通过。

纯文档改动会触发 `.github/workflows/docs.yml`，检查目录、断链与官方 Skill 路由。它不编 C++。文档若伴随代码改动，仍须照代码风险跑测试。

## 12. 归档与删除

页面失去现职时有三条路：

1. 内容仍有历史价值，移到明确的 snapshot/archive 区并标日期。
2. 内容被新页完整接管，旧页留短迁移说明一版，再删。
3. 内容本就错误且无人引用，直接删，顺手修入口与链接。

不要留下两页同名近义、互相打架。文档也有维护成本，该收便收。
