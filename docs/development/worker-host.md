# 本机 Worker 宿主（实验版）

`luban-worker` 是 Node 父进程托管的本机入口。它只消费 `<lubancore/core.hpp>`、
`<lubancore/results.hpp>`、
`LubanCore::Core` 和私有 JSON 编解码，不复制 SessionStack、ToolRuntime 或 AppServer。
本批没有网络监听、Control 服务、网页接线或后台实验执行器，不改本地 CLI 默认行为。

## 构建与进程边界

开启 `LUBANCODE_BUILD_WORKER_HOST=ON` 会同时启用 SDK。构建目标 `luban-worker`；
部署时安装 `LubanCore` 与 `WorkerHost` 两个组件。资源根显式传安装前缀下
`share/lubancore`，数据根与项目 cwd 也须显式传绝对路径。Windows 须能找到安装
`bin` 内共享库。构建和原生进程验收交三平台远端 CI，本地不编译。

父 Node 独占 Worker 的 stdin/stdout。stdin 每行一份 UTF-8 JSON，最多 1 MiB，
必须换行；stdout 每行一份响应，最多 1 MiB，没有启动横幅。超大响应明确报
`worker.response_too_large`，不裁成看似完整的结果。每个请求都须有非空 `id`，它只作
响应关联，不代替业务幂等键。父 Node 串行发送请求并读取响应。原生 SDK stdout/stderr
诊断关闭，错误只回固定码，防止工具载荷或 provider 错误体混进协议和监督日志。
保留的协议文件描述符不向子命令继承。

这条管道不外露给浏览器。网络客户端断开时，Node 保持管道和 Worker，调用
`client.detach` 摘掉事件订阅；已受理操作继续执行。重新 `client.attach` 后，查询
原 operation ID 补状态和完整回答。attachment 含本次 Worker 随机实例号与连接代际；
旧连接、旧 Worker 留下的请求都不能操作新场。

真正的 stdin EOF、响应管道丢失、`worker.shutdown` 表示父监督链结束。宿主先取消
全部活会话，再等 worker 退出并封账。它不把 Node 父链丢失冒充普通网页断线。
本版不承诺物理 stdio 管道重连。进程遭强杀时，新 Worker 通过同一数据根和 V3 ID
恢复；已派发无终态的操作标 `indeterminate`，不重跑副作用。

## 私有协议 v1

请求：`{"id":"1","method":"worker.status","params":{}}`。
响应只有 `{"id":"1","result":{...}}` 或 `{"id":"1","error":{"code":"..."}}`。
SDK 明确失败时，另附 `sdk_code`；不回 provider 原始错误正文。未知字段、类型、方法
和能力均明确拒绝，不能拿这条 wire 代替已有 AppServer wire。

| 方法 | params 与用途 |
| --- | --- |
| `worker.status` | 空对象；版本、初始化/连接状态、能力清单和本次实例号 |
| `worker.initialize` | `data_root`、`resource_root`，可带主管 `result_policy`；同配置重发幂等，异配置冲突 |
| `worker.shutdown` | 空对象；收拢所有会话后结束进程 |
| `client.attach` | `client_id`；返回新的 `attachment`，旧连接随即失效 |
| `client.detach` | 空对象；关闭订阅，保留 Session 与在途操作 |
| `session.open` | `client_session_id`、`cwd`、`model`、`connection`，另见下文 |
| `session.list` | 空对象；本进程持有的会话摘要 |
| `session.get` / `session.close` | `session_id`；查摘要或关场，关场后仍可查已有结果 |
| `operation.submit` | `session_id`、`client_operation_id`、`text`；返回持久受理回执 |
| `operation.get` | `session_id`、`operation_id`，可带 `text_offset`、`text_limit` |
| `operation.cancel` | `session_id`、`operation_id`；只取消目标会话中的操作 |
| `result.query.start` | `session_id`、`operation_id`、`client_query_id`；不带 `identity` 列正式结果，带六段身份则读持久投影，立即返回 `query_id` |
| `result.query.get` | `session_id`、`query_id`；返回 `pending`、`ready` 或带固定错误码的 `failed` |
| `approval.list` | `session_id`；列出待答审批身份与工具名，不含原始工具结果 |
| `approval.get` | `session_id`、`request_id`，可带正文页参数；读取完整审批入参 |
| `approval.resolve` | `session_id`、`request_id`、`decision`，可带 `reason` |
| `events.poll` | `session_id`，可带 `max_events`（1–128）、`timeout_ms`（0–1000） |

