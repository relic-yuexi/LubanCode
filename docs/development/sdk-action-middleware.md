# SDK main Action 中间件合同

初稿先基私有组合 `eb94b3531921c4e880ed293048cfc50f1befc637` 定合同，字段、固定帽与止损次序随后分别落文档提交。实现候选已接主场真实调用，远端原生验收尚待新头 CI；合同不充作通过证据。
Action 指一枚真实工具调用。仍走现有 Prepare、权限、Started、Execute、Complete，不另造 ActionRun 或执行循环。

## 首批公开面

沿 `SessionOptions.extensions`、每场 `Registration/Instance`、`Next` 和同份冻结计划，增 `PreAction/PostAction`。
两个新挂点只准 `Stage::Default`。现有 PreUser、PostUser、PreRequest 保留；Estimate、Capacity 仍只归 PreRequest。
默认没有 Action 注册，便不接新回调、不改旧输入、不加新执行门禁。续场仍有只读资格探针，须辨出旧卷曾冻结 Action、后来撤掉注册或半份计划的情形。

首批只接真正 main 调用。孩子、后台、嵌套、CLI 脚本或其他宿主不据本笔声称已迁；main 派 `agent` 这枚工具仍属 main Action。
不能凭同名工具、wire id 或共享 dispatcher 槽猜归属。须取当次真实 dispatch、已受理 operation 与 V3 声明；缺必要 owner 就拒新口。

公开 Context 取真实 main session、已受理 operation、当前 turn 与实际声明 action，四项缺一就拒新口；wire id 另列。
PreAction 尚未发行执行 attempt，便明确缺席，不发 execution identity；PostAction 才交实际 Started 所发 session/operation/turn/action/attempt 五键。
孩子无 owned operation，不拿父 operation 配子 action 拼五键。hook invocation/dispatch 身份也不冒充工具 execution 身份。
字段以标准值拥有；取消旗只借本次调用。Context 增 owned `action_id/wire_call_id/tool_name/effective_cwd`；执行身份另用五键标准值 `ExecutionIdentity`，`execution` 为 optional，Pre 空、Post 才取真实 Started 值。

PreAction 交工具名、候选参数与真实声明范围。PostAction 交 effective input、清洗后原始文本、明确结果状态与真实执行身份。
首批不交凭据，不开放后端选择、结果替换、条目过滤或媒体加工；具体 C++ 布局沿标准库公开 ABI，内部 JSON 类型不外泄。
超帽、坏 UTF-8/NUL、坏形状明确失败，不截一段便假称完整候选。

## 新挂点协议与固定帽

`Input.schema_version=1`。Pre JSON 精确为 `{"arguments":object}`，`Next(candidate)` 和 `output_json` 同形，只换参数，不换工具或身份。
Post JSON 精确为 `{"arguments":object,"result":{"text":string,"isError":bool,"outcome":string,"errorCode":string}}`；原始文本已清洗、capture 已成功保存。
outcome/errorCode 取当次真实 ExecutionFinished 的既有字面值，保同一枚 owned 完成事实；不在 SDK wrapper 重算状态，也不凭 isError 推副作用未知。原生 Result 未自报或自报坏词时，原 FinishTrace 的投影规则照留。
首轮 `30d7c1dc` 的 Linux/macOS 安装消费者实跑 21 场，各过 20 场；新 Action 报 `owner_invalid`。原 exclusive trace producer 没填 thread_id，Hub 只给 Runtime envelope 带场号，不能把这枚空字段误认成缺归属。新口须由同 Session 私有接线，在 Started 处冻结真实 invocation 五键；再对同帧 Started/Finished 的非空 execution、原 call/name 与 turn。trace.thread_id 沿旧路可空，有值却不属本场仍拒；不给旧 trace 虚填 SID，也不在 Finished 后重查已终止的 active identity。首轮失败原件保留，新头另验。
Post 不收 output 或 Next candidate；只准 Next() 与追加效果，不准回头改原结果。
Pre 询问效果沿 `AdmissionDecision`，仅收 `{"decision":"ask","reason":string}`；拒绝沿 Denied，不能用 allow 免审批。
Post `ResultSupplement` 仅收 `{"text":string}`。插件不声明来源、owner 或 ref；宿主从冻结 manifest、definition 与实际 dispatch 填来源，记录真正采用回执。
旁观者不能返回 output、effects、Denied 或调用 Next。内部效果矩阵准许不等于宿主已经采用。

