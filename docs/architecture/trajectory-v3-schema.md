# session 轨迹 v3 schema(冻结稿)

状态:P0 定稿冻结;P1 其余域(工具操作账/结果仓与预览/hook 事件账/subagent 独立账/预览降档)已按 §四 落地发行,字段随本稿冻结;P2 读取侧(两份投影/工具快照折叠/result_preview 展开/跨会话五键验 hash/父子账遍历/带来源链 resume)已落地 `src/trajectory/v3/reader.hpp`,resume 载荷随 §四 增补冻结。本文是 `todos/session轨迹v3_消息主轴树链与四角色壳收敛设计.todo`(下称"单子")§4.13 待敲定合同的落地答案;与单子冲突时以单子 §一 用户定案为准。写入侧实现见 `src/trajectory/v3/`,可校验 fixture 见 `tests/fixtures/trajectory_v3/`,校验脚本见 `scripts/validate_trajectory_v3.py`。

不承担旧数据兼容:新会话写 v3,v2 读取不迁移,v2→v3 无转换器(单子 §1.5/§七)。

## 一、两类行与公共信封

原始 JSONL 只追加,每行一份 JSON。顶层 `type` 只取 `message`、`event`(单子 §1.1)。两类行共用递增 `seq`、同一条哈希链、同一只单写者。

### 1.1 公共信封字段(两类行都有)

| 字段 | 类型 | 规则 |
| --- | --- | --- |
| `type` | string | `message` 或 `event` |
| `schemaVersion` | int | 恒为 `3` |
| `sessionId` | string | 所属会话;子会话另有自己的 sessionId(§4.31) |
| `runId` | string | 所属执行流 |
| `seq` | int | 本文件内从 1 连续递增,两类行共用,单写者发号;换 system、换 turn 不归零 |
| `timestamp` | string | ISO-8601 UTC 毫秒,如 `2026-09-10T04:59:25.314Z`;只管展示,回放顺序认 seq |
| `prevHash` | string | 上一行 lineHash;首行为 64 个 `0` |
| `lineHash` | string | `SHA256(prevHash || canonicalJson(本行去掉 prevHash/lineHash 两键))`;canonical 规则承继 v2(键按 UTF-8 字节序、无空白、UTF-8 合法性校验) |

信封字段 camelCase(单子 §1.1)。同一行重试提交同一身份不得重复入档;`messageId`/`eventId` 是行唯一键。

### 1.2 message 行字段

| 字段 | 类型 | 规则 |
| --- | --- | --- |
| `messageId` | string | 必填;稳定身份,正文相同也是不同消息 |
| `turnId` | string \| null | 必填;system 消息恒为 null;compact 内部回合持独立 turnId(§4.6) |
| `parentTurnId` | string | 可选;内部回合消息挂触发它的主会话 turn |
| `stepId` | string | 可选;一次逻辑模型请求步骤 |
| `requestId` | string | 可选;模型生成的消息必带,指向生成它的那次请求尝试 |
| `actionId` | string | 可选;工具调用身份(§4.15:与 payload `tool_call_id` 同值) |
| `compactId` | string | 可选;compact 内部消息与摘要必带,贯穿全链 |
| `purpose` | string | 必填,枚举:`conversation`/`compact`/`context_summary`/`session_title`/`capability`(capability 留给 §4.29 渐进披露,本棒不发行) |
| `origin` | string | 必填,枚举:`human`/`soul`/`session_runtime`/`compact_runtime`/`context_runtime`/`hook`/`skill`/`subagent`/`parent_agent` |
| `display` | object | 可选,`{"mode":"visible\|collapsed\|hidden"}`;缺省 visible;只管界面(§4.28) |
| `message` | object | 必填;`role` ∈ `system`/`user`/`assistant`/`tool`,其余按角色(见 1.2.1) |
| `causedByEventRef` | string | 可选;回指触发事件(skill 展开、摘要注入等,§4.27) |
| `sourceMessageRef` | ref | 可选;摘要指回候选产物(§4.5 第 7 行) |
| `systemMeta` | object | system 消息必填:`{"cause","changeEventRef","settingsVersion","systemChanged"}`;首行 system `changeEventRef` 为 null、`cause:"initial"` |
| `completionStatus` | string | assistant 可选:`complete`/`interrupted`/`truncated`;缺省 complete;Esc 定稿为 interrupted(§4.63),length 截断为 truncated(§4.43) |
| `provider`/`wire`/`model` | string | 模型生成的 assistant 必填(§4.44:该次实际出站身份,不是会话当前设置) |
| `responseModel` | string \| null | 模型生成的 assistant 必填;服务端没报就 null,不拿 model 冒充 |
| `providerConfigRef`/`modelProfileRef` | string | 可选;不可变配置版本引用 |
| `usage` | object \| null | 模型生成的 assistant 必填;唯一 owner 见 §五;缺实报为 null,不补 0 |

