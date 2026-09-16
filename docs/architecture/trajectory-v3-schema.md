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
| `purpose` | string | 必填,枚举:`conversation`/`compact`/`context_summary`/`session_title`/`capability`/`goal_evaluation`/`action_summary`/`memory_extract`(capability 留给 §4.29 渐进披露;goal_evaluation 为 §4.67.6 验收内部回合:实际 system/user/assistant,不进 main 输入链,默认折叠;action_summary 为 B2 整批结果压缩的摘要模型内部问答,同样默认隐藏不入 main 链,合同见 [新 action 摘要](context/v3-action-summary.md);memory_extract 为回合收尾记忆抽取的旁路内部回合——system 白名单放行它,system turnId 恒 null,user/assistant 挂 `memory-turn-<n>` 内部回合、parentTurnId 回指触发主回合,不经 AdmitMessages、不进 main contextChain,prepared 的 purpose 报 `memory_extract` 并带 `timeoutBudgetSecs` 预算) |
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
| 异步工具(异步工具单 P0;合同与 fixture 已验,生产未接) | `tool.job.registered`、`tool.job.dispatched`、`tool.job.observed`、`tool.job.cancel_requested`、`tool.delivery.prepared`、`tool.delivery.acknowledged`、`tool.delivery.uncertain`、`tool.capability.recorded` |
| Gateway 常驻(常驻总装 V0;合同与 fixture 已验,生产装配归 V1) | `gateway.work.bound`(work↔turn 绑定事实:恢复器凭 workId 反查原轮,不另派新轮)、`reply.selection.committed`(对外回复的选定事实:原件先落稳、选择事实后提交,resume 后 selectionId 不变) |
| 提示组合(应用Worker接入单 §五 134) | `prompt.composition.applied`(部署档组合系统提示的可追溯事实:组合次序、各段渲染正文 hash、来源层与最终快照 ID;业务文本伪装不了宿主权限——来源逐段在账) |
| 记忆(记忆抽取取消误报 ESC 单 Bug 2) | `memory.extraction.assessed`(回合收尾的抽取门控与结果评估:turnId 挂触发主回合;载荷 camelCase——trigger/decision/skipReason 或 extractOutcome+errorCode+extractWallMs+foregroundTailMs+userTextStats/usage)、`memory.write.receipted`(四路写路 save/forget/accept 的排队/被拒回执:source/operation/outcome/layer/jobId 或 errorCode)——全部不带 status,v2 同名事件的 v3 对应 |
| 记忆续(T08/V3-GAP-03,Session v3 旧设计清理单) | `memory.recall.injected`(一条记忆真正注入模型的事实:主会话注入=正文快照落 display=hidden 的正式 user 消息(origin=context_runtime,不冒充人类输入)经 AdmitMessages 接纳进链,事件载荷 camelCase 带 memoryId/memoryLevel/memorySchema/memoryUpdatedAt/contentSha256/messageRef/injectedBytes/sourceEvidenceRefs,turnId 挂注入服务的主回合;派工冻结=targetRunId 非空,正文 snapshotInline(≤512B)或 snapshotRef 进载荷,父账不写隐藏消息不进链——父模型没见过这段;快照消息/接纳落不稳则本次不注入,§9.2 fail-closed)、`memory.save.requested`(写入因果边:request{operation/layer/kind/memoryId/title}+sourceSession+originator;requested 只记"谁发起了一笔写",排队成败看 `memory.write.receipted`,落盘回执在 workspace lifecycle 的 memory.save.committed——三态按真实回执分账)——全部不带 status |
| 渠道远端审批(QQ 接入单 Q6 §12.2) | `channel.approval.requested`(宿主发出的审批卡事实:tokenHash/tool/argsSha256/身份摘要/deadlineMs,token 只入 hash、参数只入 hash——脱敏由宿主摘要层保证)、`channel.approval.resolved`(裁决事实:decision ∈ approved/declined/timeout/cancelled/card_failed,by=操作者或收口原因;interactionId 是平台回调身份)——全部不带 status(账不裁决,决议生效在审批 broker) |


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
| 其余(`session.started`、`system.change`、`model.request.prepared`、`model.response.started`/`.delta`、`compact.requested`、`compact.range.retreated`、`context.*.applied`、`state.goal.applied`、`goal.checkpoint.recorded`、`goal.evidence.recorded`、`goal.evaluation.requested`/`.completed`/`.rejected`、`goal.wait.registered`/`.resolved`、`goal.usage.recorded`、`input.*`、`resume.source.attached`、`subagent.observed`、`command.received`、`hook.dispatch.requested`、`hook.skipped`、`title.*`、`session.title.applied`、`tool.result.persisted`/`persist_failed`/`selected`、`hook.effects.applied`/`rejected`、`model.usage.appended`、workflow 事实记录族:`workflow.definition.loaded`/`workflow.segment.opened`/`workflow.inputs.committed`/`workflow.node.reserved`/`workflow.node.dispatched`/`workflow.node.retrying`/`workflow.node.skipped`/`workflow.output.committed`/`workflow.checkpoint.committed`)、异步工具事实族(单 P0):`tool.job.registered`/`dispatched`/`observed`/`cancel_requested`、`tool.delivery.prepared`/`acknowledged`/`uncertain`、`tool.capability.recorded`、Gateway 常驻事实族(常驻总装 V0):`gateway.work.bound`、`reply.selection.committed`、提示组合事实族(应用Worker接入单):`prompt.composition.applied`、记忆事实族(取消误报 ESC 单 Bug 2):`memory.extraction.assessed`、`memory.write.receipted`、记忆事实族续(T08/V3-GAP-03):`memory.recall.injected`、`memory.save.requested` | 不携带 status 字段 |

