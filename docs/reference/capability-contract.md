# 能力裁剪与工具扩展冻结合同

[文档首页](../README.md) · [工具参考](tools.md) · [Agent 与 Prompt Profile 契约](agents.md) · [app-server 协议](../features/app-server/README.md)

本文是冻结合同。工具怎么裁、功能怎么关、能力快照记什么、一场会话的运行材料活多久、受理真账写到哪一档,一概照此办理。设计出处见 `todos/工业化多协议接入_能力裁剪与工具扩展统一设计.todo`(P0 阶段冻结)。

**状态标注纪律**:每节标〔冻结〕或〔拟议〕。冻结=本文定死,实施照抄,改字要开兼容单;拟议=方向已定、细节随对应阶段实施修订,读者不得当已实现。本文所有字段、方法、事件在 P0 阶段均无生产实现;当前源码行为以 [功能总账](feature-index.md) 与 §六 盘点栏为准。

## 1. 术语冻结

| 名称 | 含义 |
| --- | --- |
| 功能(feature) | 一类宿主能力的总闸,如 `browser`、`mcp`。关了它,工具、方法、提示材料、后台资源一起关 |
| 工具供应 | 引擎侧有哪些工具可装(编译、安装、信任三关全过) |
| 工具授权 | 哪个主体在哪个范围可以用哪只工具。供应不等于授权 |
| 免确认 | `permissions.allow_tools` 一类"不再问一次"的档;不参与授权,不添权限 |
| 暴露策略 | 已授权工具怎么给模型看:direct/deferred/host_only。只改呈现,不改权限 |
| 能力计划(CapabilityPlan) | 解析阶段的产物:选了哪些功能、工具、扩展依赖。纯数据,无外部启动副作用 |
| 能力快照(SessionCapabilitySnapshot) | 一场会话采用的那份计划及版本戳。发现、暴露、提示、执行共同消费它 |
| RuntimeBundle | 一场会话一份的运行材料:backend、工具实例、MCP/插件连接、取消与日志接点 |
| 部署档(deployment profile) | 托管部署的服务级配置:监听器、功能面、工具选择。见 §3 |

## 2. ToolPolicySpec〔冻结〕

### 2.1 新 schema(schemaVersion 1)

```json
{
  "tools": {
    "mode": "only",
    "allow": ["mcp:tools-approved:echo"],
    "deny": []
  }
}
```

| mode | 语义 | allow 约束 |
| --- | --- | --- |
| `inherit` | 继承上层有效工具面 | 必须省略或空;带非空 allow 是配置错误 |
| `only` | 只取 allow 指定集合 | 必填;空数组合法,表示零工具 |
| `only` 且 `allow: []` | 零工具但走显式声明 | — |
| `none` | 零工具 | 必须省略或空 |

三条铁律:

- `deny` 三种模式都可附,与任何 allow 交叠时 deny 胜出。
- Agent 定义(`AgentToolRules`,src/agent/agent_definition.hpp:77)与部署档用**同一解析器**;父子、层级之间只做求交,不写第二套算法。
- 未知 mode 值、未知工具名、冲突别名一律配置错误,拒绝整个计划,不忽略后落成全工具。

### 2.2 旧 schema 转换〔冻结〕

旧 Agent 定义没有 `mode` 键,`allow` 空=继承父面(G05)。换算规则:

| 旧写法 | 换算 |
| --- | --- |
| 无 `tools` 段 | `mode: inherit`,无 allow/deny |
| `allow` 缺省或空数组 | `mode: inherit` |
| `allow` 非空 | `mode: only`,`allow` 原样 |

不混读:文件里有 `mode` 键就是新 schema,按新规矩验;没有就是旧 schema,按上表换算。旧 schema 的空 allow **永远不解释成 none**——换算在导入时显式完成并记录,不静默改语义。旧 schema 不因此废弃;`AgentToolRules` 解析路不动,转换只发生在部署档消费侧(〔拟议〕P2 实施)。

### 2.3 无工具会话〔冻结〕

