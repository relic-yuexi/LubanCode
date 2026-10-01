# LubanCore Managed 身份首批合同

状态：设计草案，尚未实现 Managed 入口，不冻结新 API 名或协议版本。依据 [架构分期](../../todos/LubanCore系统架构设计.todo) 与 [分布式路线图](../../todos/分布式支持路线图_从单机宿主到集群控制面的分期落地计划.todo) D6 身份合同。实现仍须逐批审查，并在远端三平台验收。

用户已定：首批只管新建托管会话。旧本地会话照常用；管理员显式认领、迁入与回滚另开 PR。首批新建 Managed 场仍须支持同 ID 恢复，先核保存归属与当前权限。

## 模式与信任

旧 `Runtime / Session / EventStream` 保留本地可信接口。宿主选择旧接口即走 LocalTrusted；新增 Managed 入口须明确选择，缺可信主体、项目绑定或 Policy 就拒绝，不能回落本地管理员。单人 Managed 由可信宿主明确配置默认租户与管理员。

C++ 宿主、Policy 实现与原生扩展属于可信代码。协议客户端不在这层信任中。客户端提交 tenant、actor 或路径，只能供核对；认证与资源绑定须由可信宿主注入。目录名、Git remote、工作区 marker、attachment 代际、请求 ID 均不授予权限。

## 共用上下文

沿用 D6 三层，不另造 Worker 身份表：

| 层 | 内容与用途 |
| --- | --- |
| AuthenticatedSubject | tenant、user、actor_kind、credential_id；可信宿主验证主体与成员归属 |
| ResourceScope | tenant、project、workspace、session；会话保存固定归属，D6 的 thread 对应这里的 Session |
| ExecutionScope | 固定资源、initiating_subject、policy_revision、execution_id、lease_epoch、能力范围；记录本次执行，尚无租约时不宣称已有围栏 |

`ExecutionContext` 收拢这些范围。每项受理操作保存发起主体；查询、审批和取消另带当前 request_actor，不能改写发起主体。自动任务使用明确服务授权。项目与本地工作区由可信登记表绑定，恢复时核绑定版本；相同 cwd 可开多场，不要求每场另建目录或 worktree。

SDK 与 Worker 只接一份中立 `PolicyProvider`。首批收最小裁决与变更通知合同，后续 SPI 扩充同一接口；不并排造 ManagedAuthorizer、Worker ACL 和另一套 PolicyProvider。首批参考实现须支持显式默认管理员、tenant/project 绑定与可撤销授权，不接数据库、登录界面或 OIDC。

配置按全局 → 租户 → 项目 → Session 收快照。首批只预留租户层，允许为空；下层配置不能抬高上层身份与能力许可，不在这批接配置管理服务。

## 会话与视图寿命

建议新增薄 Managed 入口和绑定当前主体的授权视图。内部共用现有 `Session::Impl → SessionService → SessionExecution / SessionResources`，不复制 Agent、MCP、工具、恢复账或销毁流程。

实际会话拥有者负责停接单、取消、唤醒、join 与一次关闭。临时授权视图析构只释放视图，不关共享会话。视图不继承旧 Session，不导出裸 Runtime、Session、EventStream 或内部服务指针。关闭后保留纯查询快照与授权状态；查询不能借已销毁 Agent，也不能丢掉当前读权检查。

动态正文出口统一返回可表达拒绝的 `Result<T>`。拒绝不能折成空列表。建立视图时已交付的不可变 ID/scope 可保普通值；已知 ID 和已发字节无法追回。

## 归属先于执行

Managed 按租户分存储根。根、项目绑定与模式标记由可信宿主管，不让普通请求任选存储路径。路径键须能防穿越、区分同名项目，并核保存归属；摘要只检损坏，不作认证。

新场先核身份和项目绑定，再保存可核归属，才启动 SDK-owned 执行、MCP、工具或模型请求。宿主可能已构造公开 Backend，SDK 不承诺阻止宿主构造它。

旧本地入口不得恢复 Managed 场；Managed 不得认领无归属旧账。归属缺失、损坏、tenant/project 冲突或恢复绑定不符均明确拒绝，不读完正文再猜，不悄悄新建。保留原 Session ID、单写者与 append-only 历史。

首批只交 API 身份与授权；强隔离未验成前，托管宿主只接可信执行。租户目录不构成 OS 沙箱。Managed 不跨租户共用承载不可信工具的 Worker；Worker 文件、进程、网络隔离另批验收。本合同不承诺拦住可信 C++ 宿主直接读盘或改内存。

## 强制授权口

| 路径 | 检查要求 |
| --- | --- |
| 建场、恢复、取得视图 | 核当前主体、获准根、固定归属及项目绑定；拒绝发生在 SDK-owned 启动前 |
| 列表、计数、操作、历史、审批、扩展描述、结果 | 每次核当前主体和固定资源；过滤后再计数；关闭句柄照查权限 |
| Submit、审批回复、Cancel、Close | 分动作裁决；幂等按 tenant/target/operation/client key 分域并核 payload；资源清理由可信拥有者收拢 |
| 模型请求、排队执行、实际工具派发 | 使用本操作发起主体与能力；有效参数确定、审批等待结束后再核；上层拒绝优先于 YOLO/AcceptForSession |
| WaitResult、EventStream::Next、异步结果查询 | 入场、等待/读取完成、返回或发布前复核；撤权通知唤醒等待，清除尚未交付队列 |

现有 `on_permission_evaluate` 只覆盖 `needs_confirm()` 工具。`read_file` 不走这条审批路；必须另设全工具强制点，不能将审批回调当成身份门。

Policy 调用与宿主回调放在 SDK 状态锁外；异常按拒绝处理。裁决携 revision，实装须写明受理、派发、返回与撤权通知的先后顺序。不能永久缓存许可，也不能承诺外部策略、跨进程发布与 OS 副作用同步原子撤销。已运行工具按真实取消能力收拢，发出取消请求不等于工具已停。

授权与出站投影各守一层。可信 SDK 原件读取不等于允许外发。Worker 保留事件字段过滤、脱敏与冻结投影；不得直接转发 SDK 原始 Event payload。默认 preview；full 仍须 Node 总许可加本场显式开启。读权、缓存全文和 full 开关互不替代。

## 验收门

每批生产代码只走远端原生 CI，本地不 configure、编译或跑原生测试。新头须有三平台安装、移位消费、会话路径与旧本地回归证据。首批至少验：

- 两租户同名项目；互猜 Session、操作、审批、结果和订阅身份均拒绝，列表及计数不泄漏。
- 同 cwd 两场、不同项目各一场；模型、权限、事件、取消、恢复与关闭不串场；撤权不能绕过无审批工具。
- 临时授权视图退出不关共享会话；显式拥有者关闭后，剩余查询照核读权，不留后台悬空引用。
- 等待中撤权、关闭后撤权、异步 query 完成后撤权；尚未交付材料不因旧许可继续外发。
- 旧入口恢复 Managed 拒绝；Managed 打开未认领旧账拒绝；新 Managed 同 ID 恢复成功，错误归属拒绝。
- 缺主体、Policy 异常、归属损坏、YOLO 与 AcceptForSession 均不能取得上层已拒绝权限。

建议拆成中立上下文/Policy、SDK Managed 共用装配、Worker 可信主管接线三批。接口批不能代报全场验收；后三者全部过门，才称身份首批交付。旧账迁入、多人角色界面、OIDC、OS 沙箱、跨节点租约、网络 outbox/ACK 另有前置门。