异步工具族(单 P0)整体 statusless:`registered`/`dispatched`/`acknowledged` 只表示事件已发生,不等于业务 job 已完成;执行与投递状态由 payload(`observedStatus`)与读取侧投影表达。**unknown 是执行投影状态**(`registered → queued → running → succeeded/failed/cancelled`,`running` 查不明为 `unknown`;审批未过停 `awaiting_approval`),不硬塞信封 status——后缀 `registered`/`dispatched`/`observed`/`acknowledged`/`uncertain`/`prepared`/`recorded` 均不在 §2.2 生命周期表。

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

容量恢复选中当前 turn 内闭合旧 step 时，`compact.started` 与 `compact.applied` 另带 `stepScope: {turnId, stepIds, prefixChanged: true}`。`stepIds` 依原链排序，取消息信封与 action 账中真实身份，不用 seq 推算。没有 step 摘要时该对象为空。移除/保留消息仍由 `removedMessageRefs` / `retainedMessageRefs` 逐项列明；用户原文留在新链，最新 step 与未闭合组不入摘要范围。reader 的 `CompactMarkerView.step_scope` 公开这份范围，resume 仍按已提交 `contextChain` 恢复。

step 摘要覆盖已执行工具时，候选 manifest 必带 `executed_actions`。每项逐字保留宿主提供的 `actionId`、`operation`（工具名与参数）、`executionStatus`、`resultOutcome`、`evidenceRefs`；遗漏或改写则拒收候选。这样，后续模型仍能区分已经执行的操作、执行终态与回喂结果，不把旧操作当作待执行工作。操作参数本身过大、保留这些事实后没有缩减收益时，停止本次摘要。

冻结前核对源 revision、step 身份、移除消息与保留消息。摘要内部请求只含选中闭合组和保留用户输入，不夹带未闭合工具调用。发现签名或不透明思考载荷时，本实现报 `compact.signature_prefix_incompatible`，不复制旧签名后假称兼容。当前生产持久化路径不写 `signature`/`encrypted_content`，也不落 `redacted_thinking`（thinking 块只存 type/text，签名丢弃），此检查是对未来保真写侧的保守前置防线，生产输入上通常不触发；adapter 级“目标模型是否要求签名前缀一致”的真核验尚未接线，留待后续单。范围替换会改请求前缀，不能据此保证服务端缓存命中。

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