`mode: none` 或 `mode: only, allow: []` 的会话:不挂 tool_search/tool_invoke,不启动 MCP 握手,不注入工具提示段。模型拿到的请求没有 tools 定义。这与"有工具但延迟暴露"(deferred)是两回事,不得混同。

## 3. 部署档 schema 1〔冻结,样例为 golden〕

权威样例:`tests/fixtures/capability/profile.minimal-tools.json`(最小测试工具集)与 `tests/fixtures/capability/profile.zero-tools.json`(零工具)。机器可读 schema:`tests/fixtures/capability/deployment.schema.json`。静态校验测试:`tests/unit/capability/test_capability_contract.cpp`。

顶层字段:

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `schemaVersion` | 整数 | 必须为 1;未知值拒绝 |
| `service` | 对象 | 监听器、渠道账号、队列消费者、默认档。本阶段只校形状与枚举,不启动任何监听 |
| `harnessProfiles` | 对象 | 键=档名;值=一份档 |

档内字段:

| 字段 | 约束 |
| --- | --- |
| `agentRef` | canonical Agent 引用,如 `example.tools:assistant`;P2 起点名必解析(AgentCatalog),找不到/不可用拒启,不回落默认提示词 |
| `features` | `default: enabled|disabled` 加 `enabled`/`disabled` 名单;名单值须在 §4 功能名表内;同键两名单同现是配置错误 |
| `components.mcpServers` | 名单;点名未在上层获准的服务是配置错误,不给档内授权 |
| `components.plugins` | 名单(canonical 插件名,即 manifest.id;不重复点名)。点名须 `features` 放行 `plugins`;P5 起进入真装载——发现根(材料根 `plugins/`)扫描 → 信任账(`PluginTrustStore`,数据根 `plugin-trust.json`,装配只读不批)→ v2 embedded-lua 挂载(`ManifestLuaRuntime`)。点名件不在发现账报 `plugin_missing`、信任账不过(未信任/disable/指纹算不出)报 `plugin_untrusted`、Lua 挂载坏报 `plugin_load_failed`、点名 process/native 件报 `component_unavailable`——四路整场明拒(thread/start 错误信封带 `data.code`,additive),不忽略不降级(应用Worker接入单 §7.2) |
| `tools` | §2 ToolPolicySpec;P2 起 `allow` 另收内置 skill 工具裸名 `skill`(须 `features` 放行 `skills`);P5 起另收插件工具名 `plugin__<id>__<tool>`(须 `components.plugins` 含 `<id>` 且 `features` 放行 `plugins`;装配期另有 manifest 全名精确对账兜底) |
| `exposure.default` | `direct|deferred|host_only` |
| `updates` | `defaultApplyAt`、`allowSessionExpansion`;首版只支持 `next_session`,`allowSessionExpansion` 只可为 false |
| `limits` | 非负整数;超出上层上限按明确规则拒绝或收窄,须在结果里给有效值 |

依赖解释是校验的一部分,不是可选项:`tools.allow` 里的 `mcp:<server>:<tool>` 名字要求 `components.mcpServers` 含 `<server>` 且 `features` 放行 `mcp`;裸名 `skill` 要求 `features` 放行 `skills`;`plugin__<id>__<tool>` 名字要求 `components.plugins` 含 `<id>` 且 `features` 放行 `plugins`;`exposure.default: deferred` 要求发现器依赖(tool_search/tool_invoke)未被禁,零工具面(`none` 或 `only`+空 allow)配 `deferred` 是配置错误——没有可延迟暴露的东西。解释不全的档不许采用。

## 4. 功能名表〔冻结〕

首批十六枚,一枚功能一枚键,不收同义词:

| 功能名 | 关什么 | 当前部署入口(2026-09-13 复核) |
| --- | --- | --- |
| `filesystem.read` | 读文件类工具 | 内置工具表;无独立开关(留位) |
| `filesystem.write` | 写文件/编辑工具 | 内置工具表;无独立开关(留位) |
| `process.exec` | run_command 子进程 | run_command 工具+command_permission 前缀档 |
| `ptc` | PTC(python 树子调用) | ptc_tool;子调用同过 RunOneTool |
| `web` | web_search/web_fetch | config `search` 段配置即供应 |
| `browser` | 内嵌浏览器与截图取件 | BrowserRuntime;见 [browser-runtime.md](browser-runtime.md) |
| `lsp` | LSP 工具 | lsp 工具表 |
| `mcp` | MCP 客户端与 MCP 工具 | config `mcp_servers`;ToolRuntime 构造期启动 |
| `plugins` | Lua/process 插件 | 终端:Package 挂载事务+信任门;app-server:`components.plugins` 点名+材料根 plugins/ 发现+信任门(P5,仅 v2 embedded-lua) |
| `skills` | Skill 装载 | skills 目录+Agent 定义 preload |
| `memory.read` | 记忆召回注入 | config `memory.enabled`+`memory.use` |
| `memory.write` | 记忆抽取写入 | config `memory.generate`/`learn` |
| `subagents` | `agent` 工具(子代理) | Agent 工具表 |
| `workflow` | workflow 定义与执行 | workflow 目录 |
| `goal` | goal 执行 | goal 域 |
| `loop` | loop 执行 | loop 域 |

底线(不可关,也不许列成功能):核心校验、资源授权、取消传递、提交闸门、账面引用校验、容量预检。关闭 `memory.*` 不影响必须保存的执行事实;关闭遥测外送不关闭本地账。

## 5. 资源授权口径〔冻结,算法语义;执法路迁移属 P2〕

```text
可供应 S = 已编组件 ∩ 已安装/已信任版本 ∩ 功能允许装配
可授权 G = S ∩ 平台授权 ∩ 租户授权 ∩ 项目授权 ∩ 调用主体授权
会话工具 E = G ∩ 部署档选择 ∩ Agent 选择 ∩ 渠道/任务限制 − 任一层 deny
可发现 D ⊆ E;本次直接 schema 集合 V ⊆ E
```

- 每层"未设置"=继承;"显式空集"=一个也不给。deny 永远优先。
- `E` 只回答工具种类。参数级(路径、网络目标、凭据、配额)在每次调用时另验;授予工具不等于授予它能碰的全部资源。
- `permissions.allow_tools`(settings.local)只影响"是否再问一次",不进 `E` 的任何一层。
- 父子求交:子 Agent、Workflow 节点在父有效授权内再求交;"父请求里没展示"不是"父没权限"。
- 托管首版禁止 allow 通配符。

身份与范围类型(`AuthenticatedSubject`、`ResourceScope`)归多租户单冻结;`RequestContext` 归 LubanCore 单冻结;本文只消费,不另造。

## 6. SessionCapabilitySnapshot〔冻结字段;实现属 P2〕

一场会话一份,首请求前采用,此后只按 §八 生效规则换:

| 字段 | 含义 |
| --- | --- |
| `planDigest` | 采用的能力计划摘要(功能面+工具选择+暴露策略) |
| `tools[]` | 每只:name、canonical id、ToolOrigin、schemaDigest、implementationDigest、version |
| `packages[]` | 采用的包摘要与信任状态 |
| `promptMaterialRefs` | 提示部件引用(与 Soul 单的段模型同源) |
| `generation` | 本快照代号;旧 tool_ref 钉它 |
| `policyRevision` | 授权策略单调版本,与 generation 分开计数 |
| `agentRef` / `profileRef` | 采用的 Agent 与部署档引用 |

跨节点恢复的裁决规则(快照齐不齐、Secret 撤没撤、workspace 对不对)归集群控制面单 §12.3;本文只定字段与"会话内发现、暴露、提示、执行共同消费同一份"这条纪律。

## 7. RuntimeBundle 寿命〔冻结〕

一场会话一份 bundle。同场多轮复用,不得每轮建注册表再把旧 Tool 指针留给后台任务。

### 7.1 构造序

```text
1 解析能力计划(纯数据,零外部副作用)
2 backend(模型连接)
3 MCP 客户端、插件进程、native 库(按计划点名,不先启动后过滤)
4 工具实例与注册表(一次性装齐)
5 取消源、日志接点、事件出口
6 交会话使用
```

### 7.2 关闭序

