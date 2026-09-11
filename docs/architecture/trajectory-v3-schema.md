# session 轨迹 v3 schema(冻结稿)

[当前实现](session-v3.md) · [会话指南](../features/sessions/README.md) · [待清理与缺口](../development/v3-legacy-audit.md)


状态：新会话已默认写 v3，写入、读取、历史显示、四角色 adapter、compact 与同场内存换账均有实现。只有 `LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0` 使新场回 v2；旧场按源格式读取，不自动转换。本页冻结字段合同，不把“字段已有”视为每个生产入口均已接通。

设计目标见 `todos/session轨迹v3_消息主轴树链与四角色壳收敛设计.todo`；源码与测试定当前行为。新增 Goal/Loop/btw/Memory 及完整 AppServer/Workflow 合同仍按各自分期实施。旧接口与消费方差距见清理清单，不沿用早期 P0/P1/P2 状态推断今日实现。

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
| `purpose` | string | 必填,枚举:`conversation`/`compact`/`context_summary`/`session_title`/`capability`/`goal_evaluation`/`action_summary`(capability 留给 §4.29 渐进披露;goal_evaluation 为 §4.67.6 验收内部回合:实际 system/user/assistant,不进 main 输入链,默认折叠;action_summary 为 B2 整批结果压缩的摘要模型内部问答,同样默认隐藏不入 main 链,合同见 [新 action 摘要](context/v3-action-summary.md)) |
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
| goal 控制状态 | `state.goal.applied`(§4.67 G0:goal 状态唯一生效点,快照不可变文件 + 提交锚,不带 status) |
| goal 验收族(§4.67.6 G2) | `goal.checkpoint.recorded`、`goal.evidence.recorded`(收口事实行,不改活动 head)、`goal.evaluation.requested`(材料版本冻结:evaluationId/contractRevision/evidenceSetHash)、`goal.evaluation.completed`(判词到手,不等于目标已完成)、`goal.evaluation.rejected`(候选被拒,带 reason)——全部不带 status |
| goal 等待/usage 族(§4.67.6/§4.67.7 G3) | `goal.wait.registered`(登记后台等待:taskRefs/notifyDedupeKey/inspectionPlan;等待是否生效仍看 applied)、`goal.wait.resolved`(等待解除:deliveryKey 去重 + reason;迟到解除不改账)、`goal.usage.recorded`(逐 requestId 的 usage 归属与计量来源;(sessionId,requestId) 去重,投影累计不重复计费)——全部不带 status |
| workflow 编排 | `workflow.definition.loaded`、`workflow.segment.opened`、`workflow.inputs.committed`、`workflow.node.reserved`、`workflow.node.dispatched`、`workflow.node.waiting`、`workflow.node.retrying`、`workflow.node.completed`、`workflow.node.failed`、`workflow.node.cancelled`、`workflow.node.skipped`、`workflow.output.committed`、`workflow.checkpoint.committed`、`workflow.branch.started`、`workflow.join.completed`、`workflow.loop.iteration.started`、`workflow.loop.iteration.completed`、`workflow.run.completed`、`workflow.run.failed`、`workflow.run.cancelled` |


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
| 其余(`session.started`、`system.change`、`model.request.prepared`、`model.response.started`/`.delta`、`compact.requested`、`compact.range.retreated`、`context.*.applied`、`state.goal.applied`、`goal.checkpoint.recorded`、`goal.evidence.recorded`、`goal.evaluation.requested`/`.completed`/`.rejected`、`goal.wait.registered`/`.resolved`、`goal.usage.recorded`、`input.*`、`resume.source.attached`、`subagent.observed`、`command.received`、`hook.dispatch.requested`、`hook.skipped`、`title.*`、`session.title.applied`、`tool.result.persisted`/`persist_failed`/`selected`、`hook.effects.applied`/`rejected`、`model.usage.appended`、workflow 事实记录族:`workflow.definition.loaded`/`workflow.segment.opened`/`workflow.inputs.committed`/`workflow.node.reserved`/`workflow.node.dispatched`/`workflow.node.retrying`/`workflow.node.skipped`/`workflow.output.committed`/`workflow.checkpoint.committed`) | 不携带 status 字段 |

生命周期规则(§4.14):同一操作可以多条 event,各持自己的 eventId/seq,共用操作身份;每次尝试最多一个执行终态;终态后迟到响应另记观察事件不改旧终态;`pending` 是"在等"、`running` 是"在执行";崩溃后见 `started` 无终态只能判"可能已执行"。

