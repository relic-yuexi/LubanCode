# 线程 capture 收尾：夹具明确交接

#291 源 `a8f1db72` 的远程 macOS 全量实际 704 项、703 通过。`unit.agent.subagent_thread_lifetime` 在第六场中止；日志记录 `startup gate timed out` 与 SIGABRT，只有 6 场、5 通过、85 条断言通过，未跑完原十场。完整 [CI37093088267](https://github.com/relic-yuexi/LubanCode/actions/runs/37093088267/job/111118490001) 原件 SHA256 为 `dc3c34cd0647bec545b92889848bc721478b1f3eb10619118cd7b9c4bbb44611`。旧日志没有线程 ID，不能据此断言超时发生在哪条线程，也不能把另一源头绿灯搬来盖这次失败。

原夹具把 cleanup 的强引用同时留在主线程和 worker capture。worker 只设退出回执便返回。若它先退引用，主线程随后的 `cleanup.reset()` 就成最后 owner，在主线程触发析构、等 retirement 闸；主线程尚未走到自己的放闸语句。析构里的五秒等待抛错，又遇隐式 noexcept，便会中止进程。这条交错由源码可证；它是不是本次历史交错，旧原件没有足够字段确认。

本笔只收夹具 owner。复用既有 StartGate，添 worker 结束许可：worker 先等许可；StartThread 已返回，主线程已退 cleanup 强引用，才放行 worker 结束。之后仍在 worker capture 析构里进原 retirement 闸，原 Reap/Close 交错照验。许可闸与 retirement 闸都由栈上守卫释放；断言失败先放闸，再等待 futures 和协调器退场。不能靠一行 move 或睡眠碰运气。

生产线程表、启动工厂、Reap、Close、join/detach 均不改。原十场、已有每条断言、五秒等待与十五秒 watchdog 全留。不增 native CASE，不拉长时限，不把失败改成跳过，也不拿这笔称 Detached 借用寿命已经收清。

本地只做源码差异、文档和静态检查，不配置、编译或运行原生测试。新源独立走三平台全量及既有必需 ASan 线程册；必须实际跑完原十场、断言非零、没有跳过。#291 Action 成功分项与旧线程中止原件分开保存，后续合流仍须取新组合自身证据。
