# 公开 Session 回复返回后的分配失败验收合同

2026-10-11。第 2 步五项用量来源的未齐验收。此合同未交实现，也不把现有 parser 探针记成公开 Session 证明。

公开什么：不添公开故障开关，不换 Backend、Session 或 Usage ABI。测试经真实公开 Runtime / Session 交回回复、取消、查询、Close，再用同 ID 恢复。私有控制口只设拒绝序号、读计数和阶段，不返回 Operation、用量或恢复投影；这些事实须从公开查询、实际事件和原始 V3 账取得。

谁拥有：故障控制归测试构建中的实际 SDK 模块。先核三平台普通分配和释放如何归属；同模块直接分配、数组、对齐、nothrow 及跨模块释放各列实证。不能把测试可执行文件的 global new 当作 Windows DLL 的分配器，也不能另编一份静态 SDK 代替正在调用的模块。正常分配须沿原分配器和对应释放路径，ASan 的分配种类也须相符；不能改用 malloc/free 后只凭非消毒器绿灯放行。

普通 SDK 构建、安装头和最小依赖闭包不带故障设施。测试构建记录实际模块路径、编译来源、链接归属及私有口；安装消费仍跑原默认构建。控制不得侵入宿主、Backend 回复构造或其他 Session。测试若用了三平台不同的拦截办法，分别交实际拦截、拒绝和释放证据，不凭共用控制接口推断覆盖相同。

怎样起场：先在未注入时跑真实一场，Backend 完成回复构造并返回，再进入 SDK 后续分配计数区。回复包含五项不同原数、真实 provider response id 和原始观察材料。计数区须覆盖材料复制、共享引擎收账、结果写入、终态缓存和事件装配，不能只数 parser 或 adapter。控制与诊断自身不分配；一次场次只拒绝一处分配，异常后恢复正常分配，核实际拒绝次数恰为一。计数未知、拦截不到或控制失效须失败，不能标跳过。

逐点扫查：按无故障实际分配册逐点拒绝，不设“只扫前若干点”的上限。每点独立进程或独立完整 Runtime，保留退出码、拒绝序号、阶段、用量观察和查询/恢复结果。基线来源与每点来源须同树、同 SDK 模块类型、同构建档。路径改变时保留实际后续计数与异常收口，不能靠基线序号假称触发了指定故障。成功、取消、Close 等回程分别校准，不拿一条回程的点数抵另一条。

怎样关场：实际 Close 取消并收回 worker、Backend 和借用。失败断言也须先解开 Backend 等待门，再等析构；回调退休、线程收讫、Backend 释放各记实证。不能拿 detach 或取消旗代替退出。迟到回复若已交回数字，数字不是正文采用许可；材料未确认也不能补造 provider 身份、raw 字段或正文。

结果边界：实际拒绝可能发生在落盘前，也可能发生在结果与终态已落盘之后。逐点核公开 Operation 与已落材料是否相符，禁止第二次异常收口覆盖已经确认的结果、重复累计用量或重发模型。结果提交确认不足便明确 Indeterminate 与失败原因，不能写成成功、已确认零用量或 result_persisted=true；某项观察材料缺口不等于整份结果未落盘，须保住原已确认数字及缺口。原始账已存观察但 SDK 结果缺件时，恢复不得补造公开结果；另核原始五项及来源。队列接收、终态可读、文件可见和耐久确认各列其范围。

事件边界：默认事件流在 Close 前消费；Close 会清空队列，事后空队列不能证明发过失败事件。每枚实际终态须核 session / operation / turn、状态、resultPersisted 与原因，并同公开查询和原盘对表。持久失败 ACK、outbox 与事件游标仍归第 6 步；瞬态失败通知不能抵那份验收。

同 ID 恢复：先关原场，卸下原 Backend，再装零调用哨兵恢复。核场号、操作号、终态、确认范围、五项或明确缺口、原材料身份。模型与工具调用计数均为零；缺材料、异常退场和已落终态的故障点都须走此路，不能只恢复无故障基线。

怎么验：现有十四条首错、七条普通扫查、五条坏帧扫查及公开生命周期二十二案保留。已交六条 Close 标记与待验的实际结果文件冲突路径不删。新矩阵须交实际逐点册、非零分配数、拒绝次数、三平台 SDK 模块来源、Lua ON/OFF、普通安装消费和 ASan 原件。原生 CASE 名、负例和旧门禁只增不减。此合同不声称已补普通堆失败矩阵，也不收自动 Memory 或八步交付。禁止本地 CI、configure、构建、原生测试和 HTTP 验收。