模型请求三段语义(失败与恢复单 P1-C/FA-03):`model.request.prepared` = 准备发送(引用先落稳才许发);`model.request.sent` = 本地交给 transport,`payload.deliveryScope="local_transport"` 钉死本地交接——不暗示已拿到远端收据,服务端事实只看 `model.response.*`;`model.response.*` 各事件才是远端确认。sent 这笔写不稳时请求不得上 wire(发送前写账硬闸)。

tool 消息回喂语义(失败与恢复单 P1-B/FA-02):最终 tool 消息本体可带 `message.is_error=true`(只写真值,缺键 = 成功)——语义以 Hook 处理后真正交给模型的结果为准,不从执行终态猜;恢复投影(EffectiveConversationFromV3 → ProjectHistoryFromReplay)从本体原样还原。工具折叠新增两档缺口态(失败与恢复单 P1-A/FA-01):`result_missing`(执行已有终态、结果链没立起来)与 `message_not_admitted`(tool 消息已写、接纳未成)——都与 `selected_no_message` 一样进 resume 的 open_actions,补保存/补接纳/补消息,不重跑工具。

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
 -> [model.request.sent] -> model.response.started(预留候选 messageId) ->
    model.response.completed(候选定稿;非流式压缩客户端零 delta 批,§4.43) ->
    assistant 回复 message(purpose=compact,候选产物,以预留 id 成行)
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

`artifactRef`(结果仓,§4.16):`{"artifactId","kind","path","sha256","bytes","mediaType"}`,kind ∈ `result_metadata|stdout|stderr|combined|raw_payload|report|image|blob`;数组 `result_ref` 固定为数组,只有一份也 `[ref]`,没有写 `[]`。`blobRef` 承继 v2 BlobRef 五键(`sha256/size/media_type/encoding/compression`)。

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
- **结果仓**(§4.16-4.17,已发行):目录 `sessions/<sessionId>/artifacts/`,`res-<六位号>.json` 为不可变描述(先临时文件、再落稳、再发布不可变名;POSIX rename 会静默覆盖,故显式查存在性)。描述文件 snake_case(`result_id`/`tool_call_id`/`attempt`/`result_kind`/`execution_event_ref`/`preview_policy`/`capture_limits`/`outputs[]`,outputs 项 `channel/ref/output_bytes/byte_count_kind/captured_bytes/capture_complete/capture_reason/encoding`);JSONL 内 result_ref 六键 camelCase(§3.1)。预览:完整渲染 ≤ 预算原样返回;超限先留说明预算 M、正文 B=预算-M 头尾均分(floor(B/2)/余),UTF-8 边界对齐,头尾段各重标 `[文件: path | 通道: channel]`,未展示正文文件点名 `not_shown`;清单超限先存 `output_index` artifact 再列容纳得下的路径 + `omitted_output_count`;`full_output`/`captured_output` 恒为数组,全部不完整时 `full_output=[]`;4 KiB 仍装不下必要来源记 `preview_unrepresentable`。

B1 首次预览接线补充：工具真实返回、执行终态落稳后，先在 `capture-<六位号>.*` 保存原始捕获，再运行 PostToolUse。`res-<六位号>.*` 保存加入 hook 反馈后的有效材料；两套编号互不挤占。两次 `tool.result.persisted.executionEventRef` 都指向同一执行终态，不能指向上一笔 persisted。`tool.result.selected.sourceResultEventRefs` 按原始捕获、有效材料的顺序列来源；未执行的拒绝结果只有有效材料。

原始捕获描述用 `preview_policy.policy=raw-capture-before-post-hook`，它还不是模型采用的预览。有效材料描述另存当次 `maxPreviewBytes`。富结果另有 `raw_payload.json`，保存原始文本、结构与媒体引用，不复制临时 wire base64。`raw_payload` 同时进入 artifactRef.kind 的允许值。`combined` 与 `raw_payload` 在首次预览中共享字节预算。单个 text 块若与 combined 逐字相等，不重复保存 raw_payload。配额导致捕获中断时，两份描述都保留 `capture_complete=false` 和 `capture_reason=quota`。