除 `worker.*`、`client.attach` 外，请求顶层必须带当前 `attachment`。这只是 Node
私有边界上的代际检查，不是网络身份认证；未来 Control↔Node 仍须独立认证和授权。

`session.open` 的 `connection` 包含 `wire`、`base_url`、`api_key_env`。
wire 接受 `chat_completions`、`anthropic`、`responses`、`gemini`；密钥只引用 Worker
启动环境中的变量名，不接受 `api_key` 原值，不在响应里回显连接配置。可显式指定
连接/空闲/请求超时。助手或审批展示文本中的已解析模型密钥会遮盖，SDK 本地原文不改。
这不等于通用秘密扫描，更不是工具结果出网策略。

可选字段：`system_prompt`、`resume_session_id`、`builtin_tools`、`approval_mode`、
`approval_timeout_ms`、`max_steps_per_turn`、`context_window_tokens`、`tool_result_sync`、
`result_policy_version`。工具名单默认空，
只认 SDK 当前开放的 `read_file`、`write_file`、`edit_file`、前台 `run_command`。
审批默认 `confirm`；也可显式选择 SDK 已有的 `accept_edits`、`dont_ask`、`yolo`。
审批答复为 `accept`、`accept_for_session`、`decline` 或 `cancel`。

同一 `client_session_id`、同一开场参数重发返回原会话，异参数冲突。该映射仅在这只
Worker 活着时保留；跨进程恢复必须使用 Node 保存的真实 V3 `session_id`。
`client_operation_id` 则由 SDK 持久去重：同键同正文返回原操作，异正文冲突。
父 Node 须保存受理回执。一个进程最多持 64 场会话（包括已关闭但保留查询的句柄）；
摘要最多列 128 个已知操作，并带 `operation_ids_complete`，不能把摘要当历史清单。
恢复后仍可按 Node 保存的任意 operation ID 查询 SDK 原账。

多场 Session 可以共用同一现有 cwd。上下文、权限、审批和操作账分开，文件实际共享；
这不是文件系统沙箱，也不强制 worktree。`session.close` 不关闭同目录的其他会话。
本版未接 MCP 配置、Lua/Package、子 Agent、后台 job 或 CPU/GPU 管理；`job.*` 明拒。

## 正文、事件与工具结果

助手终态正文完整可读。`assistant_text` 返回 `content`、`offset`、`next_offset`、
`total_bytes`、`complete`；偏移按 UTF-8 字节计，分页不切断字符。单页默认 32768 字节，
最大 65536 字节，调用方沿 `next_offset` 读到 `complete=true`。这不是固定首段 preview。
审批 `input_json` 用相同页形状，读全后再解析和展示；request ID 绑定原 SDK 审批。

事件只含序号、kind、会话/操作/轮次身份及可选审批 request ID。`Event.text` 和
`payload_json` 从不直接串行化。事件是有界、临时拉流，不承诺持久游标或断线补发；
队列溢出明确报 `sdk.events.overflow`，重连后应重查操作和待答审批。

工具结果只按已完成 operation 拉取。`operation_completed` 事件通知状态，不携正文；
主管随后调用 `result.query.start`，用 `result.query.get` 取结果。执行中的 operation
明确报 `sdk.result.not_ready`；旧轮完成后，新轮运行、关场或同 ID 恢复，仍可查旧轮。
Worker 不解析 V3 账或本机工具原件，SDK 查询持久材料，公开 `ResultProjector` 复用
`remote/result_sync` 内核。模型上下文仍按 SDK 本地材料装配，不拿出网 preview 替正文。

默认只列 `selected` 且 `result_id` 以 `res-` 开头的正式结果。SDK 可信本地接口还可列
`capture-*` 等具体版本，Worker IPC 不开放这些版本。查询身份为 `session_id`、
`operation_id`、`turn_id`、`tool_call_id`、`persisted_event_id`、`result_id` 六段，必须
逐项匹配本场本轮正式集合。这里 `tool_call_id` 是 V3 action ID，不是 provider 原调用号。
列表不回工具入参、原路径、媒体载荷或自由工具名。未曾展示的版本也不能借 query 另读。

