# Updater 活持锁夹具：真实就绪与原观测

基线 `da9f805d820c2c1baccc013248d6fe0dc89c06e9`。本笔只修原生夹具同步并补原件门，不改更新器生产锁，也不改公开 SDK。

旧母线 `2f3d8bc7` 的 macOS 全量跑 702 册，701 过；`integration.updater.updater_lock` 八案七过。活持锁案拿到 `status=3`，对应 `RefusedBrokenLock`。原日志没留当时锁正文，不能断言旧失败只有一个起因。

生产路径先 create-new 占位，再采 start token、写账、flush。文件出现不等于锁账就绪。读到未成形锁账，生产明确保守拒绝，不挪账。本笔保留这条规矩，不重试取锁来掩盖它。

夹具等真实 racer 在生产 `TryAcquire` 返回 `Acquired` 后写下并 flush 的 `acquire` 事件，还须读到本次 racer PID 和完整锁账三字段。沿原 10 秒截止与 25 毫秒查件间隔，不加固定 sleep，不加持锁时长；racer 仍持 4000 毫秒，原退出等待仍为 30000 毫秒。就绪后只调一次真实 `TryAcquire`，保原活拒、同 PID、锁不动及退出后重抢断言。失败也等本次自有进程退出；超原退出截止才收自有树，不留孤儿。

失败诊断只收本次合成夹具：拒码/detail、PID、原句柄完成态与进程活观测、锁与事件有界原字节。锁至多 4096 字节，事件至多 16384 字节，超帽明确记截断；原字节按 hex 输出，不拿替换字符冒充原件。诊断值在 caller 断言作用域存活，不多查别处文件，不打印真实账号或凭据。

八个 `TEST_CASE` 不增不删，成功活拒路径在全部关键断言后留唯一标记 `[updater-lock-ready] alive-holder-after-acquire`。原完整 CLI Test 仍跑。远端三平台独立收原册登记、JUnit、完整 LastTest、精确八案与非零断言，并核唯一标记；失败也留原件。独立门只调用原 source-file 过滤，不添 SDK focused 或 ASan 来源，不改安装消费与链接闭包。

合同先提交，夹具和门随后送审。本地只读、纯数据、AST、文档和 diff；不配置、不编译、不跑原生。新鲜受测 checkout、双亲与 tree 随 CI 原件登记，旧 SDK 绿灯不能顶替这册。