选用、tool 消息与接纳全部成功，运行时才发布预览。原始捕获失败时不运行后续 post-hook；后续任一道写闸失败也不发送下一请求。已经成功保存的原文及事件不回滚。请求清洗不能从富块重新投影已经采用的正文；Gemini 也不能绕过预览，改发旧 `structured_content`。原始字段仍在结果仓。

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
- **goal 控制状态**(§4.67 G0,已发行):`state.goal.applied` 为 goal 状态唯一生效点。payload 定案 `{goalId, fromStateRevision, toStateRevision(=from+1), contractRevision, snapshotRef, snapshotSha256, lifecycle, causeRef?, adoptedFrom?}`;`lifecycle ∈ preparing|active|waiting|paused|awaiting_user|blocked|budget_exhausted|suspended_by_policy|achieved|cleared|failed`,工作相位(phase:idle/queued/running/evaluating)记在快照。goalId 走 payload(goal 是控制状态,不占信封身份字段族);`causeRef` 为合法引用(§3.1,可缺)。`adoptedFrom`(§4.67 G1,可选)为跨卷续接凭据 `{sessionId, stateRevision}`:resume-as-new 后 goal 从来源卷接管,新卷首条 applied 的 fromStateRevision != 0,须带它且 `stateRevision == fromStateRevision`;读取侧 `ProjectGoalLineage` 沿 resume 来源链(只穿 `start_reason=resume` 边,clear/fork 断链)取最近一份有 goal 账的卷为 head。完整 goal 状态存不可变快照 `sessions/<id>/state/goals/<goalId>/rev-<六位>.json`(先临时文件再改名,不覆盖;hash 为文件字节 sha256;同字节重试复用候选文件——崩溃窗口"快照落稳、applied 未落"的幂等补账),本行只记提交锚;候选快照落稳但 applied 未落时不生效。跨行合同(校验脚本与读取侧投影同钉):同 goal 的 applied revision 逐条 +1;一链最多一枚未收账 goal,换 goal 时前一枚须已 terminal;terminal(achieved/cleared/failed)后同 goal 不得再有 applied;applied 指向的快照缺失/hash 不符时投影报缺口,不用摘要猜。快照 `pendingIntent`(§4.67 G1)为续跑意图 `{workItemId, contractRevision, predecessorIterationId, continuationOrdinal, triggerRef, nextActionRef, claimed, writerEpoch, claimedAtMs}`:去重键 `(goalId, contractRevision, predecessorIterationId, continuationOrdinal)`;claim 落账(claimed=true + writerEpoch + phase=queued 的 applied)即消费,重复 resume 不重复提交;他写者已认领的意图不盲重放(恢复核验)。goal 的其余 kind:checkpoint/evaluation 族已随 §4.67 G2 发行——`goal.checkpoint.recorded` payload `{goalId, iterationId, checkpoint(object), synthesized(bool)}`;`goal.evidence.recorded` payload `{goalId, iterationId, evidenceId, evidence(GoalEvidenceRef), facts?}`(facts 为判材料回溯);`goal.evaluation.requested` payload `{goalId, iterationId, evaluationId, contractRevision(>=1), evidenceSetHash(hex64)}`(材料版本冻结);`goal.evaluation.completed` payload `{goalId, evaluationId, decision(continue|achieved|blocked|needs_user), evaluationMessageRef(合法引用,指判词 assistant), requestRefs[]}`;`goal.evaluation.rejected` payload `{goalId, evaluationId, reason}`。验收模型请求经宿主内部请求服务留账:验收专用 system(purpose=goal_evaluation, turnId=null, systemMeta.cause=goal_evaluation)与 user/assistant(挂 `goaleval-turn-<n>` 内部回合,parentTurnId 回指工作轮,display=collapsed,不经 AdmitMessages、不进 main contextChain);assistant 照记 provider/wire/model 与逐次 usage(repair 各成一条请求,费用各记)。wait/usage 族已随 §4.67 G3 发行——`goal.wait.registered` payload `{goalId, taskRefs(非空 string 数组), notifyDedupeKey(通知合并去重键), inspectionPlan(object: pollsDone>=0/maxPolls>=1/nextDueMs>=0)}`(巡检次数入快照、重启不归零;等待是否生效仍看随后 applied);`goal.wait.resolved` payload `{goalId, deliveryKey, reason}`(交付去重;pause/clear 后迟到解除不改账);`goal.usage.recorded` payload `{goalId, requestId, source(execution/evaluator/subagent/…), usage(object: 各 token 计量非负 + usageReported)}`((sessionId,requestId) 去重、投影累计不重复计费;后台/子代理 usage 走此路,判词与验收请求的逐次费用在 G2 的逐请求账,不在此重复落)。continuation 事件族(claimed/finished 的独立 kind)与 todo/loop 归后续棒次(claim/采用/续排的账面锚暂为相应 state.goal.applied)。
- **todo/loop/fork/btw**(§4.55-4.58,后续棒次):独立存档;fork/btw 引入 `targetContext` 作用域,字段留挂点。
- **workflow 编排账**(Workflow 接入 v3 第一棒,已发行):编排账不是 agent 会话——经事件账 writer profile(`trajectory::v3::V3EventLedger`)只写 `type=event` 行,不造 system 首行、不写 message 行。目录 `workflow-runs/<workflowRunId>/{definition.json, bindings.json, inputs.json, segments/<segmentId>/workflow.jsonl, checkpoints/<checkpointId>.json, outputs/<outputId>.json, nodes/, artifacts/, subflows/}`(`WorkflowPathResolver` 统一解析,与 session 目录体系并列不混淆)。信封语义:`sessionId`=workflowRunId,`runId`=orchestrationSegmentId;每段 seq 从 1 起,恢复开新段并以 `workflow.segment.opened` 的 `sourceRef` 五键链接旧段水位。身份分层:`nodeExecutionId`=`<runId>-<nodeId>[-i<mapIndex>][-d<dispatch>]`(dispatch 号跨恢复延续),attempt 追加 `-a<n>`;`outputId`=`out-<六位号>`、`checkpointId`=`cp-<六位号>` run 内单调。关键载荷合同:definition.loaded 必带 `workflowId`/`definitionHash`(hex64);node.reserved 必带 `nodeId`/`nodeExecutionId`/`nodeKind`/`attempt`/`inputHash`(输入快照内容寻址);output.committed 必带 `nodeId`/`nodeExecutionId`/`outputId`/`outputHash`(hex64)/`outputRef`/`validation{passed,checks}` 与可选 `resolvedInputHash`——它是"节点产物可供下游消费"的唯一依据(失败/取消绝不写它);checkpoint.committed 必带 `checkpointId`/`checkpointRef`/`sha256`/`throughSeq`,孤立 checkpoint 文件不生效;node.completed.outcome 只取 `success|empty`(失败走 node.failed,恢复判据看 commit 不看事件名);run.* 终态每账至多一枚。无损纪律:outputs/inputs/checkpoint 原件不脱敏(展示/导出脱敏另做投影),payload hash 不符即拒恢复;node/skill/agent 子账与父 run 的 `nodeExecutionRef` 关联归后续棒。
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

