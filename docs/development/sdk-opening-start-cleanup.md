# SDK 开场线程失败清理合同

2026-10-09。第 2 步前置片，基线 `194f587b`。先补实际故障证据，再接自动
Memory 与后台任务。#336 已验收合入 `8a19edf4`；三平台同源原件及失败源均已
封存，见[阶段进度](sdk-stage-status.md)。自动 Memory 与 Detached 仍未交付。

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

## 本片远端发现的旧预算夹具错误

候选 `b57875b6` 的 Windows 原生专项中，开场清理十二案与五条路径均过；
旧 Owned Job 预算案却把宿主超时写死为 3000 ms。登记仍有 5000 ms 总预算，
真实登记、父账提交和派工耗时后只余 1608 ms，生产侧取三道上限最小值，
这次行为正确，夹具预期错了。

保留六案及登记 5000、模型 1000、宿主 3000/4000 ms 原预算。夹具只在真实
Register 前后、worker 入场与实际 command 入场读单调钟，独立夹住两次生产
读钟。实际超时须落在登记余量、模型及宿主上限交集内；余量够时仍恰等于
1000/3000。没有虚拟钟，没有调大预算，没有人为容差。原 probe、实际超时、
模型一次调用、Post、退场与六路径断言保留，远端另核两枚实际预算观察。