```text
1 停止接纳:拒绝新输入、新工具调用
2 断调度:不再派发新模型请求与新工具执行
3 排空在途:等安全点——后台任务与流式请求按各自 deadline 收口;
  任务钉住自己那一代快照,不因前台空闲就拆后台还握着的库
4 提交终态、按档刷账(§9)
5 析构依赖:按构造逆序拆工具实例 → MCP/插件连接 → backend
```

顺序不许颠倒:先析构依赖后排空,等于把后台任务脚下的地板抽走。取消传递贯穿 2–3:在途工具收到取消,收不了的按 Unknown 记账,不伪造完成。

### 7.3 引用与共享边界

- bundle 内对象归本场;跨场共享组件(MCP 连接池、backend、只读包内容)由资源所有者按作用域与引用计数管,单场停用不杀别人还用着的。
- 借用对象至少活到本 bundle 关闭完成;异步句柄不得悬垂。
- 不强杀线程。要硬时限的交受监督子进程;超时未收拢返回明确状态,不释放仍被访问的对象。

本节与 LubanCore 单 §6.2 关闭合同同向:那份管 SDK/Runtime 级 Shutdown,本节管会话级 bundle;两级的"停接纳→排空→拆依赖"顺序一致,后者先于前者。

## 8. 变更生效〔冻结默认;当前会话扩展属 P7〕

| 变更 | 默认生效时点 |
| --- | --- |
| 装新包、加 MCP、改 Agent 工具清单、改 Soul | 下个新会话;当前场报 `pending_next_session` |
| 自愿收窄工具面 | 当前调用闸立即收窄;下一安全请求换快照 |
| 撤权、泄凭据、紧急停用 | 立即拦未 dispatch 调用;取消在途;终态可能 Unknown |
| 普通插件升级 | 下个新会话;旧 generation 排空后回收 |

当前会话显式扩展(preview/apply、expectedRevision、安全点)在 P7 前不开;能力查询继续报 `current_session_expansion=false`。

## 9. 受理真账 durability 档〔冻结口径〕

三档沿用既有权威定义([trajectory-v3-schema.md](../architecture/trajectory-v3-schema.md) §三;实现在 src/trajectory/journal.cpp `FlushFileDurable`、src/platform/atomic_write.cpp):

| 档 | 动作 | 承诺 |
| --- | --- | --- |
| `Buffered` | 入 stdio 缓冲 | 无崩溃承诺 |
| `ProcessCrash` | fflush | 进程崩了不丢;掉电不保 |
| `PowerLoss` | fsync/FlushFileBuffers | 掉电不丢 |

操作受理真账的定档:

- 受理事实(operation.accepted/claimed/completed/cancelled)达 **PowerLoss** 档,才许回 durable accepted 回执。`ostream::flush` 不是档位,不得据此宣称断电不丢。
- 输入材料(正文、图片)先落不可变存储(blob/artifact,PowerLoss),后提交受理事实;先答 accepted 后补材料是违约。
- 材料已落、受理事实未落的孤儿文件按保留期清理,恢复时不认作已受理。
- 受理事实统一进 v3 writer 单写者账(〔拟议〕P1 实施);现 `operations.jsonl` 只作迁移来源,不与 v3 账并列为第二份权威。

## 10. G15 探针清单〔冻结清单;实现属 P1/P2〕

以下探针按 §12.1 链路逐窗注入,验证事实与执行次数。P0 只冻结清单。

错误回执类:

| # | 注入点 | 必须看到 |
| --- | --- | --- |
| E1 | 操作台账打不开(目录只读/盘满) | 拒收,不回 durable accepted,不进执行队列;同键重发同样拒收 |
| E2 | Append 写失败(半行/IO 错) | 受理失败;不得拿内存表充当已受理真账出回执 |
| E3 | 材料落盘成功、回执发出前崩溃 | 同键重发命中原意图返回原 operationId;不重复执行 |
| E4 | 同键同载荷 / 同键异载荷 | 前者原回执,后者 `operation.conflict`;真实执行次数与账一致 |

意图丢失类:

| # | 注入点 | 必须看到 |
| --- | --- | --- |
| I1 | 材料落盘成功、接受事实未落(孤儿) | 恢复不认作已受理;按保留期清理 |
| I2 | 正文/图片只在内存队列时进程消失 | 重启从持久账重建队列;无持久正文的"受理"不得恢复为待执行 |
| I3 | resume 多跳来源链(resume 的 resume) | 沿全来源链查原意图去重;不重复发行执行,不拿裸 `op-N` 当全局唯一身份 |

claim 恢复类:

| # | 注入点 | 必须看到 |
| --- | --- | --- |
| C1 | claim 后、执行前进程消失 | 恢复为待领取;意图不丢 |
| C2 | 已 claim、执行中、无终态时进程消失 | 核验工具事实账:有 started 无终态记 Unknown,不盲重跑,不当已完成 |
| C3 | 执行后、终态提交前进程消失 | 以工具事实核对副作用;未知记 Unknown |
| C4 | 两进程竞态 claim 同一操作 | 单写者/owner epoch 只放一个推进;另一方只读或明确拒绝 |

## 11. 协议 2.0 映射〔冻结映射;信封归 AppServer 单〕

- 2.0 使用标准 JSON-RPC 2.0 信封(出站带 `jsonrpc:"2.0"`)。1.2 的"出站不带该字段"是 1.2 的冻结(见 src/app_server/protocol.hpp `kEmitJsonRpcField`),两版互不越界。
- 版本协商只在 initialize;1.2 客户端连 2.0 得明确版本不匹配,不暗改 resume/审批行为;同一会话不半途切版。
- 反向请求(引擎→客户端的 `toolExecution/request`、审批问询)用**独立反向 id 空间**,与客户端请求 id 两不相干;transport id 只配对消息,不当执行身份。
- 各传输承载同一 domain DTO,只换编码:stdio/WS 双向;HTTP+SSE 用 SSE 下发、HTTP 方法上行;gRPC 按流/unary 映射;IM 普通聊天消息不当工具回执。
- 报文样例:`tests/fixtures/capability/appserver-v2.messages.golden.json`,信封校验在 `tests/unit/capability/test_capability_contract.cpp`。

## 12. owner 归属

同义不许两账:本文冻结的字段与算法,别的单子引用,不复制。分工账:

| 事项 | owner 单 |
| --- | --- |
| 2.0 信封、方法面、事件游标、断线策略 | AppServer接入SessionV3 |
| SessionFactory、无终端装配、Theme 退役、RequestContext、SDK Shutdown | LubanCore与CLI分离 |
| Tenant/Project 身份、ResourceScope、Worker OS 隔离、LocalTrusted/Managed | 多租户隔离 |
| 实例路由、租约 fencing、CapabilitySnapshot 恢复裁决 | 多用户多节点控制面 |
| IM 入口、durable-before-200、渠道出站投递 | 多渠道消息接入 |
| 扩展 ABI/SPI、manifest、热重载两层快照、在飞安全 | 轻量可塑ExtensionRuntime |
| 提示部件段模型、工具 schema 与能力说明同源 | System拼装Hook(Soul) |
| 外部任务等待、ownerEpoch 执行侧、结果投递底座 | SessionV3异步工具 |
| 执行/结果提交闸门、attempt 收口 | 失败与恢复 |
| 应用根三变量、来源裁剪、参数根/数据根分家、RuntimePaths | 应用Worker接入补齐 |

## 13. 应用根与来源裁剪〔冻结 2026-09-14;实现属应用Worker接入补齐单 P1〕

外部应用常驻、每项独立研究起一只 Worker 时,LubanCode 进程的材料来源与状态落点由三枚环境变量钉死。设计出处 `todos/应用Worker接入补齐_Agent与Skills装配_独立参数目录及恢复隔离.todo` §四。

### 13.1 三枚变量〔冻结〕