### 8.1 迁移前形状基线（历史参考）

**V3-LEGACY-04：** 下列记录来自四角色迁移前，供 wire 回归对照。当前 `api::Role` 已含 System/User/Assistant/Tool，四家 adapter 已支持；主循环仍有 User 工具容器，不能把“枚举已扩”说成所有内部路径已迁完。旧基线中 system 单列 `Request::system`，工具结果装在 User 消息内。四家现行拍平形状由合同测试册钉死:`tests/unit/api/test_wire_role_contract.cpp`(四家 × 形状矩阵 + 消息数量映射,只读现状不改 wire 行为)。要点:

- **anthropic**(`src/api/anthropic/client.cpp`):内部消息逐条对位;tool 结果留在 user 容器的 `tool_result` 块,两条相邻 tool 结果各自成条(v3 目标允许同组合并成一条 user 的两个 `tool_result` 块——现行不合并,基线钉死);thinking 块带签名按原序保真回传。
- **chat**(`src/api/chat/request.cpp`):tool 结果从 User 容器拍平成独立 `role=tool` 消息(`tool_call_id` 配对),已是目标形状;user 正文与工具结果混装时一条内部消息分裂成两条 wire;只装工具结果的 User 消息不产 user 消息(空正文不造);thinking 默认策略 Never 不回传。
- **responses**(`src/api/responses/request.cpp`):逐块成 item;thinking 跳过(reasoning 一次性);工具调用/结果各成 `function_call`/`function_call_output` item,call_id 保真。
- **gemini**(`src/api/gemini/request.cpp`):工具块各自单独成条 content;`functionResponse` 顶 role=user(协议只认函数名不认调用 id,函数名按历史 `tool_use_id` 对回);thinking 跳过。
- **数量映射是常态不等**:同一份内部对话(system 顶层 + 消息 5 条:U→A(call×2)→T1→T2→A2),anthropic 出 5 条 messages、chat 出 6 条(含 system 消息)、responses 出 7 个 input item、gemini 出 7 条 contents(合同测试册横切节钉死)。

