# SDK 模块分配拒绝合同

2026-10-11。公开 Session 分配失败矩阵的控制底座。旧模块归属测试仍保留；这片不凭控制器校准冒称 Session 逐点验收。

公开什么：不添公开 ABI。实际测试 SDK 导出私有 POD 控制口，只装卸模块调用拦截、启停当前线程计数，读分配数、拒绝数与分配种类。不返回 Operation、usage、事件或恢复材料。普通构建与安装头不含控制器。

谁拥有：测试在无 SDK 工作线程时装卸。Windows 只改实际 SDK 的 PE 导入槽；Linux 只改该模块的 ELF 分配导入重定位；macOS 只改该模块的 Mach-O 分配导入槽。每处保存原地址，成功分配仍调用对应原标准 operator new，delete 不改。不能换成 malloc/free，不能拦宿主或另编静态 SDK。控制只计指定线程，关闭后不计。装卸失败须报错，不能跳过、只装部分槽或让控制器返回假成功。

原分配器委托口须实际读取保存的地址，不能让常量传播把间接调用折回已改导入槽的标准符号。测试也须走正常委托，不只试拒绝出口；递归拦截、重复计数或分配种类改变都须失败。

Mach-O 映像定位须接纳 fileoff 为零、filesize 非零而 vmaddr 为零的头段。slide 用实际载入头地址减 vmaddr 求出；不能把零地址当作缺段，也不能换成宿主映像。

怎样关场：控制借用在实际分配调用前建立，异常出口也关闭。装卸只在无借用、无 SDK worker 的场景下执行；已改槽遇失败须恢复。真实成功路径、数组、对齐、nothrow、sized/unsized 及跨模块释放继续跑原 144 条路。额外校准八种原生分配：宿主分配不计，SDK 基线确有一次，首点拒绝恰一次；抛出 bad_alloc 或 nothrow 返回空，退出计数后核结果。

完整矩阵随后在真实 Generate 返回后启计数，逐点扫成功、取消和 Close 回程，再核公开查询、事件、原盘、Backend 退休与同 ID 零重发恢复。不封顶点数，不用控制器造事实。控制底座通过不等于这份矩阵交齐。

回程接线：Backend 完成回复构造后，只在实际 worker 线程预借计数槽，不启分配计数。adapter 收到 Generate 返回值才激活；调用方设回程门，Close 线程在门开之前不计。计数槽只含序号、种类、次数与门状态，不带 Session 结果。worker 退休与显式关闭计数槽都撤借用，尚有借用便拒绝卸拦截。正常 SDK 不添这条测试路。

先用真实公开 Runtime / Session 校准成功、取消、并发 Close 三条完整回程：核非零分配数、零拒绝、Backend 只调用一次并真实析构，核公开五项数字及来源，再关场、同 ID 恢复。校准册交齐后才逐点拒绝，三条回程各按自身基线扫全数；基线本身不算故障矩阵通过。

同源 Windows/Linux/macOS、SDK Lua ON/OFF、移位安装消费与 ASan 证据齐了才交付。保留旧 CASE、144 标记、六条门禁负例和精确名单。只在远端验，不跑本地 CI、configure、构建、原生测试。

Windows 源 a774 的实际原生日志报安装失败，尚未证明分配入口走哪条路。失败诊断须先留安装阶段、收集槽数、八种入口命中位和地址重合标记；再由实际宿主读取八个保存入口所属映像与入口字节。只读已提交、可读页面，不探未映射地址。诊断在无计数借用时执行，不改导入槽、不改标准分配器、不造回程结果。缺入口仍须失败，不能靠诊断行放宽验收；查明实际导入跳板或模块自有实现后再写修复。

## Windows SDK-resident standard allocation bodies

Actual Windows full-host job 114386577931 at source 510b69ea reports all eight
original standard allocation addresses inside the loaded lubancore.dll, distinct
entry bodies, zero matching import slots and forms_mask=0. Scalar/array forwarding
bodies are not missing operators. IAT-only discovery cannot qualify this build.

Keep import interception where all eight imports exist. On Windows only, admit a
second strategy when every original entry belongs to this exact SDK image and
lies in committed executable memory. Use Microsoft's Detours 4.0.1 at immutable
commit e4bfd6b03e50de46b47abfbd1e46b384f0c5f833, private to MSVC testing builds.
Do not patch a shared CRT, host allocator, delete function, or SDK release build.
Require DetourCodeFromPointer to retain each exact entry rather than silently
following an alias into another function. Installation and restoration each use
one transaction for all eight entries, outside workers and active borrows. Any
ownership, attachment or transaction error fails qualification with its actual
stage/error; a partial strategy never passes. Keep separate import-slot and
body-target counts so diagnostics cannot present patched code as import slots.

Each replacement reads its live saved standard-function delegate. Detours updates
that delegate to a trampoline in the same transaction. Delegated array/nothrow
implementations may call another SDK standard operator: a thread-local guard
counts/refuses only the outer call, not the implementation's nested delegation.
Unwind the guard on every exception. Successful allocations and all 144 original
release routes retain the actual standard allocator/free pairing. Restore every
entry before retiring the installer; no live allocation scope permits detach.

The original eight-form baseline/refusal/restoration assertions, host isolation,
three actual Session paths and uncapped every-ordinal matrix remain required.
Pin the downloaded source archive by SHA-256; keep this dependency out of ordinary
SDK builds, exported target requirements and installed public headers. Acceptance
still needs same-source remote three-platform full CLI, SDK Lua ON/OFF, installed
and relocated consumers and ASan. No local configure, build or tests run.
