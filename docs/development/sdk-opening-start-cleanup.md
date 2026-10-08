# SDK 开场线程失败清理合同

2026-10-09。第 2 步前置片，基线 `194f587b`。先补实际故障证据，再接自动
Memory 与后台任务。此片尚未验收，不宣布自动 Memory 或 Detached 已成。

## 公开什么

保留 Runtime、OpenSession、Close、Shutdown 公共调用口。开场线程启动失败
回 `sdk.session.open_failed`；异常不能留下假成功、悬 running 或 Busy。
标准与非标准异常都须收退场，未知异常只给固定诊断。

故障注入归私有测试缝，仅 BUILD_TESTING 构建启用；头文件不安装，普通 SDK
没有故障入口或可变故障槽。测试槽归实际 SDK DLL，逐调用线程隔离，换槽与
捕获析构不持 Runtime 登记锁。不能让测试程序改另一份 inline/thread_local 副本。

## 谁拥有

Runtime 的 Opening 计数持有开场准入，弱登记不延长失手执行寿命。Session
持 execution、Backend、工具捕获、provider、实际 SessionLock、writer 和 worker。
当前标准异常已沿退栈与 Close 清理；本片补证据与缺口，不另造并行 owner。

故障发生在真实 std::thread 创建点之前。未起 worker 便没有模型调用；不能
用另一个模拟 Session 或故意坏配置替代已走完 Initialize 的启动失败。
同 ID 再开仍须经过真实锁与 writer，不跳锁、不补造主轮终态。

## 怎样关场

先停接与取消，再沿实际 Close 释放工具、Backend、provider、锁和 writer。
资源与捕获析构完成后，Opening 才归零。Shutdown 须等失手开场真正收尾，
不能只看弱登记过期或线程柄为空。普通成功开场仍用原 worker/Pump，不走旁路。

## 远端验收

在既有 `test_lubancore_lifecycle.cpp` 中实际跑标准启动异常、非标准启动异常、
多次故障后成功、同 ID 再开、关场后目录改名，以及 Shutdown 与失手清理交错。
每案查模型调用数、捕获与 Backend 析构、实际锁/writer 释放和线程回收。
注入计数须证明命中 SDK DLL；另跑不同调用线程的注入隔离和普通构建无入口。

复用既有生命周期专项登记，原 CASE 与预算保留。Windows/Linux/macOS 须有
同源独立 SDK、Lua ON/OFF、安装消费、全量 CLI 与必要 lifetime 证据才算交付。
本地不跑 CI、configure、build 或原生/HTTP。实际失败原件留存，不能放宽门。