#### 1.2.1 message 本体按角色

- system:`{"role":"system","content": string}`;完整拼装结果,恢复不重拼(§4.3)。
- user:`{"role":"user","content": string | blocks}`;32 KiB 上限与原文存档按 §4.51(后续棒次接线,字段先冻结)。
- assistant:`{"role":"assistant","content":...,"tool_calls":[...]?,"reasoning_content":...?}` 或协议原生有序块;思考载荷按 §4.42 完整留档,不拼成一条字符串丢原形。
- tool:`{"role":"tool","tool_call_id": string,"content": string}`;`tool_call_id` 等于信封 `actionId`(§4.15)。

### 1.3 event 行字段

| 字段 | 类型 | 规则 |
| --- | --- | --- |
| `eventId` | string | 必填 |
| `kind` | string | 必填,枚举见 §二 |
| `status` | string | 生命周期事件必填,且必须匹配 kind 的固定映射(§2.2);非生命周期事件不得携带 |
| `turnId`/`parentTurnId`/`stepId`/`requestId`/`actionId`/`compactId`/`commandId`/`hookDispatchId`/`taskId`/`titleGenerationId` | string | 可选;按 kind 的必选表(§2.2)要求出现,不硬塞 |
| `payload` | object | 必填(可为 `{}`);各 kind 的载荷表见 §四 |
| `effects` | array | 可选;本事件直接持有的效果(§4.27):`{"effectId","type":"display\|config\|message\|context",...}` |
| `effectRefs` | array | 可选;引用其它事件效果 `{"eventRef","effectId"}`;向前、无环 |

## 二、事件 kind 冻结清单与状态机

### 2.1 kind 全表(P0 首批冻结)

命名一律 `<域>.<对象>.<动作>` 点分小写。新 kind 只许随 schema 版本追加,不得改老含义。

| 域 | kind |
| --- | --- |
| 会话 | `session.started`、`session.ended` |
| system | `system.change`(变更缘由事件,§4.3) |
| 上下文 | `context.system.applied`(system 版本链提交)、`context.input.applied`(普通消息接纳)、`context.tool_previews.reduced`(§4.38 降档,P1 其余发行) |
| 模型请求 | `model.request.prepared`、`model.request.sent`、`model.request.failed` |
| 模型响应 | `model.response.started`、`model.response.delta`、`model.response.completed`、`model.response.failed`、`model.response.cancelled`、`model.usage.appended`(迟到/更正/无消息请求的观察承载) |
| compact | `compact.requested`、`compact.pending`、`compact.started`、`compact.range.retreated`(§4.64 撞窗整轮回退,compact 运行时接线发行)、`compact.validation.started`、`compact.validation.completed`、`compact.applied`、`compact.failed`、`compact.cancelled`、`compact.rejected` |
| 工具执行 | `tool.execution.pending`、`tool.execution.started`、`tool.execution.waiting`、`tool.execution.resumed`、`tool.execution.finished`、`tool.execution.failed`、`tool.execution.cancelled`、`tool.execution.rejected`、`tool.execution.unknown` |
| 工具结果 | `tool.result.persisted`、`tool.result.persist_failed`、`tool.result.selected` |
| hook | `hook.dispatch.requested`、`hook.pending`、`hook.started`、`hook.completed`、`hook.failed`、`hook.cancelled`、`hook.unknown`、`hook.skipped`、`hook.effects.applied`、`hook.effects.rejected` |
| 命令 | `command.received`、`command.pending`、`command.started`、`command.completed`、`command.failed`、`command.rejected`、`command.cancelled`、`command.unknown` |
| 队列 | `input.received`、`input.enqueued`、`input.admitted`、`input.superseded` |
| 标题 | `title.requested`、`title.extracted`、`session.title.applied` |
| resume | `resume.source.attached` |
| subagent | `subagent.spawn.requested`、`subagent.linked`、`subagent.observed`、`subagent.spawn.failed` |
| 后台任务 | `task.started`、`task.pending`、`task.completed`、`task.failed`、`task.cancelled` |

### 2.2 kind → status 固定映射(§4.14)

`kind` 与 `status` 映射由 schema 固定,不出现 `finished + running` 这类组合:

