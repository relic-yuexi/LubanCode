# LubanCore 身份原语与可撤销 Policy

状态：首笔实现候选，尚待新头远端 CI。这里只落中立类型、动作裁决、通知与参考 Policy，未接 Managed factory、Session 出口或工具执行门。现有 LocalTrusted 入口不改。完整托管合同见 [Managed 身份设计草案 #260](https://github.com/relic-yuexi/LubanCode/pull/260)；那份设计仍有后续装配、持久归属与恢复工作。

## 可信输入与裁决

公开接口在 `lubancore/authorization.hpp`，命名空间 `authorization::v1`。认证归宿主；`AuthenticatedSubject` 只是认证后注入值，不能拿客户端自报字段当凭据。主体键收 tenant、user、actor_kind、credential_id 四项，默认不授予管理员。

`ResourceScope` 固定 tenant/project/workspace/session。只有 `OpenSession` 可省 session ID，供发号前查项目范围；其余动作与订阅须有完整场范围。空 session 的 OpenSession grant 只匹配那份项目范围，不作会话读权或取消权通配。ID 为非空、至多 512 字节、无 ASCII 控制字符的 opaque 值，不在这里转文件路径或猜租户归属。

`ExecutionContext` 区分当前 request_actor 与持久 initiating_subject。查询、审批、取消等动作查当前 actor；`RequestModel`、`DispatchTool` 须带完整 execution，当前 actor 须等于 initiator。两份主体各核 tenant，execution.resource 须与目标完全相同。allowed_capabilities 只收窄这次执行，不覆盖当前 Policy 拒绝。policy_revision、execution_id、lease_epoch 记录来源；保存 revision 不等于保存许可，lease_epoch 不等于已经实现租约围栏。

调用任意宿主 Policy 时用公开 `Authorize` 边界函数。缺主体、坏范围、未知枚举、跨 tenant、缺 Policy、异常或坏裁决都拒绝。异常原文不入公开错误。裁决带当前 revision 与公开 reason code；本批只按动作裁决，还没有最终工具参数或模型请求参数。

## 显式参考策略

`RevocablePolicy` 是进程内参考实现。宿主先 `BindProject`，再显式 `Grant` 或 `SetTenantAdministrator`。绑定键为 tenant/project/workspace 三元组，同项目可登记多份 workspace，同 workspace 可开多场。

`ProjectBinding.version` 是这份三元组绑定版本，另有全局 Policy revision。context 明带 binding version；grant 记完整主体、固定资源、授权动作与当时绑定版本。未登记 workspace 不能授予权限。管理员也须有合法目标绑定，只在自身 tenant 内生效。

解绑删对应 grants，保留该三元组版本高水位。重绑须用更高版本；旧 context 与旧 grants 都不复活。授权、撤权或绑定实际变化才抬 Policy revision，重复无变化操作不抬号。规则先复制成候选，再一并发布，失败不发布半套规则。

参考策略不接数据库。销毁后重建 provider 不保高水位，不可充当持久项目登记或恢复认证锚点。

## 通知、退订与寿命

订阅不是读权。`SubscribeChanges` 冻结订阅范围；任意 provider 发错范围或 revision=0，都只通知冻结范围/revision=0。零号表示通知无有效版本，接收方须失效并重查，不能拿它当授权或同步证据。

通知没有初始回放，也不与 Authorize 原子绑定。后续 Managed 等待者须先订阅、再核当前权，并在派发或交付前复核。已交付 ID 和正文无法追回；本笔不承诺跨 provider、跨进程或 OS 副作用原子撤销。

参考策略每只订阅串行调用，可合并到最新 revision，不保证每个 revision 都发一遍。回调、宿主 capture 析构、退订等待都在内部状态锁外。回调可读或改 Policy；通知异常收在该回调内，其余订阅照常通知。慢回调可能占住触发它的线程，宿主回调宜只做失效和唤醒。

`PolicySubscription` 由 unique_ptr 持有，析构退订。外部退订禁新回调，并等真实在途调用退出。从任意通知内退订只停新调用，不等自己或别的通知，免得交叉退订互锁；owned callback capture 留到真实调用退出。后来从外部再退订，仍须收齐。任意 provider 传入的关闭操作须幂等且不抛异常，通知侧停用后还会调用它收齐。并发 Unsubscribe 可用，销毁 handle 本身仍须由拥有者同步。

Provider 销毁也按这条规则停所有订阅。订阅可晚于 provider 析构。宿主不能拿内部锁或回调正需的锁去等退订；这条通知合同不替宿主资源锁序。

## 验收范围

新 SDK 册只消费公开头，验拒绝越权、完整主体与多场分域、执行发起者和当前查询者分开、解绑重绑高水位、撤权后旧 revision 不授予许可。真实线程验重入通知串行、通知内停用后外部收齐、两只订阅互退、provider 在途销毁，以及小 capture 析构重入和释放。闸门限时，锁序故障有 watchdog，不把整套远端测试挂住。

本地只查源码、文档与纯数据门，不 configure、编译或运行原生测试。CI 已接三平台安装移位 consumer、focused 和 ASan 必验来源；新头通过前，这份文档不记已验收。它也不证明 Managed Session、Worker 身份门、持久 sidecar 或 OS 沙箱已交付。