|新显式挂点边界|固定 UTF-8 字节帽|
|---|---:|
|Pre arguments JSON、Next 候选、output JSON，分别计|1 MiB|
|Post 原始 text / 整份 JSON|1 MiB / 2 MiB|
|单枚 supplement / 本次全部 supplement|16 KiB / 32 KiB，总数至多 16 枚|
|reason / code|4 KiB / 256 B|
|含新 Action 点的整体冻结扩展计划文件|4 MiB；关闭新点的旧计划保原路|

解析前核帽，累加先防溢出；字符串拒坏 UTF-8/NUL，精确对象拒多余字段。Denied/handler 错误 code 须非空；正常 result.errorCode 可空。
这些只管声明新点的场，不提高原模型 wire 总帽，也不改变旧三点、CLI 或零注册默认。合法 supplement 仍可能装不进真实富 wire，照原不可表示规则止损。
可信 C++ callback 仍协作取消；Lua 的 wall_budget 不充作任意原生回调硬截止。

## 采用与权限

PreAction 只准拒绝、强制询问、改写候选参数；不开放绕权限或 floor 的 allow。
新参数重过真实工具 schema，再过配置规则、ModePolicy、有效 floor 和原审批路。
只有新 Action 回调实际改参，才启原候选/改后 schema 与最终 policy/floor 复核；旧 PreToolUse 改参保原复检，零注册 CLI 只读路径不添新检验。
拒绝或尚未获准时没有 Started，不预发 attempt，不执行工具。
既有 PreToolUse 归并和 PermissionRequest 顺序须写成一条实际采用链；新回调不能跳过旧 deny 或强制 Ask。

同一 Prepare 帧先保旧硬闸，再跑新候选链、旧 PreToolUse 与实际改参复核，随后沿共享 EvaluatePermission 和原审批表。
Deny、forceAsk 跨 Next、output、旧 Allow 与临时授权保持；不能让 PermissionRequest Allow 洗掉强制 Ask。普通 Pass 才沿原自动放行。
新 Action 强制 Ask 即使遇 needs_confirm=false，也须进入同次原审批表；这项分支只随真实新能力开启。
当前 SDK main 并未接 PermissionRequest 回调；CLI 只在真 Ask 时发它。不得写成两处早已同路，有实际旧回调才采用其 Deny/普通 Ask-Allow。

`Next` 只推进中间件候选链，不执行工具。工具只在原 Execute 位执行一次。
沿原单票、同线程、返回后失效约束；保存或跨线程调用不能借旧 callback 继续跑。
改参只改工具 input，工具名、wire id 和真实声明身份不可由扩展换掉。

PostAction 位于原始 FinishTrace 与 capture 落稳之后，只追加带来源文本。
采用 `result.supplement` 一类明确效果，不能套用只归消息挂点的 ContextAppend，也不覆盖 raw capture。
父模型所见正式材料、原始材料与 hook 效果各留真引用；追加真实富文本时沿原材料保全与 wire 预算，不抬原帽。
结果已发生，Post 的失败不能写成工具未运行或自动撤销；不重跑工具来补 hook。
普通 handler 失败不凭空升级副作用未知；真正捕获、效果提交或关闭未确认，仍走原未知止损。
required/Abort 的 Post 失败停止父模型后续，父操作保已知 Failed 与 raw 原件，不改成工具未执行；只有真实保存/sink 未确认才走未知。
这道止损也须挡住同批结果摘要模型。当前 turn 独占一份失败事实；先禁摘要 Backend，再保真实 raw、rewrite、commit，真实副作用未知优先，随后按 Action 原错停成已知 Failed。新 Submit 不继承这份失败，不添全局终态枚举。
真实 middleware 写回执若未确认，须在 dispatch、Pre 同帧采用回执和 Post 追加回执后立刻核账，Started 前与当批摘要前都设闸；它沿原 StopIndeterminate 收口并保账错误，不等到最终 Complete 才发现。普通 callback 抛错仍走上一条已知 Failed，不能借它伪造账失败。

required/Abort 与 optional/KeepOriginal 沿原执行核语义。已消费 Next 的回执照留，不再跑下游。
旁观者不返回效果、不拒绝、不能调 Next；例外必须另立合同，不能从内部效果矩阵推成公开能力。

## 归属、冻结与关场