| kind 后缀 | status |
| --- | --- |
| `.pending` | `pending`(必须带 `payload.reason`) |
| `.waiting` | `pending`(带 reason 与可恢复等待引用) |
| `.started`/`.resumed` | `running` |
| `.finished`/`.completed`/`.applied`/`.sent`/`.linked` | `done` |
| `.failed` | `failed` |
| `.cancelled` | `cancelled` |
| `.rejected` | `rejected` |
| `.unknown` | `unknown` |
| 其余(`session.started`、`system.change`、`model.request.prepared`、`model.response.started`/`.delta`、`compact.requested`、`compact.range.retreated`、`context.*.applied`、`input.*`、`resume.source.attached`、`subagent.observed`、`command.received`、`hook.dispatch.requested`、`hook.skipped`、`title.*`、`session.title.applied`、`tool.result.persisted`/`persist_failed`/`selected`、`hook.effects.applied`/`rejected`、`model.usage.appended`) | 不携带 status 字段 |

生命周期规则(§4.14):同一操作可以多条 event,各持自己的 eventId/seq,共用操作身份;每次尝试最多一个执行终态;终态后迟到响应另记观察事件不改旧终态;`pending` 是"在等"、`running` 是"在执行";崩溃后见 `started` 无终态只能判"可能已执行"。

### 2.3 compact 状态机(§4.5-4.8)

```
idle
 -> compact.requested(trigger=manual|auto, reason, compactId, requirementsSnapshot;建内部回合 turnId,parentTurnId 挂主 turn 或 null)
 -> [compact.pending]?(确有等待才记,带 reason)
 -> [compact.range.retreated]*(§4.64 发送前门禁不通过:整轮回退摘要输入边界,
    每次计划修订一枚,带 planRevision 与本次退出的 turn/message 引用;
    退出引用不等于 removedMessageRefs,只有成功 applied 后被摘要替代的
    前缀才进 removed;退空仍不过则记 rejected 收场,不发请求碰运气)
 -> compact.started(冻结源上下文版本与压缩/保留范围)
 -> [专用 system message(purpose=compact,可选)] + compact prompt user message(purpose=compact,可多条)
 -> model.request.prepared(压缩请求,重试逐次留档)
 -> [model.request.sent] -> assistant 回复 message(purpose=compact,候选产物)
 -> compact.validation.started -> compact.validation.completed(passed 与 checks[])
 -> 摘要 user message(purpose=context_summary, origin=compact_runtime, turnId=null, sourceMessageRef 指回候选,暂不生效)
 -> compact.applied(唯一成功终态;PowerLoss 档)
 或失败三态(都是终态,不更新 contextRevision,不显示"压缩完成"):
    compact.failed(模型请求失败)
    compact.cancelled(用户取消)
    compact.rejected(校验不过/收益不足/源版本冲突)
```

`compact.applied` payload 至少含(§4.8 全表):`sourceContextRevision`/`newContextRevision`、`oldStateHash`/`newStateHash`、`summaryMessageRef`/`validationEventRef`、`removedMessageRefs`/`retainedMessageRefs`、`protectedTurnIds`、`contextId`/`contextChain`、`contextTokensBefore`/`contextTokensAfter`、`tokenMetric`、`trigger`/`compactId`。

采用顺序(§4.8,写死):锁上下文提交口 -> 核对源版本 -> 预构造新内存视图并验预算 -> 摘要与引用落稳 -> `compact.applied` 按 PowerLoss 档落稳 -> 发布预构造内存视图 -> 放锁。

- applied 前崩溃:旧上下文有效,已有回复/校验/摘要都只算候选。
- applied 后、内存发布前崩溃:resume 从 applied 重建新上下文。
- applied 写盘失败:不得发布新内存视图;停止继续发送的路径,报告故障。
- 成功提交后不再落一个含糊的 `completed`,避免两个终态。
- 同一主上下文一次只运行一个 compact;额外触发合并或拒收并留原因。

回合归属(§4.6):compact 用独立内部 turnId,不冒充真人回合;`parentTurnId` 挂触发它的主会话 turn,空闲手动执行时为 null;map/reduce 各自开 step,重试另开 requestId 不换 compactId;注入摘要 `turnId:null`。

### 2.4 上下文链提交(§4.30)

前驱属于 `(contextId, contextRevision)`,不在 message 上写全局唯一前驱。链节点 `{"messageRef","prevMessageRef"}`,只有根节点 prevMessageRef 为 null。五类提交事件各前进一次 revision:

| 提交事件 | 场景 | 载荷 |
| --- | --- | --- |
| `session.started` | 开卷;revision 1 | `payload.context = {contextId:"main", revision:1, contextChain:[{systemRef,null}]}` |
| `context.system.applied` | system 版本切换 | `beforeRevision`/`afterRevision`、`rootMessageRef`(新 system)、全量 `contextChain`(根换新 system,后续节点重接) |
| `context.input.applied` | 普通消息接纳 | `beforeRevision`/`afterRevision`、`appendedChain`(首新节点接旧尾,后续逐个相接)、`addedMessageRefs` |
| `compact.applied` | 压缩生效 | 全量 `contextChain`(见 2.3) |
| `context.tool_previews.reduced` | 预览降档(§4.38) | 全量 `contextChain`(原 tool 节点换派生消息,后续重接);载荷见 §四 |

校验:根唯一且前驱 null、非根前驱存在、每节点至多一个后继、无环、无重复、全链连通;数组按根到尾序列化且邻接项与 prevMessageRef 一致,冲突视为坏记录。压缩期间队列输入独立保留,不在 retained 里也不丢。

## 三、引用与 hash 规范

### 3.1 引用格式

| 引用 | 同会话 | 跨会话(resume/fork/父子) |
| --- | --- | --- |
| messageRef | id 字符串 | `{"sessionId","runId","seq","id","hash"}` 五键齐全,读侧校验 hash(§4.2:不存裸 seq) |
| eventRef | id 字符串 | 同上 |

`artifactRef`(结果仓,§4.16):`{"artifactId","kind","path","sha256","bytes","mediaType"}`,kind ∈ `result_metadata|stdout|stderr|combined|report|image|blob`;数组 `result_ref` 固定为数组,只有一份也 `[ref]`,没有写 `[]`。`blobRef` 承继 v2 BlobRef 五键(`sha256/size/media_type/encoding/compression`)。

### 3.2 hash

- 行哈希见 §1.1;算法承继 v2 `ComputeEventHash`,只是字段名 camelCase 化(`prevHash`/`lineHash`)。
- `compact.applied.oldStateHash`/`newStateHash`:对 system 引用及有序上下文引用计算,算法带版本(v3 定为 `sha256` over canonical json of `{contextId, revision, [refs]}`,写进 `payload.stateHashAlgorithm:"v3-refs-sha256"`)。
- 跨会话引用必带目标行 lineHash。

### 3.3 耐久分档与刷盘策略(§4.43"批量刷盘阈值随 schema 冻结")

Durability 三档承继 v2:`Buffered`(只入 stdio 缓冲)/`ProcessCrash`(fflush)/`PowerLoss`(fsync/FlushFileBuffers)。写盘策略定案:

| 内容 | 档位 |
| --- | --- |
| 流式 delta 事件批次 | 批内 `Buffered`,批尾(250ms 或 4 KiB 窗口,先到为准)`ProcessCrash` |
| 全部 message 行、生命周期终态事件、`model.response.completed/cancelled` | `PowerLoss` |
| `compact.applied` 及其后第一批 | `PowerLoss` |
| 非终态过程事件(pending/started 等) | `ProcessCrash` |

普通 flush 不得称为断电安全;ProcessCrash 与 PowerLoss 保留区别(§4.2)。

## 四、各域字段表(§4.14-4.34 挂点)

P1 其余域已发行(工具操作账 `tool_action.*`、结果仓与预览 `result_store.*`、hook 事件账 `hooks.*`、subagent `subagent.*`、降档 `writer::ReduceToolPreviews`);slash/skill/标题/队列/估算/容量/后台任务/todo 族留给后续棒次,字段仍按下列挂点冻结。