终态收口先补 owner。Complete 入口把候选 Operation 移入 Session 自持槽，再装用量与材料。实际 RecordTurnFinal 确认后，记下这份候选及其确认范围；缓存、Job 或事件后续分配拒绝，worker 不再调用第二次 Complete，也不重写结果或终态。已发布查询保留原终态；尚未发布则无分配地移入 Submit 已建查询槽。结果未完整确认仍报 Indeterminate，不能把材料缺口补成成功。后续异常封住新提交和排队执行，Job 仍按原关场路径收回。私有槽不是持久 outbox，也不补造丢失事件或缓存。实际 SDK 模块逐点矩阵仍须验这条路；源码修补、既有回归或伪造回调都不能抵验收。

Close 撤订阅须接走原弱引用册，不能在 closed 已置位后另建强引用数组。数组扩容若失败，会跳过回调、Backend 与事件投递 owner 的收讫，再次 Close 却因 closed 直接返回。原册在锁内无分配移交，锁外逐枚锁住尚存事件流并关闭；不延长已过期订阅寿命，也不在 API 锁下销毁用户回调。逐点矩阵仍须核这条关场路，源码消去分配点不能冒称矩阵已齐。

逐点册实现沿原 allocation-owner CASE 扩展，旧 144 条分配释放路与八种拒绝校准照留。三条回程各先跑自身实际基线，再为 1 到本路基线末点逐点开新 Runtime；每点只拒绝一次。SDK 自记实际阶段与 worker 借用退休，宿主不能写这些证据。拒绝 Close 本身时，恢复正常分配后重试真实 Close，再核 Backend 已析构。回程事件在 Close 前读，实际缺失单列，不能补造；已确认结果的事件装配失败不等于结果未落盘，也不算持久 ACK。

每点从公开 Operation、原 operations.jsonl、SDK 结果原文件、V3 观察和同 ID 恢复对表：最多一条终态、一项模型观察，不重算五项数字，不添来源。完整结果和终态都确认才认 result_persisted；缺件与材料缺口须留明白。原盘已有完整确认结果，查询及恢复须保住原状态、正文和用量，不能用第二次 Failed 盖掉。已交回数字仍须公开保住；原身份材料缺口单独报。每点记实际序号、拒绝阶段、后续分配数、终态/观察/通知数量、持久状态及零重发恢复，不能只记扫查总数。未触发某点、遗漏、重复或半途退出都不算矩阵完成。

原观察材料不只比字段数。公开投影若保留观察，逐项核宿主交来的 provider namespace、原路径、原数、Reported 来源及 operand 引用。观察缺失须明确 incomplete，不能换一套 legacy 路径或猜出的来源冒充原材料；取消和 Close 也照此核。原盘身份及 JSON 观察继续同公开值对表。

远端源 a7749b4f 已交实际基线：Linux 7816 / 6028 / 6029 点，macOS 7230 / 5597 / 5597 点，依次为成功、取消、Close。Windows 与整套同源验收仍未齐。单条 Close 点还须实际等 20 ms 核等待，完整矩阵不能沿用小案 300 秒预算。本案 SDK focused 和原 CLI/ASan 注册各留 3600 秒，其余案仍按原预算；门禁明确核本案预算，不能借放宽全册或少扫点过关。超时仍判失败，不当通过。

远端源 06d7ad9a 的 macOS 成功路实际基线为 7261 点，1 至 23 点已交原件，随后未捕获 bad_alloc 导致 SIGABRT。测试宿主须在下一次实际中止时留固定帧数组的调用栈，并在每点开场前刷新真实路径与序号。诊断由原宿主执行，不能另编 SDK、改异常结果、接管 Operation 或关掉后续分配点；打印后仍交回原 terminate handler，中止照判失败。诊断需要的栈接口先在无拒绝借用时预热，正常测试结束须还回旧 handler。安装拒绝控制前另存实际 CMake SDK 映像原件，供隐藏符号与栈偏移定位；源映像须先同真实分配口核归属，副本不装载、不执行，也不代替原模块参与测试。

## Actual JSON construction termination at the post-return stream start

