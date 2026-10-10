# Journal 原生回执前置

本笔基 `edcf8cb8f822fed95d5d4045e00ed64d97cf0a34`，分支 `codex/journal-native-receipts`。先提交这份合同，再写代码。只补 `JournalWriter` 机械追加与关闭回执；不注册公开 JournalStore，不迁 V3、Recorder、Operations caller，不开数据库或第二套 Session。

## 原路与值口

原 `AppendLine` 先写正文，再写换行。正文短写也照样调用换行；只有两次都成功才进 flush。三档实际都先 `fflush`，PowerLoss 随后才调文件 `fsync/FlushFileBuffers`。本笔保持原顺序、调用次数、短路、行字节、成功 line_count 与 broken 纪律。

新增 `AppendLineDetailed` 返回固定大小 owned 值：`RejectedBeforeIO/Committed/Unconfirmed`、请求档、可确认档、BeforeIO 拒因、正文与换行实际字节数，以及 flush/file-sync 各阶段实际返回和原系统错误。broken、已关柄、空行拒绝都零 I/O。首次 append 未确认另缓存；随后重复调用、关闭或读回不得升级它。

旧 `AppendLine` 只调用一次新值口，再投影 Committed；默认无 probe 路不添 I/O、分配、重试或异常。Buffered 实际已经 `fflush`，成功可报告文件层 ProcessCrash 确认；PowerLoss 只确认这只已打开文件，不冒称目录名或新文件名已经断电落稳。文件 flush 失败时保前一阶段已确认档，不把部分正文当已提交整行。

新增 `CloseDetailed` 缓存首次 `fclose` 实际 witness，独立保 `broken_before/after`。重复调用回同份值，零二次 `fclose`。既有 append 未确认不因真实 Close 成功而消失；旧 `Close` 仍投影原 `!broken`，未打开 default writer 的 Close 仍 true。析构只走这条原关闭路，不补成功。

move 转移文件句柄、首次 append/close 回执与 probe，再清空源。源析构零二次关闭。move 赋值先关旧 target，旧 target 关闭结果不得污染随后源 writer 的 broken 或缓存。移动与析构仍 noexcept；回执不持 writer、文件句柄、字符串或借用引用。

## 受控原生边界

原 `Open` 签名与行为不改。内部另提供显式 `OpenWithNativeIoProbe`。probe 只在真实正文、换行、fflush、文件 sync 或 fclose 调用结束后观察固定值；返回 true 才注入未确认。它不代执行 native I/O、不供假返回值、不改 fd、不关换句柄。默认无 probe。

每份阶段值分栏记录实际返回与注入标记。errno 只在真实 stdio/POSIX 失败后立刻取；Windows 码只在真实 `FlushFileBuffers` 失败后立刻取，不能拿陈旧码填成功阶段。注入发生在实际 native 边界，不能称自然 OS failure；真实 native 成功而 probe 报未确认时，保两份事实，不能把 native 返回改成失败。

probe 是可信同步测试口，方法 noexcept，不回调 Writer 或长期阻塞，不开线程。Writer 持 owned probe；每次调用只借到 native 阶段退场。记录与回执固定大小，不攒原文。它不是公开 SDK 开关，不给 provider durable 权限。

## 六路验收

新来源 `tests/unit/trajectory/test_journal_native_receipts.cpp` 固定六 CASE，各在硬断言后印一枚 `[journal-native-receipt-path]`：`committed`、`before-io`、`append-gap`、`flush-gap`、`close`、`move`。根端另接 CMake、SDK focused、两 ASan selector 和精确 argv/marker 门。

六案用真实 writer、真实文件和真实 native 调用，核三档确认/默认 bool 等字节、零 I/O 拒绝、正文与换行边界未确认、fflush 与文件 sync 分层、Close 首回执/重复/析构、move 与旧 target 关闭。受控 seam 只注未确认，不捏 DTO；每案核 actual native 成功/错误与注入各栏，保首份未确认和实际 line_count。

原 Journal/Recorder/V3/SDK 来源、CASE、预算、调用者枚举与断言全留。只交机械 I/O 证据，不把整行 Committed 冒称领域事件提交或模型采用。本地只做纯 Python、文本、AST 和文档检查，不 configure、编译、CTest、运行原生、下载 GitHub 原件、push 或编辑 PR。新 source/tree 的远端原生另验。