- **会话**(Resume 接入 v3 单 R2 增补):`session.started` payload 定案 `{writerVersion, context{contextId,revision,contextChain}, launchCwd?, runKind?}`。`launchCwd`/`runKind` 是会话级事实(建场时启动 cwd 与 main run 种类,枚举同 v2 manifest.run_kind:`main_session`/`one_shot`/…),列表投影(选择器/`/sessions`)以此为 cwd/run_kind 的权威来源;子账(subagent)不带会话语义不写键,老档缺键读作"未知",不暗填 `main_session`。

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
- **异步工具**(异步工具单 P0,合同与 fixture 已验、**生产未接**;拆开 execution / protocol obligation / delivery 三件事):身份映射(单 §5 逻辑名 → 落点,不另造重复身份)——`actionId`+`attempt`=信封 actionId+payload attempt(复用 tool 族);`jobId`/`deliveryId`/`ownerEpoch`/`wireCallRef`/`resultRef`+`resultVersion`=payload(同 goalId 口径);`targetRequestId`=信封 requestId(不拿 deliveryId 代替);`originRef`=信封 turnId/stepId+payload `assistantMessageRef`。载荷合同:`tool.job.registered` 必带 `jobId`/`mode ∈ job_handle|native_deferred`(inline 不注册 job)/`assistantMessageRef`(合法引用)/信封 turnId+stepId;native_deferred 另必带 `wireCallRef {provider,wire,callId,async:bool[,responseId,itemId]}`;可选 `approvalRequired`(停 awaiting_approval 不派发)、`executionPolicy`(字段集拟议待 P1 确认:`allow_background`/`side_effect_class`/`resource_keys`/`retry_policy`/`deadline_ms`/`resume_policy`/`max_output_bytes`,先钉类型不钉枚举)。`tool.job.dispatched` 必带 `jobId`/`ownerEpoch`(租约代号,细节拟议待 P1)。`tool.job.observed` 必带 `jobId`/`observedStatus ∈ registered|queued|running|succeeded|failed|cancelled|unknown|awaiting_approval`;`resultRef`/`resultVersion` 成对出现或都不出现(进度不占终态版本);attempt 可缺(观测可由宿主发起)。`tool.job.cancel_requested` 必带 `jobId`(取消是请求不是终态)。`tool.delivery.prepared` 必带 `deliveryId`(resultVersion+目标分支+用途的稳定去重键)/`resultRef`/`resultVersion`/信封 requestId;`tool.delivery.acknowledged` 必带 `deliveryId`/`evidenceRef`(没证据不宣称接纳;只证明服务端接纳续接链,不证明模型理解结果);`tool.delivery.uncertain` 必带 `deliveryId`/`reason`(回执丢失,不把重试当 exactly-once)。执行终态、结果持久化与选用**复用已有 `tool.execution.*`/`tool.result.*`**,不另造重复身份。能力快照 `tool.capability.recorded` 必带 `basis {provider,wire,model[,endpoint,toolName,runtimeConfigRef,toolDeclarationHash]}`(判定依据留档)与 `verdicts`(非空,逐项 `status ∈ unknown|verified|unsupported`);`unknown 默认不用 native_deferred` 的闸门在读取侧 `DecideAsyncModes`(native_deferred 只认 verified,缺项按 unknown 处理;job_handle 仅明示 unsupported 才禁)。跨行合同(`async.*` 错误码):同 jobId 二次注册、未注册先派发/观测/取消、注册先于调用证据、同 job 重复终态观测、acknowledged/uncertain 先于同 (deliveryId,requestId) 的 prepared、同 (deliveryId,requestId) 重复 prepared/acknowledged、投递引用无工具域账的 Action、resultRef 指不到 `tool.result.persisted`、evidenceRef 指不到账上事件——读取侧 `ValidateAsyncToolSequence` 拒;跨会话五键引用只验格式,不追外账。读取投影(`reader.*`):`FoldJobExecutions`(执行投影,状态单调、终态粘住)、`ProjectProtocolObligations`(协议欠账:哪枚调用还欠模型一份结果,tool 消息落账即配齐、跨 turn 口径)、`FoldDeliveries`(投递投影:prepared→sent(同 requestId 的 `model.request.sent`)→acknowledged;uncertain 可被更晚 acknowledged 解除)。
- **Gateway 常驻域**(常驻总装 V0,合同与 fixture 已验、**生产装配归 V1**;与 tool.job 族同规矩,schemaVersion 纯追加):`gateway.work.bound` 必带 `workId`/`sourceKind`/`sourceId`/`ownerEpoch`(均非空 string;workId/sourceId 全局带域,不收 `op-1` 这类会话局部号跨场判重)、`attempt`(从 1 起,重派计数)、信封 turnId(预留 turn 身份——派发次序里绑定先于 V3 开轮事实提交,恢复器扫 V3 流凭 workId 反查原轮,不再派新轮);可选 `inputRef`(合法引用格式;接纳前正文在 durable input artifact,进 V3 后对照)。`reply.selection.committed` 必带 `selectionId`/`deliveryTarget`/`formatVersion`(均非空 string;原件先落稳、选择事实后提交,resume 后 selectionId 不变——不重新散列投递身份,resume 一次不多送一次)、`ordinal`(从 1 起,同轮多段回复的次序)、`sourceMessageRef`(合法引用,选定的来源 message;不得拿"最后一条 assistant"猜答复)、`artifactRef`(六键,最终正文原件,含 sha256)、信封 turnId;可选 `completedEventRef`(指向 turn 终态事件——选择事实引用对应 V3 终态)。工作认领(`claim(ownerEpoch)`)本身落 Work 领域账,不进 V3;执行/投递状态由领域账与读取投影表达,unknown/needs_review 不硬塞信封 status。
- **提示组合域**(应用Worker接入单 §五 134,生产装配随 app-server 部署档组合路接线):`prompt.composition.applied` 为部署档组合系统提示的可追溯事实,statusless(只记"本场用了哪份组合",不改控制状态)。payload 定案 `{promptSnapshotId(hex64,最终拼装正文全文 SHA-256), agentRef(string,空=未点名走默认人格), segments(数组,每段:{order(从 0 连续,组合次序), refPath(非空,模块相对路径或段名), origin(非空,来源层名), source(磁盘层为 UTF-8 全路径,嵌入/现填段为空串), contentSha256(hex64,该段渲染正文哈希)})}`。开场(thread 建场、组合定型)后提交一次;审计口径:段账逐段有 hash 与来源,业务正文哪段来自哪层可查,业务文本伪装不了宿主权限。v2 场无此行(组合账不回填旧格式)。
- **todo/goal/loop/fork/btw**(§4.55-4.58,后续棒次):独立存档;fork/btw 引入 `targetContext` 作用域,字段留挂点。