- **工具**(§4.15-4.21,已发行):`actionId`(`== payload.tool_call_id`,同时出现必校)、`provider_tool_call_id`、`attempt`(正整数,从 1 起)、`result_id`(结果仓 `res-<六位号>`)、`effectiveArgsRef`、幂等键 `sha256(canonical({version,action_id,tool_identity,effective_args_hash,execution_scope_hash}))`(不算 attempt);事件族 §2.1。载荷命名定案:`tool_call_id`/`attempt`/`reason`/`error_code`/`exit_code`/`phase` 用 snake_case(工具协议字段),`effectiveArgsRef`/`executionDurationMs`/`assistantMessageRef`/`waitRef`(可恢复等待引用)/`toolIdentity`/`idempotencyKey` 用 camelCase;`cancelled.phase ∈ before_started|during_execution`;`finished.exit_code` 缺省(非进程工具)或 null(退出未知),不默认 0。tool 消息新增可选键 `resultSelectionRef`(指向 `tool.result.selected`,§4.19 示例)。
- **结果仓**(§4.16-4.17,已发行):目录 `sessions/<sessionId>/artifacts/`,`res-<六位号>.json` 为不可变描述(先临时文件、再落稳、再发布不可变名;POSIX rename 会静默覆盖,故显式查存在性)。描述文件 snake_case(`result_id`/`tool_call_id`/`attempt`/`result_kind`/`execution_event_ref`/`preview_policy`/`capture_limits`/`outputs[]`,outputs 项 `ref/output_bytes/byte_count_kind/captured_bytes/capture_complete/capture_reason/encoding`);JSONL 内 result_ref 六键 camelCase(§3.1)。预览:完整渲染 ≤ 预算原样返回;超限先留说明预算 M、正文 B=预算-M 头尾均分(floor(B/2)/余),UTF-8 边界对齐,头尾段各重标 `[文件: path | 通道: channel]`,未展示正文文件点名 `not_shown`;清单超限先存 `output_index` artifact 再列容纳得下的路径 + `omitted_output_count`;`full_output`/`captured_output` 恒为数组,全部不完整时 `full_output=[]`;4 KiB 仍装不下必要来源记 `preview_unrepresentable`。
- **hook**(§4.22-4.24,已发行):`hookDispatchId`/`hookInvocationId`/`hookId`/`definitionHash`/`hookPoint`/`handlerKind`/`inputRef`/`outputRef`/`definitionOrder`/`failurePolicy`;`hook.completed ≠ 改写已采用`,效果采用另记 `hook.effects.applied`(载荷 `effectType` 必带,`inputRef`/`outputRef`/`appliedValueRef`/`validation` 可选)/`hook.effects.rejected`(`effectType`+`reason`)。子执行(§4.23):独立 `actionId` 的 `tool.execution.*` 链,pending 载荷带 `parentActionId`/`hookDispatchId`/`hookInvocationId`/`logicalTool`/`backend`。
- **slash**(§4.25-4.26,后续棒次):`commandId` + `command.*` 生命周期;`effects[]`/`effectRefs[]`(§4.27)。
- **skill**(§4.29,后续棒次):披露阶段 `catalog/instructions/resource` 事件,`messageRefs` 关联实际载体。
- **subagent**(§4.31-4.33,已发行):目录 `sessions/<id>/subagents/<child>/<child>.jsonl`,每层完整布局,可递归。`childSessionRef`={sessionId,runId,journalPath};`childCheckpointRef`={sessionId,runId,seq,lineHash}(固定子账前缀);`parentActionRef` 含 declaredMessageRef;`taskId`。父账五步:spawn.requested(payload `taskId`/`childSessionRef`/`attempt`/`parentActionRef`/`taskArgs`/`configSnapshot`/`asyncStart`)→ 子账 bootstrap(首行 system 的 systemMeta 带派生来源 `cause:"subagent_spawn"`+`parentActionRef`+`taskId`+`spawnEventRef` 五键;委派任务为 origin=parent_agent 的 user;随后子账 `task.started`)→ linked(status=done,payload `taskId`+`childCheckpointRef`)→ observed → spawn.failed(`taskId`/`phase`/`reason`)。首版异步:linked 落稳即收据,收据 tool 消息明示 pending/started,不冒充子任务完成。
- **标题**(§4.34,后续棒次):`titleGenerationId`;返回格式定案:标题 prompt 要求模型输出单行 JSON `{"title":"..."}`,提取按解析规则版本 `title-extract-v1`;调度定案:独立 step,与首 query 同 turn。
- **队列**(§4.35,后续棒次):`inputId`、`mode`(steer/followup)、`modeSource`、`deliveryId`;接纳数量定案:每安全边界一批 steer(逐条)与至多一条 followup,显式模式覆盖入口留给后续棒次。
- **估算**(§4.36,后续棒次):`estimator=utf8_bytes_div4`、`estimatorVersion=1`、`scope=model_input_json_utf8_v1`;输出为 hook outputRef 结构(prepared 用 `tokenEstimateRef` 引用,不内联)。
- **容量**(§4.40,后续棒次):`contextWindowTokens`/`modelDefaultOutputTokens`/`requestedMaxOutputTokens`/`effectiveMaxOutputTokens`/`outputLimitSource`/`wireOutputLimit`/`outputReserveTokens`/`safetyMarginTokens`。
- **降档**(§4.38,已发行):`context.tool_previews.reduced` 为独立上下文提交事件(§2.4 同类:携带完整新链),载荷定案 `{contextId, beforeRevision, afterRevision, oldPreviewBudget, newPreviewBudget, replacementRefs, contextChain, inputHash, estimatedTokensBefore, estimatedTokensAfter, pairingCheckRefs}`;只降不升(32768→16384→8192→4096,任一档够用就停)。派生 tool 消息:新 messageId、保留原 turnId/stepId/actionId/tool_call_id/resultSelectionRef,`origin=context_runtime`,`sourceToolMessageRef` 指原消息(该键出现时 origin 必为 context_runtime);原消息不改写,任何一次请求只选一个版本。当前档位记在上下文视图,普通后续请求不自动回升。
- **长文本/图片**(§4.51-4.52,后续棒次):用户文本 32 KiB 预览 + 原文 artifact;图片原图引用进 message,编码交给 wire。
- **后台任务**(§4.53-4.54,后续棒次):`taskId` + `task.*` 事件,`parentActionRef` 关联。
- **resume**(§4.10/§4.59,P2 读取侧增补冻结):`resume.source.attached` payload 定案 `{sourceRef:{sessionId,runId,seq,id,hash}(五键指源末行,§3.1), contextRevision, systemMessageRef, branch}`。读取侧沿 `sourceRef` 逐级回溯来源链:每级验五键 hash、按 sessionId 去重、环标 duplicate;祖先账默认按 `sessions/<id>/<id>.jsonl` 解析。resume 本身不改写源内容(坏尾修复归 §4.60)。
- **todo/goal/loop/fork/btw**(§4.55-4.58,后续棒次):独立存档;fork/btw 引入 `targetContext` 作用域,字段留挂点。