### 8.2 四角色迁移记录与剩余边界

早期八项改造已落到四角色枚举、四家 adapter、RedactedThinkingBlock、WireMessageMap 与有效输出上限读取。对照测试在 `test_wire_role_contract.cpp`，新增内核测试在 `test_four_role_kernel.cpp`。

剩余边界是生产内部容器与消费链：`src/agent/loop.cpp` 仍用 User 装一批 ToolResultBlock，v3 写桥拆成独立 tool 行；相邻 tool 的 wire 合并与 messageRef 映射须按真实出口核对，不能拿合同中的“可合并”当成已实现。旧 v2 特有块序列化也不据 v3 测试判定无损。

第三棒(6/7/8 三条)落点:第 6 条——内核补 `RedactedThinkingBlock`(variant 尾部追加,只增不改),anthropic 解析器认 `redacted_thinking` 原生块(整块随 content_block_start 到齐),assembler 落事实块,anthropic wire 出口不透明 data 原样回传、空签名照实不虚构;chat 的 reasoning 回传与 responses/gemini 的一次性思考路都不吃它(载荷一个字节不出门),K2.6 回传路零回退。v2 会话档(runtime 侧 `MessageToBlocksJson` 的 get_if 链)尚未收录该块,resume 后不带回——归 runtime 在途 PR。第 7 条——`api::WireMessageMap`(container + message_to_wire + wire_element_count)由四家 `BuildMessageWireMap` 与 `BuildRequestJson` 同一条拼装路产出,经 `api::Backend::BuildWireMessageMap` 虚函数与 `RequestPreparedContext::wire_message_map` 挂上既有请求快照路径(schema 不动,无新事件 kind)。第 8 条——`Backend::GetEffectiveOutputLimit` 按 extra_body 覆盖序(provider 级先、请求级后;anthropic/chat 键 `max_tokens`、responses `max_output_tokens`、gemini `generationConfig.maxOutputTokens`)报有效上限,loop 最终硬闸的输出预留与降级判定认这份;应急/降级收窄经 `ForceMaxOutputTokensOverride` 写进请求级覆盖位,窄值真出门。钉子在 `tests/unit/api/test_four_role_kernel.cpp`(第三棒三节)与 `tests/unit/agent/test_loop.cpp`(差距 8 两案)。

读取、wire 与写回现已有独立测试和 `scripts/tests/v3_accept_matrix.py` 生产 exe/假后端验收入口。合同测试与集成验收分别看，不把历史发行记录冒充本轮重跑，也不据此宣称真实模型摘要质量已验证。

## 九、usage 消费方与取数口(§4.12)

§五 owner 表冻结后,原各造各账的消费方按下表收敛取数。本节记录消费方迁移边界，**不代表全部消费方已接 v3**;折算钩子(键集 → `api::Usage` 口径)由 `tests/unit/trajectory_v3/test_v3_usage_owner_hooks.cpp` 钉死,活口径锚点(`TotalInputTokens`、明报位)钉在 `tests/unit/api/test_wire_role_contract.cpp` 末节。

| 消费方 | 现行取数 | v3 取数口 | 依赖 P2 读取侧 |
| --- | --- | --- | --- |
| `/usage` 命令(`src/app/commands/usage_commands.cpp`) | v2 Journal 的 UsageSample 流 | assistant message 的 `usage`(唯一可累计事实)+ `model.usage.appended`(失败/迟到/更正观察,不参与累计) | 是 |
| token 账本五层聚合(`src/accounting/usage_aggregate.hpp`) | UsageSample(v2 事件投出) | sample 的 provider usage 改吃 v3 owner;估算栏吃 `model.request.prepared` 引用的 tokenEstimateRef（估算槽位已接，离线消费仍待迁移） | 是 |
| cost 估算(`src/accounting/cost_estimator`) | UsageSample + 价格表 | 随账本同源;compact/标题等内部请求的 usage 各入各账,不混主上下文 | 是 |
| token 校准器(`src/agent/token_calibrator.hpp`) | 活事件流(`assembler.usage_seen()` + 请求字节账) | 实报侧:assistant `usage` 按完整输入口径(`TotalInputTokens`);本地侧:prepared 引用的估算与请求特征(§4.12:特征/标签对齐才谈得上免重放回测) | 回测/跨会话面是 |
| 会话活账(`src/app/turn_usage_account.hpp`、`src/cli/context_tracker.hpp`) | `on_usage` 活事件流(UsageReport) | 活路径不变,不经文件;resume 后显示历史需读 v3 | 显示历史面是 |
| compact 触发与 token 显示(§4.11) | 估算,不吃实报 | PreRequest 估算槽位已接；provider usage 只做事后对照 | 否(估算 hook 另计) |
| 前缀缓存守恒账(`src/agent/prefix.hpp`) | UsageReport 诊断字段(活路径) | 逐请求指纹照旧;跨会话对账需 v3 请求特征 + usage 对齐 | 跨会话面是 |