同一场 `SessionResources` attachment 只持一份扩展实例、dispatcher 和冻结 plan，复用现有装配，不添旁路 callback/pending 表。
逐 Action 参数与身份取当次值，不借 dispatcher 的可变 tool_execution_id 槽传新身份。
普通回调在实际 worker 上跑；并行读的 Pre/Complete 仍在原主线程收口。旁观 helper 真 join，实例自行协调共享状态。
本笔不能替任意宿主 callback 保证硬截止，也不另开等答线程。

Close 先停新接纳、发取消，等 worker、回调与 helper 真退出；再沿原 service Close 封账、清 callback、退 dispatcher/实例与其借用资源。
回调与 Next lease 必须先于所借 Agent、registry、Backend 和 writer 销毁；保存 Context 不保活取消旗或执行对象。
回调内阻塞生命周期入口仍拒绝；关场后的 owned 历史查询照原合同保留。

注册声明、选中来源、定义 hash、顺序和失败策略先冻结，再启工厂与公开执行。
definition_hash 只表宿主所声明实现版本，不认证任意原生函数正文。
只有声明任一新 Action 点的场启用这道严格 opening 闸，校准整体冻结 extension plan 与有效 binding，不只摘出 Action 部分。
已落稳的新 Action 声明也算在内，续场撤掉所有新点仍须先拒；有效 system 上的 Action 绑定会挡缺半计划。零注册旧三点不借本笔收严。
现有 SDK extension Build/恢复对照在 Service 构造之后；新闸须拆出无工厂的计划预检，并在构造前与既有 SessionLock 下复核。
旧卷 Continue、system 转移和任何新写入前，核同份计划、实际 source owner 与有效 binding。
含 Action 的坏计划、漂移、缺半计划必须拒开且旧卷字节不动。未声明新 Action 点时，已冻结的合法旧三点计划仍走原路；旧档无 Action 计划也照旧兼容。
计划读取按已打开对象核普通文件、拒末段链接，实际读至帽加一字节；换成 FIFO/设备须拒，不凭打开前 status 冒称读到了普通文件，不把这道闸称作文件系统沙箱。新计划只有 CommittedDurable 回执才准采用。
无 Action 场仍保现有 opening 顺序。旧三点坏计划可能在 Service/Continue 之后才拒，这笔不修那笔旧污染欠账，不暗改旧默认。
资格探针目前沿原读取器读 V3 旧卷，尚未纳入另笔 SessionRecoveryView 预算；不声称全部 SDK 恢复读取已有限或都走同份 owned 视图。组合时须先接同 ID 实际预算预检，再走本闸。
续场只读持久事实，不自动重跑 handler、工具或未知副作用；恢复规划器不等于已经接好自动执行。

## 兼容线与验收

旧 CLI 的 PreAction/PostAction 别名仍写 PreToolUse/PostToolUse；脚本 payload、归并顺序、确认档和旧 Post 反馈不改。
零注册、旧宿主、孩子与后台保原路；它们未接公开 Action 不冒记“迁完”。不用新增 capability 顺手改变旧取消语义。
首批与原公开子 Agent 的 child-only grant、Memory 隔开、默认关闭和深度 1 合同并存，不扩孩子装配。

远端须验真安装消费者只用公开头、默认零注册、schema/floor 复核、拒绝或 Ask 尚未获准时无 Started、唯一 Execute、raw/Post/formal 采用链。
另验 callback 异常、迟 Next、取消/Close、同项目双场与异项目双场、同 ID 计划漂移先拒且模型/工具零重跑、旧卷字节不动。
保原 CLI alias/scripts、公开三个旧点与实际孩子回归；新来源数量按最终代码实数冻结，不先冒报 native 通过。
新 Action 来源固定 10 场，安装消费者也走同份公开实现的 10 条路径；另核真实权限链、摘要止损、回执止损与锁下 binding 四条内部路径。账故障沿已有 Ledger/V3Writer 注入，接真实 SDK Action adapter、Agent 与大结果摘要；公开 Session 没有 writer 故障入口，不把这项内部验收冒记成公开 Session 全栈故障。
本地只读、静态、纯数据与文档门。configure、编译、CTest、原生程序全交远端 CI；共享分支另行收口。

首轮 `30d7c1dc` 的 ASan 构建还报原生夹具类型错：审批枚举实际定义在 `lubancode::ApprovalMode`，不在 runtime 命名空间。
后续只改这处具名值域引用，权限链、十场及全部断言照留；旧编译失败原件单独封存，新源须重跑远端原生门。