## 五、usage 唯一 owner 表(§4.12 定案)

| 情形 | owner 落点 | 规则 |
| --- | --- | --- |
| 请求成功、实报到齐 | assistant message 的 `usage` 字段 | 唯一可累计事实;结构 `{"inputTokens","outputTokens","reasoningTokens"?,"cacheReadTokens"?,"cacheWriteTokens"?,...}`,缺失子项省略键,不补 0 |
| 流中断(Esc/断流定稿) | 同上 | 已收部分照实内联;没收到 → `usage:null` |
| 请求失败、无 assistant 落盘 | `model.usage.appended` 事件 | 关联 requestId;观察承载,不参与累计 |
| 实报迟到/更正 | `model.usage.appended` 事件 | 关联 messageId/requestId;不倒改旧 message |
| compact 模型 usage | compact 内部 assistant(purpose=compact)自己的 usage | 不得充当主上下文前后数字(§4.11) |
| provider 没报 | `usage:null` + 事件里不造数 | 不补 0,不借下一请求倒填 |

`model.request.prepared` 若引用估算,只引用不复制实报;估算与实报不得混为同一字段(§4.12)。

## 六、未知版本策略与多流归属

- 未知 `kind`、未知顶层键、未知枚举值:读取侧一律拒收(错误码 `schema3.unknown_kind`/`schema3.unknown_key`/`schema3.bad_enum`),按坏行处理,不静默跳过。写入侧只发行已实现 kind。
- 多 stream 归属:一份 session 一只 JSONL 单写者;流式片段事件(model.response.*)全落本文件,凭 `streamId` 区分不同流;网络重试新开 requestId/streamId,沿原 stepId 记 `retryOf`,旧流片段不得接到新流后面(§4.43)。

## 七、fixture 与校验

| fixture | 覆盖 |
| --- | --- |
| `tests/fixtures/trajectory_v3/startup.jsonl` | 首行 system(seq=1,turnId=null,自带身份)+ `session.started`(revision 1 单节点链);无空回合 |
| `tests/fixtures/trajectory_v3/soul_switch.jsonl` | 旧 system -> `system.change` -> 新 system(回指) -> `context.system.applied` |
| `tests/fixtures/trajectory_v3/tool_round.jsonl` | 一轮对话:user 接纳 -> `model.request.prepared`(inputMessageRefs) -> 流式片段 -> assistant(来源+usage) -> 工具生命周期(简版,字段示范) -> tool message 接纳 |
| `tests/fixtures/trajectory_v3/compact_full.jsonl` | 八行全链一次成功 compact,compactId 贯穿,contextChain 重接 |
| `tests/fixtures/trajectory_v3/stream_interrupted.jsonl` | delta 批次 -> `model.response.cancelled` -> interrupted assistant 定稿(usage:null) |
| `tests/fixtures/trajectory_v3/hook_effects.jsonl` | 工具接纳 -> PreAction dispatch(started/completed/`effects.applied` 换后端) -> started(effectiveArgsRef+幂等键) -> finished -> result.persisted -> PostAction 过滤(`effects.applied` 补充) -> result.selected -> tool 消息(头尾节选预览)接纳 |
| `tests/fixtures/trajectory_v3/preview_reduction.jsonl` | 一轮工具(R1_32 接纳) -> 派生消息 R1_16(origin=context_runtime、sourceToolMessageRef) -> `context.tool_previews.reduced` 换链提交(revision 5) |
| `tests/fixtures/trajectory_v3/subagent_parent.jsonl` | 父账:assistant 声明 -> 工具接纳 -> `subagent.spawn.requested`(预留) -> started -> `subagent.linked`(检查点) -> finished -> result.persisted/selected -> 异步收据 tool 消息 -> observed 预备位 |
| `tests/fixtures/trajectory_v3/subagent_child.jsonl` | 子账:首行 system(systemMeta 派生来源:parentActionRef/taskId/spawnEventRef 五键) -> session.started -> 委派 user(origin=parent_agent)接纳 -> `task.started` |