Remote macOS job 114394531203, source 0c65a60a, records the actual start of
success-path ordinal 24 and then the original host terminate stack. The stack
enters nlohmann 3.12.0's initializer-list basic_json constructor, called by
V3Writer::BeginStreamResponse, through V3EnsureStreamStarted, the real
BackendAdapter, AgentLoop and Session worker. This replaces the earlier
unlocated destructor hypothesis with an actual construction/retirement boundary.
The loaded SDK image is retained as module-image.bin for source/offset inspection.
The native abort remains failure; points 1 through 23 are not a completed sweep.

Repair the real post-return JSON construction and retirement owners. Structured
initializer-list temporaries must not escape cleanup ownership; a noexcept DOM
retirement must not allocate a new traversal stack. Register container cleanup
before mutation, move the cleanup ownership together with transferred DOMs, and
empty children before parents on normal and exceptional exits. Retain arbitrary
admitted nested payloads and the existing canonical wire schema: neither a new
arbitrary nesting cap nor a recursive destructor substitutes for bounded owner
bookkeeping. Borrowed serialization cannot outlive its document owner.

BeginStreamResponse is the first evidenced caller. Audit its actual CommitEvent,
EventLine projection, canonical serialization, subsequent returned-usage,
completion, result/final and notification JSON owners as the uncapped scan reaches
them. Do not fix only the host probe, suppress a destructor refusal, change the
ordinal domain, replace allocation with malloc/free, skip the failing point, or
fabricate committed usage. Native append confirmation and later semantic failure
must retain their existing distinct receipts and same-ID zero-replay recovery.
All three real paths, every calibrated ordinal, eight operator forms, original
144 release routes, five returned numbers and original observations stay required.
Same-source remote full CLI, SDK Lua ON/OFF, installed/relocated consumers and ASan
must accept the repair. No local configure, build, native tests or CI may run.

### Container-owned retirement frames and private dependency ABI

Keep the existing upstream JSON versions (FetchContent 3.11.3 and current vcpkg
3.12.0), with SHA-256-pinned original json.hpp and abi_macros.hpp inputs. Build a
private header overlay that changes only container allocation/retirement and its
ABI namespace. Unsupported or modified source inputs fail configuration closed;
no silent fallback to allocating noexcept teardown is admitted. Do not mutate the
package installation or install private JSON headers as public SDK dependencies.

An object/array allocation reserves its retirement frame in the same original
standard-allocator block, before constructing children. Strings, scalar values,
map nodes and public SDK values retain their existing allocators. Each frame can
link to its parent during destructive retirement; visit children iteratively,
empty a child before releasing it, and never allocate a traversal stack. Ordered
objects retire from the back without shifting/copying const string keys. Preserve
exception-safe allocation/deallocation pairing, alignment, moves and all existing
JSON schema/canonical bytes. No destructor recursion or arbitrary depth cap is
introduced. Give all project internal JSON headers the same private ownership ABI
namespace, including forward declarations; foreign package JSON objects cannot
silently cross the ownership boundary. Installed public headers expose no JSON
objects. Preserve the upstream MIT notice in distributed SDK license material.

The actual SDK qualification port must build and inspect deep mixed object/array
documents, including ordered objects, before the refusal scope; retire them inside
an armed ordinary-allocation scope, require zero allocations, then refuse an
ordinary standard allocation in that same scope to prove the controller stayed
armed. Retain original module ownership, all 144 routes, eight forms and every
real Session ordinal. All verification stays on remote CI and ASan.

### Relocated dependency notice qualification

Actual PR354 d95957e6 macOS SDK-only job 114402671951 compiled the tests-OFF
SDK, then rejected share/lubancore/licenses/nlohmann-json/LICENSE.MIT as a
private development artifact. The earlier no-product configure error belongs
to the required successful negative case; it is not this job's failure.

Qualify only the exact installed dependency notice paths and SHA-256-pinned
original bytes. Require the JSON notice in both component and full relocated
packages; require the immutable Detours notice only for Windows testing builds,
and reject its presence in ordinary/non-Windows packages. Record their actual
hashes separately from the staged search resources. Do not admit an entire
licenses directory, private JSON/Detours headers, sources or extra notices.
Preserve every existing full-install boundary, native roster and refusal point.
Acceptance remains same-source remote CI; run no local gates or native work.