### 四.1 异步工具 P1 定案补遗(纯追加;实现于 `src/tools/tool_job_coordinator.*`)

P0 记账留下五处"拟议待 P1 确认",定案如下。schema 载荷合同零改动(P0 已按此钉型),本节只冻结语义:

1. **ownerEpoch 租约细节**:代号格式 `epoch-<n>`,每 jobId 独立、从 1 起单调递增,由单写者(持账的 ToolJobCoordinator)发号。账上体现=每枚 `tool.job.dispatched` 的 `payload.ownerEpoch`,同 jobId 第 N 枚 dispatched 即第 N 代租约;恢复接管=再落一枚 dispatched(epoch 递增),不另设租约文件、无过期时间(租约有效期=该 job 执行投影未收终态期间)。完成信封必带 ownerEpoch:单写者只收与当前租约一致的信封;不一致(旧 worker 迟到/双重恢复)拒收——不落终态、不落观测,协调器计数暴露(`stale_envelopes_rejected`)。终态已落后的第二枚信封同样拒收(单 §6"合法终态只接纳一次";计数 `duplicate_terminal_envelopes_rejected`)。
2. **executionPolicy 字段集**(七键冻结,枚举定案;类型 P0 已钉):`allow_background`(bool,缺省 false);`side_effect_class` ∈ `read_only`(缺省)|`local_write`|`git`|`external`|`irreversible`——read_only 可并发(仍受全局/Session/工具三档上限管),其余档默认串行:resource_keys 逐键互斥,未声明 resource_keys 按 `tool:<logical_name>` 单键串行;`resource_keys`(string[],同键同时至多一个在跑);`retry_policy` ∈ `none`(缺省)|`auto`——P1 协调器只执行 none,auto 落档案不自动重试(同 action 新 attempt 的重试执行归后续批次);`deadline_ms`(缺省 0=无限期)——到点先请求取消(自动 `tool.job.cancel_requested`,reason=`deadline_exceeded`)+置取消旗,不直接判失败,副作用可能仍在跑(单 §6);`resume_policy` ∈ `hold`(缺省)|`requeue_when_registered`——两档对 dispatched/running 无终态一律转 unknown 不盲跑,requeue_when_registered 只额外声明"registered 未派发可重新入队"(P1 协调器默认行为,字段是留档声明);`max_output_bytes`(缺省 1 MiB)——结果原文捕获配额,超限截断,persisted 描述 `capture_complete=false`/`capture_reason=quota`,预览仍按 32 KiB 合同(§4.18)。
3. **inline 不注册 job 的口径**:inline 模式不落任何 `tool.job.*` 事件——执行/终态/结果/配对全走既有 `tool.execution.*`/`tool.result.*`+tool 消息路径(inline 主路现状),协议配对由 tool 消息配齐(读取侧 `ProjectProtocolObligations` 已按"无注册即 inline"投影)。协调器不把 inline 调度进 job 队列、不造影子 job;job 注册与否即模式分界,inline 的等待/并发由 inline 执行策略管。
4. **observed 不要求 attempt**:定案为永不强制。观测者是"宿主对 job 的观测"(job_get/job_wait 巡检、恢复巡检、单写者收到完成信封后的终态落账),不绑定某次工具执行尝试;attempt 可带(观测伴随真实调用尝试时),缺省不带。信封合法性只看 jobId/observedStatus/resultRef 成对。
5. **deliveryId 跨 requestId 重试的条目粒度**:deliveryId 是 resultVersion+目标分支+用途的稳定去重键,跨重试不变;账面条目粒度=(deliveryId,targetRequestId)——每次发送尝试一条 `tool.delivery.prepared`,重试开新 requestId 另立条目(`FoldDeliveries` 已按此折叠,`ValidateAsyncToolSequence` 拒同对二次 prepared)。消费方按 deliveryId 聚合:任一条目 acknowledged 即已送达;全部条目未决=进行中;有 uncertain 且无 acknowledged=uncertain。