主管在 `worker.initialize.result_policy` 配置节点上限：

```json
{"allow_full_tool_results":false,"preview_max_bytes":4096,"version":"v1","secret_envs":[]}
```

这份配置只来自可信父进程；不冒称 Node 网络鉴权已交付。省略时不准 full，preview
4096 字节，节点版本 `v1`。preview 预算为 1–65536 字节；`secret_envs` 最多 64 条
启动环境引用，解析后总计最多 65536 字节。每场还把该场模型密钥纳入秘密集合，先
脱敏，再截 UTF-8 前缀。IPC 不接收秘密原值，投影记录也不存解析后密钥。

新场 `tool_result_sync` 默认 `preview`，`result_policy_version` 默认 1。full 必须同时
满足节点许可和该场显式 `tool_result_sync="full"`；未获准就报
`worker.full_result_sync_disabled`，不降级开场。SDK 冻结每场模式与版本；Worker 另存
节点设置与秘密集合共同摘要作为主管绑定。恢复省略场参数便保留原值，显式参数须与原场
一致。节点许可、预算、版本或解析后秘密集合改变，恢复明确报策略冲突。指纹只留
本地，用来检出配置变化，不作鉴权，不出 IPC。旧 Worker 场缺主管 manifest 时明确
报 `worker.result_policy_missing`，不替未知旧场猜模式。

每份六段身份只保存一份已脱敏冻结投影，放在 `data_root/worker-result-policies`。
记录与登记表原子落盘；重查、重启复核实际 SDK 材料、身份、策略与摘要，复投旧前缀，
不重新切片。登记后丢件、坏件或原材料变化便报错。只有记录落稳才返回成功。它不是
持久 outbox，不承诺 ACK 或断线补发。文件锁不阻塞等待；同目录另一主管占锁时明确
报 `worker.result_storage_busy`，不并发改同份登记表。

投影正文只放顶层 `text`，stdout/stderr/report 共享一份总预算；combined 存在时作为
聚合正文，其余通道只回固定元信息。当前 `run_command` 把 stdout 与 stderr 收进同一
根管道，正式结果只存 `combined`，不承诺两份分流原件。binary、raw_payload 不回正文；缺件、坏件、
capture 不完整和超大材料均明确标状态或拒绝，不能伪装完整 full。full 正文上限
1 MiB，主管读取本地文本另设 8 MiB 总帽，不拿 IPC 帧帽代替原件读取预算。
最终闭合 JSON 连外层响应也须落在 1 MiB 帧内；转义使帧超限时整份拒绝，
不裁 full、不分页。query 不接 `mode`、`offset`、`limit`、`tail`、cwd 或路径。

查询跑在一条宿主自有线程，最多 8 项等待，避免读盘、脱敏占住取消和 health 主路。
排队项只持公开 Session 句柄与纯主管数据。停机先拒查询、取消全部会话，再收齐真实
查询线程。`client_query_id` 只在本进程去重；最多保留 64 份查询回执，旧终态回执
可淘汰，在途项不淘汰。跨重启重试靠六段身份和持久冻结记录，不靠 query ID。

## 验收

`tests/integration/worker_host/test_worker_process.py` 启动真 Worker 子进程与本地假模型
HTTP 端点，覆盖健康/能力、同目录两场、持久幂等、审批与取消路由、detach/reattach、
旧连接拒绝、父 EOF 收拢、强杀同 ID 恢复、工具载荷不透传、完整中文/emoji 长文分页；
另验真实 read_file 的受限 preview、节点与场双门 full、重启复投、主管坏件/缺件拒绝、
跨场与六段身份拒绝、运行中新轮保留旧结果及 full 超帧拒绝。第十八场
`result_combined_stream_preview` 跑真实命令，分别向 stdout 与 stderr 写标记；节点许可
full，一场仍用默认 preview，另一场显式选 full。两场各存完整、已核验的 combined 原件。
full 须包含两枚标记，preview 须取 12 字节预算内完整 UTF-8 前缀，与全文前缀逐字核对；
通道元信息不得旁路吐出 stderr。
CI 将安装目录搬离源码和构建目录，再跑这份测试；报告逐场登记，缺场或跳过不得算过。