| 变量 | 语义 | 取值约束 |
| --- | --- | --- |
| `LUBANCODE_HOME` | 应用专用参数根,值即根本身,不追加 `.lubancode`。未设置=个人 CLI 旧布局(`~/.lubancode`)原样,不暗迁移 | 非空绝对路径;空值/相对路径/不可访问均为配置错误,启动门拒启(退出码 1,stderr 人话),不静默回个人目录 |
| `LUBANCODE_DATA_HOME` | 独立运行数据根(会话账/信任账/插件数据/缓存/日志)。未设置默认 `<LUBANCODE_HOME>/data`;仅在启用应用根语义时采用 | 同上;孤立设置(无 `LUBANCODE_HOME`)是配置错误;等于参数根、或包住参数根(参数根落数据根之内)均拒——读写不分;数据根在参数根之内合法(默认即如此) |
| `LUBANCODE_MANAGED` | `1`=托管模式,即多租户隔离单 LocalTrusted/Managed 档中 Managed 档的进程级入口;`0`=关 | 只认 `1`/`0`(空串与其余值均拒,不猜);`=1` 必须显式设置 `LUBANCODE_HOME`——托管第一件事就是在参数根里发现文件、播种材料,没根的托管无从谈起 |

铁律:

- **禁止隐式降级**:`LUBANCODE_MANAGED=1` 下缺件即拒,不回落个人默认目录,不裁剪成"照旧读 cwd/个人材料"顶替。
- env 是进程级的:宿主应用给每个 Worker child 构造专属 env,不修改自己的全局环境,不重定义 `HOME`/`USERPROFILE` 冒充应用参数根。与多租户隔离单"不改进程环境以切换租户"不冲突——那条管"同进程轮流服务多人",本合同管"一进程一份 env、进程内单一身份"。
- 启动序:先识别三变量,再发现文件、播种默认材料、启动组件。校验失败发生在任何读家目录的动作之前。
- **Windows 空值语义**:"设为空串"与"未设置"在 Windows 上必须走 Win32 面区分——CRT 面 `_putenv("NAME=")` 的语义就是删除变量,`getenv`/`_dupenv_s` 也读不到环境块里物理存在的 `NAME=` 空值条目。生产读侧 `platform::GetEnvVarPresent` 走 `GetEnvironmentVariableW`(变量未设=rc 0 且 lasterr `ERROR_ENVVAR_NOT_FOUND`;条目在值为空=rc 0 且 lasterr 未设)。宿主给 child 传空值只有 envblock 一条真路(Node/libuv spawn 的 env 表原样落块);活进程内测试注入用 `SetEnvironmentVariableW(name, L"")`。CRT 消费面(`GetEnvVar` 及全仓既有环境变量读取)不受影响:空值条目对它当未设,恰是"空=未设"的既有语义。
- 路径等值比较走 `platform::PathComparisonKey`(weakly_canonical 失败退 lexically_normal);OS 挂载/权限承担硬拒绝,字符串判断只作前置提示(多租户隔离单口径)。

### 13.2 来源裁剪〔冻结;执法路随 P1 落地、档点名来源归 P2〕

| 来源 | 应用根(非托管) | 托管(`MANAGED=1`) |
| --- | --- | --- |
| 参数根内材料(config.json/prompts/souls/agents/skills/languages/models.json) | 读 | 读 |
| 个人 `~/.lubancode`、`~/.agents`(skills/AGENTS 等个人材料层) | 不进视野(整层裁) | 不进视野(整层裁) |
| cwd 项目级 `.lubancode/config.json`、settings.local | 照旧读 | **整层裁掉**;部署档点名来源 P2 起接档字段,当前一律不读 |
| 旧位置 `.lubancode.json` 迁移 | 不适用(参数根是新地界,无旧账) | 不适用 |
| 发行内置材料(官方 skills/内置提示模块) | 读(发行层,非个人家目录) | 读;来源在能力清单列明 |

状态落点:workspaces/会话账、workflow-runs、browser-artifacts、package-trust/package-state/hook-trust/plugin-trust、hooks-outbox、plugin-data/package-data、package-store、cache、logs、memory(含 memory-jobs)全部落**数据根**;参数根可挂只读。个人 CLI(未设应用根)一切落 `~/.lubancode`,行为与从前逐字节一致(AW-01)。

