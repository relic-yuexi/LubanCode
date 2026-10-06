# SDK Managed 存储监督器与视图

基线 `c66f96c6`。本片开放真实存储生命周期：可信项目登记、授权建场、
取得视图、读取实际身份与按现时权限关闭。尚未交同 ID恢复和执行，未到
Managed 身份首批完整验收线。Submit、Run、历史、结果、事件等待和外部
EventSink 均不开放，不装第二套 Agent 或线程栈。

Project 是可信宿主登记句柄，保存现有 authorization::v1::ProjectBinding、
原 PolicyProvider 与真实 WorkspaceIdentity。登记属于宿主 bootstrap，可以
解析获准 cwd、创建按 tenant/project 摘要分隔的根并登记 workspace。
workspace_key 为空只在这一步取真实值；非空须完全匹配。返回 binding，
供宿主沿原 RevocablePolicy BindProject/Grant 配置权限，SDK 不暗授管理员。
摘要是目录名，不是认证。Project 的 Runtime owner tag 也不是认证 token。

同份 binding/version 不许换 Policy 或 cwd 后冒充原项目。普通请求只拿
已登记 Project 与认证后主体，不任选路径。主体、tenant、project、workspace、
session、bindingVersion 每口对账。同 cwd 可开多场，项目目录不加排他锁。
项目配置由 Runtime 强持到 Shutdown；Project、关闭后 View 全退尽也不撤登记。
补片合同见[项目登记寿命](sdk-managed-project-registration.md)。

OpenManagedSession 先查项目范围 OpenSession，取得真实 session ID；先订阅
完整范围，再查当下权限与失效代次，才 Reserve/Publish/Finish。
Session::Impl::InitializeStorageManaged 消费 c66 真 bundle，沿原 SessionService
开 V3，不调用原执行 Initialize、Start 或 Resources/Agent。实际 publication、
V3首行身份与 run核对后，留一份纯值快照。发表前再查授权、代次与 Shutdown。
首行另记并回核 SDK managed-storage layout/version，不把这间存储场冒充已准入执行场。
通知只使代次失效，不代表 Allow，也不在通知栈调用 Close 或外部 Policy。

成功 Open 只交实际 session_id 回执，证明建场，不授读或关场权。宿主拿
这枚 ID为完整场范围 Grant，再 AcquireManagedView。取 View 查 AcquireView；
ReadIdentity 每次前后查 ReadSession，Close 每次查 CloseSession。拒绝返回
稳定错误，不回空数据，不带异常正文或内部根路径。已经交出的 ID无法追回。
Managed 关场首错进入 Runtime 保留通道前也收成固定码；内部原始回执照留。
Close 等到原串行锁后再核当次代次，才允许停接单或关账；Policy仍在锁外调用。
外部 Policy与文件写入不原子化，候选在晚拒绝时真关闭，保原归属/账残留。

Runtime 的 Managed 监督登记强持现有 Session::Impl，并进入原 weak session
登记与 Shutdown 次序。候选只有在真开场、授权复核和发表检查都过后才注册。
失败或 Shutdown 抢赢，候选在锁外退完整 Service/订阅/Provider。原 Local
Session 的弱登记、开场、销毁与回调退场顺序保留。

View 只持受限 Impl 生命借用，不藏公共 Session，不借裸 Service/Writer。
View 析构只退视图；最后一只 View消失不会关活场。实际 Close仍由原 Impl
串行化，关完从监督强登记移除；退休值在锁外销毁。其余 View保纯身份快照，
关闭后照查当前读权。Runtime Shutdown沿原广播停接、实际收齐、Close、等
pending opening次序处理活场；不靠客户端 View续命。

Policy与 capture析构不在 Runtime/Impl状态锁下运行。公共 Managed授权入口
拒 Policy回调重入；订阅与 provider 最后沿既有 PolicyCallbackScope退场。
失效代次只检本进程发布窗口，不是租约或永久许可。无等待接口，不增授权
复核线程；以后受理、执行与通知等待仍接这只真实监督器。

原授权册加实际公开 SDK验收：真目录/锁/首行、精确 scope与多场隔离，
预 I/O拒绝、Policy故障与撤权、View析构不关场、当前授权关闭、Shutdown
与开场竞态、关闭后读权及回调重入。原 CASE/断言/时限不删不松。
新头须真实安装、能由新宿主仅用 SDK/STL消费。本地只查文本、文档与纯数据；
原生、编译与 HTTP只走远端 CI。
等待 close_mutex 期间通知这一分支目前只核源码；公开面没有受控慢关闭口，
本片不伪造辅助函数调用或时序碰运气，原生覆盖仍待后续真实接点。