P1 宿主侧四接口落地口径(单 §8):start/get/wait/cancel 是宿主侧任务服务(ToolJobCoordinator),不是模型工具——模型可见性归后续批次,不改工具清单。四接口都过授权闸门(jobId 不是访问凭证,工具名与入参走原工具权限/作用域/Hook 的 gate 回调,缺省 fail-closed;派发出队时再复查一次,不因入队时获准就永久放行)。jobId 格式 `job-<六位号>`(账内单调,恢复续号)。start 接单回 `{jobId,status:"queued|awaiting_approval"}`,注册落稳前不派发;get 终态带 resultRef(指向 `tool.result.persisted` 事件)与 ≤32 KiB 有界预览,不自动重跑;wait 有界等待,超时回 pending+状态快照游标,不宣告任务失败;cancel 回取消请求状态(`cancel_requested`|`already_terminal`),不保证已终止。执行前(出队派发时)查授权与取消状态;完成信封由 worker 投递、单写者校验 ownerEpoch 后追加,worker 不直接写 history(单 §5)。

### 四.2 异步工具 P2 运行时接线补遗(纯追加;实现于 `src/agent/async_tool_seam.hpp`、`src/runtime/{async_tool_runtime,provider_tool_contract,result_delivery_planner}.*`、`src/tools/job_tools.*` 与 loop/宿主接线)

P1 五处定案之外,P2 落地的运行时口径(载荷合同零改动,只钉接线语义):

