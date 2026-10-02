# 前台子执行组合验收

先写合同，再合代码。本笔把共用 ExecutionOwner、前台本次取消旗与子账终态回执放进同一私有组合头。公开 SDK 子 Agent、异步审批、父 V3 终态观察与采用链另笔交付。

## 谁持有，怎样关场

主场持原 Backend、工具与 MCP；前台孩子持 wrapper、overlay、Agent 与真实子账。取消旗只借到阻塞调用返回；父 invocation 拷值只作 cause，孩子不能借父 operation 拼身份。轮 scope 先撤回调，再退局部桥与取消链；普通 Close 仍按既有合同等真实调用退出。

子桥只尝试一笔终态 append 和真实 Close，缓存整份回执。执行取消、预算失败与成功各记各账；Close 未确认不能改写真实执行结果，也不能靠重跑模型或工具补账。registry 存值副本；换场换 owner，旧孩子不能往新场表里落账。共享 registry 不延长旧 spawn callback 或父 writer 寿命。

## 新增组合册

同项目目录同时开两场，各持 Backend、任务账、取消旗与 registry。第一场本次旗取消，第二场照常收场；真子账须分别记 Cancelled 与 Succeeded，不能串旗、串身份、串回执。失败退场先放测试闸，再等线程。

子场名字与 run 号都归父场。V3 主场不写 `session.json`，现有发号只扫这份旧 manifest；两场主 run 可同号，首个孩子也可同号，不限于同秒。本笔按各父目录、真实子卷、父来源与完整五键核归属。registry 若查到同串局部键，须回本场整份回执；不同串外场键与从未分配键仍须拒绝。公开 SDK 发布子会话身份前，另补跨场寻址合同，不能单凭子 SID 或 run 字符串认全局身份。

另一场把前台本次取消接进子轮，同时让真实 Close 后的边界报告失败。回执仍保 Cancelled、已提交终态五键与 CloseFailed；父工具返回 StopIndeterminate，不冒称完整交接，不重跑工具，重复查询只读缓存。

验这条 V3 生产 spawn 路时，内部 ledger Options 只增一枚默认空的 `subagent_close_fault` 测试接点，拷值递进 BootstrapChild 与真实子 writer；回调在真实 Close 放柄后才报错。不造 fake Finish，不影响父 writer，也不冒称操作系统自然报错。公开 SDK 配置不开放这枚测试接点。

## 远程验收

新组合来源固定两案，进入 SDK focused 与 ASan，实际 case 与路径标记须非零且齐全。原七场终态、五路前台终态标记、十场本次上下文、两场 CLI Stop 与十四场 ToolRuntime 保留。三平台全量、六套安装消费与 focused、Host、Worker、两种 Runner、项目提交与 manifest 册、九份真实依赖图在同一组合头重验。

小 PR 的绿不能替组合头验收。本地只读源码、查 AST、文档和纯数据；configure、编译、CTest 与原生进程只在远程 CI 跑。组合分支留 Draft，不改共享功能分支或 main。
