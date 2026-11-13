# V3 Writer 原生 Journal 回执

基线：SDK 汇总头 `2c376020`。本笔接内部写者，给下一步 JournalStore 留真实凭证。

## 追加合同

`WriteReceipt` 尾添 `optional<JournalAppendReceipt> journal_append`。V3 发号、校验、哈希、领域状态与内存视图仍归原 Writer。`AppendLineLocked` 只调用一次 `AppendLineDetailed`，先拷出机械值，再沿原成功或失败分支走。

只有机械 `Committed` 才推进 V3 seq/hash 与视图。schema、canonical、closed 和原 `inject_io_failure` 拒绝尚未进 Journal，机械字段留空。首实际追加未确认仍回原 `Rejected/v3writer.io_failed`，置 broken；后续仍回 `IoFailed/v3writer.broken`，不重写。

`first_unconfirmed_journal_append()` 在原 Impl mutex 下复制 Journal 缓存。它只交 owned 值，没有 FILE、Writer 或 Impl 借用。首错不随后续拒绝、Close、Verify 或恢复改口。移入目标跟随 Impl，移后空对象返回空值；已拷回执可在 Writer 退场后查阅。

原 native 结果与受控未确认分栏。实际 fwrite/fflush/file-sync 成功，不会被注入改成自然 OS 错误。PowerLoss 只确认文件同步，不给目录持久化、模型采用或 Hook 效果背书。

## 测试接线

内部 `V3WriterOptions` 尾添 `journal_native_io_probe`。仅显式非空的 Start 走原 `OpenWithNativeIoProbe`；默认仍调用原 `Open` 一次。Probe 沿原 Journal 合同同步观察真实 IO 后边界，不重入、不阻塞、不抛出，也不拿写柄。

Continue 和 ContinueOwnedPrefix 遇到非空 probe，先回 `v3writer.native_probe_resume_unsupported`，不先读卷或开写柄。正常空值恢复沿原 File、前缀与 anchor 验证走。

共用 `WriteReceipt` 的 EventLedger 本笔仍留空机械字段，不能把另一条 producer 冒充已经接线。

## 首回 Close：一处行为修复

现 Close 会反复调用外层 `inject_close_failure`。首次注入报错只把 V3 Impl 标 broken；Journal 已缓存成功。若第二次注入返回空，Close 便误报成功。

本笔在 Impl 内缓存第一次 checked Close 结果。首回仍照旧：标 closed，调用原 Journal Close，再调原外层注入。重复调用直接拷首结果，不再 fclose、不再调用注入。首失败不转成功，首成功也不被后来的注入改口。首回原错误码、native 次序与默认成功行为照旧。关闭后保身份、路径、上下文和 owned 机械首错。

## 六条实际验收

一源 `tests/unit/trajectory_v3/test_v3_journal_receipts.cpp`，六个真实 CASE。每案末尾仅发一枚 `[v3-journal-witness-path]` marker：

1. `committed`：真实 Start；三档实际 message/event 追加，核机械请求与确认、原 V3 id/seq/hash、实际 File 链。
2. `before-io`：真实 schema、closed、原 V3 注入拒绝；机械值为空，原错误与 seq/hash 保留；恢复携 probe 先拒绝。
3. `append-gap`：真实开场后，分别在 body/newline 后注入未确认。首 V3 Rejected、实际 native 成功与首缓存都保留，后续 broken 零追加。
4. `flush-gap`：真实 fflush/file-sync 后注入，核各阶段实际成功、独立未确认和前一档确认；读回不能把 live 首错改成成功。
5. `cache`：owned 首错经重复 Close、Verify 和后续拒绝仍原样；另以首次外层 Close 注入、重复 Close 核同一首错和一次 native fclose，并核成功首结果也不反转。
6. `lifetime`：真实 move、退场后读 owned 副本；另一真实卷经过 Close、JournalFileAnchor 和 ContinueOwnedPrefix，恢复不携旧 live 缓存，不换 SID/run。

门由汇总分支接：新源须进 focused、SDK-only exact source allowlist、两条 ASan selector 和 required。核实际 argv、非空原生断言、六 CASE、完整 LastTest 与六枚 marker。证据来自新头三平台和 ASan CI；本地只读源码、跑文本与 Python 检查。

## 后续公开替换前置

Operations 尚须保首追加 witness，并在主卷放 SessionLock 前完成第一次 checked fclose。活读 `ReduceToolPreviews`、SDK ReadV3Ledger、同前缀恢复 anchor、祖先/子卷、plans/operations-inputs/results 仍须逐路迁移。那几条实路接齐后，才能注册整场 JournalStore。

本笔拥有 Writer 两件、此合同与新原生测试源。JournalWriter、Service/Operations、SDK 接口和 CI 门沿原主人分工收口。