1. **批次闸门 seam**:AgentLoop 的工具批次拆成两遍——先裁决整批(`AdjudicateBatch`:inline 收齐续跑 / job_handle 接单即配 / native_deferred 留欠账)并注册全部 launch,再按声明序执行 inline(wait 在 launch 之后处理,单 §7 次序纪律)。非 inline 调用不走 bridge 内联链(协调器自落调用证据链),批次栅栏与 RunOneTool 都跳过;接单回执块带 host-only 标记 `job_admission`,本轮 v3 结果提交路径(V3ToolResultsCommitted)按它跳过——不为同一枚调用重落第二条链。全批欠账(native 一枚结果都没有)不推空 user 消息;配对纪律 `ToolBatchPairingMatches` 认 `async_call` 位(没标 async 的调用必须同步配对,不许悬空)。
2. **两档派发点**(2026-09-16 宿主指示并入):默认保守档 = 完整 assistant 落账后派发;流式提前档 = SSE 流中单枚 call item 完整(call_id 定型、参数 JSON 收齐——assembler 只在收尾时入 `completed_tool_uses`,半截 delta 天然到不了)即派发,宿主继续消费流。提前档策略合成:仅 side_effect_class=read_only 且未声明 resource_keys(只读无键不抢串行位)且权鉴现查允许;tool 消息留给 `CompleteAdmission` 在声明消息落账后补(链序不倒:assistant 先、接单消息后)。提前档的调用证据锚 = 流式预留的 assistant messageId(interrupted 路也以它成行;进程中途崩溃留恢复缺口,按账面 disposition 收)。重复终帧按 call id 幂等去重,只派发一次;提前派发后流断 → dispatched 无终态 → 恢复 disposition unknown_hold 不盲重跑。
3. **投递规划(P2 面)**:完成通知只入 mailbox;请求边界(拼请求前——冻结输入前的唯一时点)选已提交结果。P2 只投 native_deferred 欠账:配对链走 `ToolActionSession::ReopenAligned`(只补 selected+tool 消息,不执行、不造 pending、不改 attempt),配对正文 = ≤32 KiB 业务预览;job_handle 完成通知入 mailbox 只记账(模型经 job_get/job_wait 自取,notify-vs-continue 调度归 P4)。prepared 在请求账发号落稳后记(每次发送尝试一条,重试新 requestId 另立条目——P1 定案 5);acknowledged 的 evidenceRef 指 `model.response.completed` 事件 id(桥的请求簿留档);证据解析不到/失败/流断落 uncertain。恢复(单 §6"原文已落仓、消息未提交")用同一套配对路:`RestoreFromLedger` 把终态已落、义务未配的 native 欠账重建进 mailbox,补投递不重跑。
4. **能力闸**:ProviderToolContractValidator 纯合成(不接网络)——native_deferred 只在 wire=responses 且调用带 async 标记且外部探针 verified 才 verified;P2 无真探针(生产恒 unknown = fail-closed 降级,归 P3 原生试点)。快照 `tool.capability.recorded` 由运行时在首次裁决懒落一次(basis 带 provider/wire/model/endpoint)。
5. **模型可见面**:job_get/job_wait/job_cancel 三枚工具(`src/tools/job_tools.*`)挂宿主注册表;job_wait 的结果 JSON 里已完成 job 的业务结果(results)排在自身状态(statuses)之前。start 不单设——白名单工具的普通调用由闸门接单即配。
6. **宿主接线**:终端(interactive_session/turn_runner)、one-shot(one_shot + headless_executor 网关路)、AppServer(Detached 面)、子代理(agent_tool 任务域寿命,不承诺跨进程存活)经 `AttachDefaultAsyncToolRuntime`/`AsyncToolRuntime::Create` 接同一套闸门与规划器,不各造;生产缺省零策略(tools 白名单空 = 全 inline,权鉴 fail-closed,不派发),行为与从前一字不差。Workflow agent 节点同款接线;llm 采样节点无工具环(单次 SampleModel 请求,无批次/续接边界),不适用闸门——留账 P3 与原生试点一起评估。


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
| `/usage` 命令(`src/app/commands/usage_commands.cpp`) | **已接 v3**(T06,2026-09-12):`ReadSessionUsage` 分派 v3(`ProjectV3Usage` 吃 assistant usage owner + `WalkSessionTree` 递归子 session);v2 老路保留给旧档 | assistant message 的 `usage`(唯一可累计事实)+ `model.usage.appended`(失败/迟到/更正观察,不参与累计);折算口 `accounting::UsageFromV3Owner` | 已接(离线读面) |
| Telemetry(`src/telemetry/projector_v3.cpp`) | **已接 v3**(T07,2026-09-16):`ProjectV3LedgerFile` 投 v3 账,`gen_ai.usage.*` span 属性与 `lubancode.model.tokens` metric 吃 assistant usage owner(键集与 `UsageFromV3Owner` 同表,`tests/unit/telemetry/test_telemetry_projector_v3.cpp` 对表钉死);缺实报 `coverage=unknown` 不补 0;`model.usage.appended` 只作观察警告 | 同上(assistant owner + appended 观察) | 已接(离线读面) |
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