规矩(§4.12/§五):request_metrics 统一为 event,引用同一 usage 时不得成为第二份可累计事实;估算与实报不混同一字段(prepared 只引用 `tokenEstimateRef`,不复制实报);provider 没报不补 0,不借下一请求倒填。

## 十、v2 事件在 v3 的未覆盖清单(接线点 1 收尾棒盘点)

v3 写侧接线(接线点 1)后,v2 事件表里有一批在 §二 kind 全表里没有对应物、或对应 kind 的合同尚未发行的条目。下列未覆盖项的写侧仍有早退不落分支(不伪造行;`src/runtime/trajectory_session.cpp` 各早退处注记),本节把这批账摊开登记——新 kind 只许随 schema 版本追加,本清单不为补缺擅自加 kind:

| v2 事件(v2 kind) | v3 对应 | 处置 |
| --- | --- | --- |
| 环境快照(`run.environment.captured`,§9.1:os/git/provider 快照) | 无 kind。v3 会话身份在账首行,环境重现(P0-4 取材件)不在 v3 目标内 | 不落;`CaptureEnvironment` 对 v3 场无写者,如实返回 no_recorder |
| 手动/采纳标题(`control.title.changed`,/title 与自动采纳共用) | `title.requested`/`title.extracted`/`session.title.applied` 族在 §二表内,但绑定 `titleGenerationId`(§4.34 自动取题流,后续棒次),payload 定案未发行;手动改名没有生成身份,伪造 `titleGenerationId` 即造假 | 暂不落,归 §4.34 标题棒次一并接(届时手动改名按"标题来源=manual"入 `session.title.applied`) |
| 审批档切换(`control.mode.changed`) | 无 kind。审批档是 v2 session.json/manifest 的账;v3 场只在内存生效(接线点 1 既定口径) | 不落;切档内存生效,`session.v3_approval_mode_memory_only` 如实回告 |
| 容量预检(`context.pressure.recorded`) | 无 kind。v3 的容量/压缩账是 compact 一族(§4.40 容量字段后续棒次) | 不落 |
| 任务 turn 账(sent 边界数字,§11.1) | 无 kind。`model.request.sent` 本身照落 | 不落 |
| verification/恢复注记/迟到响应(`tool.verification.*`/`recovery.*`/迟到 mcp 响应) | 无 kind。v3 工具账是 `tool.execution.*`/`tool.result.*`(§四,已发行);verification 一族不在 v3 目标内 | 不落 |
| `run.started`/run terminal/`session.clear_requested` | 不沿用 v2 run 生命周期事件（公共信封仍有 runId）:开场 = 首行 system + `session.started`;封口 = `session.ended`(clear 换账时 payload 带 `nextSessionId`);无 session.json | 已有对应(clear 八步的 v3 折算见 `SessionManager::ClearV3Locked`) |

清点口径:凡写侧早退不落的,读取侧(两份投影/resume/verify)不因缺这些行报错——它们从未属于 v3 账;需要这些事实的消费方(`/doctor` 环境核对、标题真值回填)在 v3 场按"缺件"处理,不从当前环境补造过去(§4.12 同门)。

### Goal adopted verdict and progress recovery

Goal snapshots retain `appliedEvaluation` alongside `appliedEvaluationId`. The value contains the adopted verdict, including host overrides; the next evaluation and resume use this committed value, rather than treating an unadopted model reply as the previous decision. It is null before a verdict is adopted.

The host fingerprints evidence facts and criterion status for continuation. Identical material increments `counters.noProgressStreak`; changed material resets it. Reaching `budget.maxNoProgressIterations` commits a paused state and removes the pending continuation in the same snapshot. Model narrative and newly assigned evidence ids do not reset this counter. Waiting does not increment it.
