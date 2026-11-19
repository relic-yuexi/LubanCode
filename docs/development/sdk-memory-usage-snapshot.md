# SDK 用量快照短合同

2026-10-10。第 2 步自动 Memory 前置。#347 已同源验收、实际合入 33a057865224，与验收源 e6e931ad 同树。八份实现输入已按实际合并树核字节。先交本合同，再迁源码；本片仍待自身远端验收，完整自动 Memory 仍欠账。

公开什么：此片先补引擎内部 UsageSnapshot，不添公开 SDK 五项用量 API。事件追加在 StreamEvent 末尾，旧 variant 项次序保住。它只传实际已返回用量、明报位、缓存位和异常，不传正文、工具、终态或响应号。公开五项计数、逐项来源、provider 响应号随后另片接齐，不能拿此片两项入口充完整交付。

谁拥有：公开 Backend::Generate 在真实 SDK 模块返回 reply 后，adapter 先传可观察用量，再裁决取消与非法正文。无 usage、无 reply、错误与两类异常不编用量。UsageSnapshot 表示整份观察，assembler 覆写，不相加；它不收内容，不设 stop_reason。见过快照后，没有任何用量证据的终帧只收尾；明确报零仍覆盖。未见快照时，MessageDone 全沿旧行为。正常回复的快照与终帧不能重复计费；多次请求汇总仍归原层。CLI 和 SDK 借同一 assembler、SampleModel，不另造账。

怎样关场：返回后取消仍归取消，只保住已归还事实，不漏正文、工具或完成。原 Backend 回调和最终析构守卫不动。空场、无回复与异常不造成功终帧，不保证 Backend 或线程已退出。此片不改主 Loop 各尝试耐久回执、V2 协议、V3 持久材料，也不授予候选或 Memory 写权。这些欠账各片接真 owner 和真实文件，不能用私有回调或 EventSink 冒充落盘。

怎么验：SDK 旧六案保留，添返回取消（非零及明确零）、非法 stop reason/UTF-8/工具、缺 usage、无回复错误及两类异常、成功终帧及跨请求汇总五案，共十一案、十一标记。真实 Generate、adapter 和 SampleModel 跨 SDK 共享模块执行；Trace 只转发，不造帧。取消和拒收只见 usage，不见正文或完成。assembler 旧十一案保留，添非终态、终帧和全快照旗标三案，共十四案、三枚新标记。ASan 两道选择器和登记必需名单同时增 unit.api.assembler；既有 179 来源全留，增至 180 来源、1994 CASE。新五案及 assembler 十四案都须实际执行，不能只登记。

三平台完整 CLI、SDK Lua ON/OFF、九套移位安装消费及 ASan 收同源原件。保住 Backend 十九案七标记、Memory 两路十案十标记、Title 十四案及所有旧名单；普通安装不带测试头和测试导出。TSan 按本片实际路径核触发结果，跳过不计通过。禁止本地 CI、configure、构建、原生测试和 HTTP 验收。

八步目标不缩：此片只交请求事实传递，后续补公开五项来源、真实尝试终态与持久 ACK、自动 Memory owner、候选及 Auto 写队列、同 ID 恢复与 CLI 全功能迁入。随后依序做完整 SPI、最小依赖包、身份治理、公开服务、分布式适配、扩展与编排。
