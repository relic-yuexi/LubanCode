# Managed 会话归属内部前置

状态：内部候选。只交 sidecar 捕获、核对与持锁发布；尚未接 SDK 或 SessionManager。现有资源开场、恢复读正文与 LocalTrusted 路径均未迁。不能据此称 Managed 会话已经交付。

## 值与文件

固定文件名为 `managed-session-ownership.json`。版本 1 只收 `schemaVersion=1`、`mode="Managed"`、`tenantId`、`projectId`、`workspaceKey`、`sessionId`、`bindingVersion` 七键；不收附加键。四枚 ID 均为非空、至多 512 字节的合法 UTF-8，不含 ASCII 控制字节；bindingVersion 为正 uint64。ID 只是保存归属，不认证 actor，也不授予权限。sessionId 必须等于实际 Session 目录末段。

文件至多 8192 字节，采用确定 JSON 字节。捕获同时持原字节与解析值。读面用现有 `FileIoPath`、`RejectReparsePoint` 与 `ReadBoundedRegularFile`；拒链接、Windows 重解析点、非普通文件、坏 UTF-8、坏 schema 与超帽。session_dir 必须为显式绝对路径；去掉非根目录尾分隔符后核最终目录，不能借尾斜线绕链接检查。

无锁 Capture 只给开场资格材料。LockedCapture 另核真实 `SessionLock::holds()`、固定 lock 文件名和实际目录配对。核对旧捕获与锁内新捕获须逐字相等；主账锚不代替这份 metadata 字节。原语没有父目录 handle 相对操作，也没有锁文件 inode 围栏；可信宿主须串行护住目录、锁与 attempt，不能在调用中换父目录或放锁。本笔不承诺恶意目录竞换、OS 沙箱或跨机 fence。

## 核对与发布

LocalTrusted 核对仅接受真正缺件；标记存在、损坏、链接、非普通或读失败均拒。Managed 核对要求完整归属与可信宿主 expected 逐项相等；缺件或错归属明确拒，不能从旧正文猜归属。

Prepare attempt 必须持实际锁。它只保存 owned 目录、expected、原捕获与首发布事实，不留锁、Writer、Policy 或回调借用。新发布只接受真正缺件，且目录中只有 `session.lock`；已有正文或任何别的条目均拒，不能认领旧会话。已有标记只允许 exact same expected 重验；给 `ExistingMatching`，不造一枚写盘回执。它只证明当前 owned 同归属，绝不证明原 publication 耐久通过，也不是 SDK-ready。标记存在而 V3 缺失（含前次未知残留）时，fresh attempt 也只给这份材料；后续 Manager 必须另有真实 durable 动作，不能凭 matching 放行 V3。本批不交那个重新确认口。恢复完整合法旧场只核已有材料，不制造当年回执。

首次 Publish 先在原锁下重捕获、核原字节与目录；新件只调用一次现有 `AtomicWriteFile(ProcessCrashDurability)`，写七字段原 JSON。实际调用发生时才记录请求档。真实 receipt/error 按值保留，outcome 沿原平台定义；只有 `CommittedDurable` 给本次 `Committed`。换名后目录刷盘未确认给 `Unconfirmed`，标记原样保留，不删、不修尾、不读回升级。换名前真实失败保 `NotCommitted`，不冒称已经写出标记。

attempt 缓存首次结果。后续 Publish 原值返回，不再 IO；失锁、读回完整件或另一次尝试成功均不能改这份事实。新 attempt 可核 visible exact 标记，却只能给 `ExistingMatching`，不能把旧未知回执补成耐久成功。析构不写、不删、不补确认。内存异常发生在实际写调用之后，首事实保 `Unconfirmed`；没有平台回执时不编造 OS 错误。

## 接线边界与验收

后续宿主仍须在读正文前调用资格捕获，在 SessionLock 下复核，再决定是否采用；Policy 在宿主锁外裁决。此 helper 不启动模型、工具、MCP、后台 Job，不读取主正文，不注册公开 Managed 入口。

现有 `TrajectoryDirectory::CreateSessionV3` 先拒已有 Session 目录，再建 `artifacts/` 与 `subagents/`；现有 `v3_opening_participant` 到场时，两处子目录已经在场。本 helper 只准缺件且仅有 `session.lock`，不能直接塞进那个回调；先跑 helper 再调用旧 `CreateSessionV3`，也会撞上“目录已存在”。后续 Managed 开场须另拆目录预留与子目录创建：先预留空 Session 目录、持实际锁、确认归属写入耐久，再建其余目录与 V3。现有认领门不放宽，旧目录仍拒；本笔不改 Directory、Manager 或开场时序。

新原生源 `tests/unit/trajectory/test_managed_session_ownership.cpp` 固定六 CASE，真实使用 SessionLock、文件与 AtomicWrite：`actual`、`local`、`recovery`、`lock`、`drift`、`unknown`。各打印一次 `[managed-session-ownership-path]` 标记，且放在硬断言之后。覆盖同归属重验、两租户同 Session ID、缺件与坏件、链接/非普通、失锁/错锁、先捕获后漂移、旧正文拒认领、首次未知留标记与零重复写。

未知案只在测试中调用既有目录刷盘故障口，使用同一实际 platform 链接实例。真实 AtomicWrite 已换名，受控目录刷盘返回失败；另用既有文件刷盘口核换名前失败与临时件清理。两者均不能称自然 OS 故障。生产没有 ForTest 开关，也不借 SDK DLL 另一份实例假称穿透。Windows 链接案直接造 junction，POSIX 造 symlink；原生创建失败须报错，不跳过。远端三平台与 ASan 须收这份真实 source、完整 argv、六 CASE 与六 marker；本地不 configure、编译或跑原生。所有旧生产、来源与预算逐字保留。
