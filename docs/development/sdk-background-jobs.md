# SDK 后台 Job：首批装配合同

本合同先定后台寿命和关闭规则，尚未发布接口或接通执行。用户已授权自行定细节；首批取保守默认，不改现有 CLI 后台规则。开工前须先收入[真实 Job 来源绑定](sdk-job-operation-binding-v1.md)、[V3 日志回执](v3-journal-witness.md)和线程创建失败清理。

## 公开什么

首批只接 Session 自持的后台 Job：显式启用、显式预算、提交、查状态、读 preview、等待和取消。Job 身份独立于调用它的主 Operation；Session、项目、发起主体和真正采用事实一同持值。不用裸线程 ID、进程 PID 或文件路径替 Job 身份。

配置按 Session 冻结。预算含墙钟时长、输出捕获和并发上限，模型只能收窄。墙钟从真实受理登记时算，排队也扣预算；开跑前重查，已到期就不启动执行。默认关闭后台入口；既有前台命令和子 Agent 不会悄悄转成后台。首批不接递归后台子 Agent。

SDK Job 和独立 Runner Detached job 分两种寿命。首批不让 SDK 内部线程承诺宿主退出后继续运行；Detached 后续交由进程外 Runner 持有。两类以后可接同一份查询值，但 owner、取消和关闭能力必须明确，不能把布尔 `detached` 当交接凭证。

## 谁拥有

Session 执行对象持 Job 登记、预算、取消源和实际执行资源。调用句柄只持 owned 身份与查询票，不能维持第二位执行 owner。Job 捕获本次配置、发起主体和有效权限范围，不借 CLI 门面、临时 ToolRegistry、当前目录或稍后可能换掉的后端。也不捕获父 ToolContext/TLS、临时审批或发布回调，不强持 public Session 句柄；借用实际资源须有明确租约，活到 join 和发布退出后。

主 Operation 和 Job 各记自己的终态。主场收到“已受理”只说明登记采用，不能冒充后台成功。只能从真实 coordinator、已 adopted Job 与实际 writer 绑定；未知追加或发布 gap 冻结本场，不另注册同一项，也不自动重派。

## 取消与关场

- 父 Operation 仍在运行时，取消向它已受理的 Session Job 传播。父 Operation 正常完成不会顺带取消后台；完成后用 Job 取消或 Session.Close。首批不提供父取消后继续跑的隐含例外；以后 Detached 须经过明确交接，再按新 owner 合同解释取消。
- 父 Operation 失败、预算耗尽或进入 Unknown，取消它尚未结束的 Job。Unknown 同时封新副作用；已发生的动作与未确认回执照实保留，取消不能倒称回滚，也不能触发重派。
- `Session.Close` 先封新登记，再取消排队和运行 Job，唤醒等待者，等全部执行与发布借用真退出，最后关 writer、存储租约和资源。不能 detach 线程，也不能用等待超时当作已经停止。
- Job 取消幂等。请求取消与真实 Cancelled 终态分开记；不把已有成功、失败或未知改写成取消。
- Runtime 关闭沿 Session 关闭次序，保存首个实际错误。最后一份 public Session 句柄退出也须走同一条清场路径。

后端和工具须合作响应取消；不合作时 Close 继续等待。界面可停止等候，不能销毁后台仍在借用的对象。执行中调用阻塞 Close、Wait 或跨 owner 同步重入，沿 SDK 现有重入门拒绝。

## 许可与结果

“本场以后同意”绑定当前 Session 与当前 Job，不跨兄弟 Job 或其它 Session。Job 不凭父场曾经通过审批免掉自己的有效权限核查。未来 Managed 入口在真正模型请求、工具执行和结果发布前重查发起主体；尚未接治理的后台能力不能在 Managed 场中开放。

默认只传 preview；Full 仍须 Node 总许可和每场参数同时开启。查询持有完整 Session/Job/Operation 来源；恢复和关闭后查询也不能扫描别场文件。结果、审批和事件订阅各自有界，慢查询不拖住执行 owner。

## 恢复与扩展

恢复先核真实采用事实和发起主体。进程内 Job 不承诺重启续跑；缺少外部 owner 证明时，Queued、Starting、Running、Cancelling 都只给历史投影，不能复活为新执行。无活 owner 时，Wait 明报执行 owner 不可用，不无限等、不造终态、不重派。失去连接、未确认追加和执行失败分开报，不能拿重试查询触发重跑。

后续远端 Worker、Runner 和工作流通过 Job 执行适配口接入，复用身份、状态和回执；传输、放置和存储各归自己的 seam。租约与 fencing 真接通前，不开放跨节点双 owner 执行。

## 验收门

真跑提交到后台执行、父取消、Job 取消、Close 与发布竞争、线程工厂抛错、排队拒绝、失败和未知回执。核同项目双场、异项目双场、兄弟 Job 审批隔离、关闭后历史查询，以及宿主句柄退出时后台无悬空引用。

新宿主消费只用安装后的公开 SDK；CLI 原后台用例继续通过。三平台 focused、全量和相关 ASan 均须新源实跑。源码、合同和纯数据门可本地检查；配置、编译、CTest 与原生执行只跑远端 CI。