### 13.3 RuntimePaths 归属〔冻结〕

`config::RuntimePaths`(`src/config/runtime_paths.hpp`)是参数根/数据根的唯一解析口,挂靠 LubanCore 单阶段 B 的 RuntimeAssembly 服务条款("配置/工作目录/凭据显式传递")——Core owner 接管该服务时此类型并轨,别的单子不得另立同名路径服务。进程级便捷口 `config::HomeLubancodeDir()`(材料根,重定向后即参数根)与 `config::StateRootDir()`(状态根)供库层消费,语义以本节为准。

### 13.4 app-server 当前装配能力事实〔P0 清单;2026-09-14 对源码〕

逐项对符号,"开关存在"不冒充"装配完成":

| 能力 | 状态 | 源码符号 |
| --- | --- | --- |
| 部署档解析(schema 1:features/tools/exposure/limits/MCP 名单) | 已落 | `app_server::LoadHarnessDeploymentFile`/`ParseHarnessDeployment`(src/app_server/harness_profile.cpp) |
| headless 生产装配(MCP 按档点名启动、allow 名单装工具、必需件起失败拒整场) | 已落 | `app_server::AssembleSession`(src/app_server/session_assembly.cpp);入口 `RunAppServerMode --app-server-profile`(src/app/cli_app.cpp) |
| 无档默认 | 显式零工具默认档(合同 §2.3),不照搬终端工具表 | `RunAppServerMode` 未递档分支 |
| Agent 装配(agentRef 解析入首请求) | **已落(P2,2026-09-14)**——档点名 agentRef 必解析(AgentCatalog,解析面=码内内置+材料根 agents/);档案定系统提示部件(prompt_assembler 既有管线:Profile/persona 替业务正文,宿主段按实际工具面现拼盖不掉);找不到/不可用拒启,不回落默认 | `app_server::ResolveHarnessAgentPlan`/`ComposeHarnessSystemPrompt`(src/app_server/agent_wiring.cpp);消费 `SessionAssemblyRequest::agent_plan` |
| SkillTool/技能材料装载 | **已落(P2,2026-09-14)**——features.skills 放行且 tools 面点名 `skill` 才装配:显式单根(材料根 skills/)扫描,清单段/工具/预装正文三面同进同退;skills.preload 缺名整场明拒;不搬终端五层合并,装技能不授予任何执行工具 | `AssembleSession` 步骤 2.5(src/app_server/session_assembly.cpp)+`tools::ScanSkillsDir`/`SkillTool`;组合 `ComposeHarnessSystemPrompt` |
| Lua/process 插件装载 | **已落(P5,2026-09-14)——v2 embedded-lua 真装载**:`components.plugins` 点名 → 材料根 `plugins/` 扫描(与终端同一发现面)→ 信任账(`PluginTrustStore`,只读消费)→ `ManifestLuaRuntime` 挂载(每场一份,Lua state 不跨会话,registry 先析构 owner 后收口);工具面由 `tools.allow` 的 `plugin__<id>__<tool>` 名定(装载面≠注册面,与 MCP 同规矩);插件工具走统一工具闸(`ManifestLuaToolAdapter`:needs_confirm 恒真、ApprovalClass::External);HTTP/Secret 是 manifest 声明面(permissions.network/secrets)经既有宿主执法(越权在传输层权限对账步落锤、Secret 只在调用作用域解析),P5 未另立授权字段。**未接面如实**:process/native 件点名仍报 `component_unavailable`(kind 未接线);Package(packaged)插件不经此通道;挂载事实只进 `SessionAssembly::mounted_plugins` 与诊断日志,未进 thread/started 协议字段 | `PluginMounter`+`AssembleSession` 步骤 0(src/app_server/session_assembly.cpp);`HarnessProfile::plugins` 与 plugin__ 依赖解释(src/app_server/harness_profile.cpp);入口接线 `RunAppServerMode`(src/app/cli_app.cpp);测试 tests/unit/app_server/test_plugin_assembly.cpp(信任/HTTP·Secret/寿命三件)+ test_session_assembly.cpp(装配路) |
| 幂等受理+输入原件持久(operations-inputs、CanonicalInputPayload、ProcessCrash 耐久) | 已落 | `SessionService::SubmitInput`(src/runtime/session_service.cpp;PR #59) |
| 协议 1.3 幂等受理面(thread/start、turn/start 可选 clientOperationId:同键同载荷回原受理,同键异载荷 operation_conflict;会话创建去重按主体+workspace 落 session-creates.jsonl) | 已落(P3) | `Server::HandleThreadStart`/`AcceptTurnStart` + `SessionCreateLedger`(src/app_server/server.cpp) |
| operation/read 只读核对口(受理/派发/终态/unknown,重启后按 clientOperationId 找回;final 正文经 v3 投影按 turnId 定位;零副作用不触发执行) | 已落(P3) | `Server::HandleOperationRead` + `SessionService::ReadOperationFacts` + `runtime::FindFinalAssistantText`(src/runtime/session_service.cpp、trajectory_history_view.cpp) |
| 客户端自动重试(受理—终态幂等闭环) | **可用**(P3 故障注入过:受理落盘失败拒收零执行、终态行丢失回 unknown 不冒充、重启窗口同键找回不重跑;证据 tests/unit/app_server/test_app_server_operation_idempotency.cpp。P4 补进程级硬杀证据:飞行中 SIGKILL/TerminateProcess → 同根重启按原键 unknown+gaps、模型零重跑) | 同上两行;真拔电/OS 崩溃未验,归后续真机批次 |
| Node 外部客户端端到端(部署隔离+幂等+凭据+双根并行+空值 envblock+收口) | 已落(P4,2026-09-14)——五幕真 exe+假模型:主链路(托管档,幂等闭环+凭据只走参数根+状态只落数据根+收口退出码 0)/双 Worker 并行互不串材料/飞行中硬杀与重启找回/EOF 孤儿自退后记录仍可查/空值 env 跨进程 envblock 明拒。客户端 `examples/node-client/lubancode_worker_client.js` 钉协议 1.3;`initialize` 能力表补登 `operation/read`(P3 方法在而漏登,报告交接面);兼容记录 docs/reference/app-server-external-client.md | ctest `e2e.app_server.node_client`(tests/CMakeLists.txt find_program node 注册;无 node 环境不注册,该腿如实缺证据——linux-manylinux 容器即此档) |
| turn 终态与 ResultEnvelope(finalMessageRefs、usageReported、resultEnvelopePersisted) | 已落 | `SessionService::RecordTurnFinal`/`MakeTurnCompletedParams`(src/app_server/schema.cpp;PR #62) |
| 应用根三变量/来源裁剪/参数根-数据根分家 | 已落(本节合同+P1) | `config::ResolveRuntimePaths`/`StateRootDir`(src/config/runtime_paths.cpp) |
| gateway/channels 状态根接数据根 | **未接**(避让在跑的 QQ 接入单,另立小单) | cli_app.cpp gateway run 段仍走 HomeLubancodeDir |
| rg-stage 工具缓存的播种脚本 | **未接**(读取走数据根;fetch_ripgrep.py 不认数据根,应用根下的 rg-stage 须部署者自落) | src/tools/search_ripgrep.cpp UserStage 层 |

错误码归属:路径/来源类配置错误在启动门以 stderr 人话+退出码 1 拒启(与 CLI 既有风格一致),不另立协议错误码;协议面"点名未接线组件报 `component_unavailable`"自 P2 生效(装配失败经 thread/start 错误信封带 `data.code`,additive 字段)——P5 起 v2 embedded-lua 真装载,此码收窄到"runtime kind 未接线"(点名 process/native 件),另增三枚插件装载失败码:`plugin_missing`(点名件不在发现账,含 manifest 坏被扫描剔除)、`plugin_untrusted`(信任账不过:未信任/disable/内容指纹算不出)、`plugin_load_failed`(Lua 挂载坏:entry 读不到/编译坏/handler 对账不过),同一错误信封带出;"缺获准执行工具返回 `capability_unavailable`"(本单 §六冻结)待 Skill 依赖声明 schema 升级时生效。