校验:`python scripts/validate_trajectory_v3.py <file.jsonl>... [--rehash]`。逐行 schema 校验 + seq 连续 + 哈希链衔接 + 语义断言(system 链三态、compact 全链闭合、流式定稿唯一 assistant、usage 不补零)。`--rehash` 用于从无哈希的草稿生成合法链(fixtrue 维护用)。

## 八、四角色 wire 对照与现行差距(§4.46)

v3 的 system/user/assistant/tool 四角色经 adapter 结构化转换到四家 wire,不简单改 role 字符串。目标映射(单子 §4.46 原表):

| 内部含义 | Anthropic Messages | Chat Completions | Responses | Gemini Generate Content |
| --- | --- | --- | --- | --- |
| system | 顶层 `system` | `system` 消息 | 顶层 `instructions` | `systemInstruction` |
| user | user 消息内容块 | `user` 消息 | user message item(`input_text`) | contents/parts(role=user) |
| assistant 正文 | assistant content 的 text 块 | `assistant.content` | assistant message item(`output_text`) | contents/parts(role=model) |
| assistant 工具调用 | assistant.content 的 `tool_use` | `assistant.tool_calls` | `function_call` item | model 的 `functionCall` part |
| tool 结果 | user.content 的 `tool_result` | `role=tool` + `tool_call_id` | `function_call_output` item | `functionResponse` part(协议 role=user) |
| thinking | thinking 块(带 signature) | 按方言回传策略(默认不回传) | 不回传(一次性) | 不回传(一次性) |

工具结果从内部 tool 角色变成 Anthropic wire 的 user,不代表它变成真人输入:turnId、来源与 Action 仍取原始消息,不能从 wire role 倒推(§4.47 同理)。相邻同组两条 tool 可映射为一条 user 中的两个 `tool_result` 块,按稳定顺序排列并还原 provider 调用 ID;此时两个内部 messageRef 对应同一 wire message 的不同块——请求快照保留这份映射,不能假定内外消息数量一一相等。

### 8.1 现行形状基线(两角色实现)

现行 `api::Role` 只有 User/Assistant:system 单列 `Request::system`,工具结果是 User 消息内的 `ToolResultBlock`。四家现行拍平形状由合同测试册钉死:`tests/unit/api/test_wire_role_contract.cpp`(四家 × 形状矩阵 + 消息数量映射,只读现状不改 wire 行为)。要点:

- **anthropic**(`src/api/anthropic/client.cpp`):内部消息逐条对位;tool 结果留在 user 容器的 `tool_result` 块,两条相邻 tool 结果各自成条(v3 目标允许同组合并成一条 user 的两个 `tool_result` 块——现行不合并,基线钉死);thinking 块带签名按原序保真回传。
- **chat**(`src/api/chat/request.cpp`):tool 结果从 User 容器拍平成独立 `role=tool` 消息(`tool_call_id` 配对),已是目标形状;user 正文与工具结果混装时一条内部消息分裂成两条 wire;只装工具结果的 User 消息不产 user 消息(空正文不造);thinking 默认策略 Never 不回传。
- **responses**(`src/api/responses/request.cpp`):逐块成 item;thinking 跳过(reasoning 一次性);工具调用/结果各成 `function_call`/`function_call_output` item,call_id 保真。
- **gemini**(`src/api/gemini/request.cpp`):工具块各自单独成条 content;`functionResponse` 顶 role=user(协议只认函数名不认调用 id,函数名按历史 `tool_use_id` 对回);thinking 跳过。
- **数量映射是常态不等**:同一份内部对话(system 顶层 + 消息 5 条:U→A(call×2)→T1→T2→A2),anthropic 出 5 条 messages、chat 出 6 条(含 system 消息)、responses 出 7 个 input item、gemini 出 7 条 contents(合同测试册横切节钉死)。

### 8.2 差距清单(现行两角色实现 → 四角色壳要动的点)

1. `api::Role` 扩 System/Tool(或 adapter 入口另设映射层)后,所有"不是 User 就是 Assistant"的二选一分支逐处核对,只加枚举不算实现:`api/types.hpp` 的 `RoleToString`、`src/api/anthropic/client.cpp` 的本地 `RoleToString`、responses/gemini 的 `WireRole`、chat 的 User/else 主分支、`src/api/assembler.cpp` 的角色判断。
2. **chat**:独立 tool 角色消息直接落 `role=tool`(`tool_call_id` == actionId 配对);`IsUserTurnStart`/`SegmentToolUseFlags` 的"真 user 输入"判据按内部消息来源判定,不能扫 wire role 倒推(§4.47)。
3. **anthropic**:独立 tool 角色映射回 user 容器 `tool_result` 块;相邻同组合并为一条 user(现行逐条);`ShouldRecoverTaggedThinking` 的"末条 user 含 tool_result"启发式改认 tool 角色。
4. **responses**:`instructions` 与 `function_call_output` 形状已合目标;`ContentBlockToItem` 的 `input_text`/`output_text` 角色三元与独立 tool 消息对接。
5. **gemini**:`ToolNameByUseId` 对回表改认独立 tool 消息的 `tool_call_id`;`functionResponse` 的 role=user 硬编码处按映射表落位。
6. **thinking/redacted**:`ContentBlock` 无 `redacted_thinking` 原生类型,anthropic 解析器无该分支(§4.42 不透明块无损回传待补);兼容端可能返回空签名,不得按 Claude 非空签名要求虚构。
7. **请求快照**:保留内部 messageRef ↔ wire message 的映射(两个内部 messageRef 可对应同一 wire message 的不同块)。
8. **容量检查时点**:最后一道容量检查吃 adapter 与 extra_body 全部覆盖完成后的实际输入形状(anthropic 的 extra_body 尾部覆盖之后),不得在它之前估完便放行。

四角色贯通的完整验收另需从真实 v3 JSONL 读取四角色 → 上下文选取 → adapter 生成 wire → 流式响应落回 v3 的端到端链路(P2 读取侧 + 后续棒次);本节与合同测试册是那条链路的对照基线,不是其替代。

## 九、usage 消费方与取数口(§4.12)

§五 owner 表冻结后,原各造各账的消费方按下表收敛取数。本节为 P4 盘点,**不实现消费**;折算钩子(键集 → `api::Usage` 口径)由 `tests/unit/trajectory_v3/test_v3_usage_owner_hooks.cpp` 钉死,活口径锚点(`TotalInputTokens`、明报位)钉在 `tests/unit/api/test_wire_role_contract.cpp` 末节。

| 消费方 | 现行取数 | v3 取数口 | 依赖 P2 读取侧 |
| --- | --- | --- | --- |
| `/usage` 命令(`src/app/commands/usage_commands.cpp`) | v2 Journal 的 UsageSample 流 | assistant message 的 `usage`(唯一可累计事实)+ `model.usage.appended`(失败/迟到/更正观察,不参与累计) | 是 |
| token 账本五层聚合(`src/accounting/usage_aggregate.hpp`) | UsageSample(v2 事件投出) | sample 的 provider usage 改吃 v3 owner;估算栏吃 `model.request.prepared` 引用的 tokenEstimateRef(估算 hook,后续棒次) | 是 |
| cost 估算(`src/accounting/cost_estimator`) | UsageSample + 价格表 | 随账本同源;compact/标题等内部请求的 usage 各入各账,不混主上下文 | 是 |
| token 校准器(`src/agent/token_calibrator.hpp`) | 活事件流(`assembler.usage_seen()` + 请求字节账) | 实报侧:assistant `usage` 按完整输入口径(`TotalInputTokens`);本地侧:prepared 引用的估算与请求特征(§4.12:特征/标签对齐才谈得上免重放回测) | 回测/跨会话面是 |
| 会话活账(`src/app/turn_usage_account.hpp`、`src/cli/context_tracker.hpp`) | `on_usage` 活事件流(UsageReport) | 活路径不变,不经文件;resume 后显示历史需读 v3 | 显示历史面是 |
| compact 触发与 token 显示(§4.11) | 估算,不吃实报 | 估算 hook(后续棒次);provider usage 只做事后对照 | 否(估算 hook 另计) |
| 前缀缓存守恒账(`src/agent/prefix.hpp`) | UsageReport 诊断字段(活路径) | 逐请求指纹照旧;跨会话对账需 v3 请求特征 + usage 对齐 | 跨会话面是 |

规矩(§4.12/§五):request_metrics 统一为 event,引用同一 usage 时不得成为第二份可累计事实;估算与实报不混同一字段(prepared 只引用 `tokenEstimateRef`,不复制实报);provider 没报不补 0,不借下一请求倒填。
