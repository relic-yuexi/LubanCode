# SDK Memory 写计划：原子写失败诊断

本笔基私有组合 `dfb029c0e57b1d39f4beec0c2a0184de799a7e6f`，先定合同，再补消息。
`2f3d8bc7` Windows combined 原日志 111075205011，4607–4616 行，四场隔离开场报 `sdk.memory_write.plan_write_failed`；新 ResultStore 原 17 案和安装消费者 20 案已过。
原日志 SHA256 `db427092bee50969096346ee452d072ffb0f187d961f95695c6ddf6ee9ede9c8`。
这份消息丢了 AtomicWriteError，未说明临时件、换名或耐久阶段；不据此认定长路径或短拒。

## 唯一改点

仅补 `SessionMemoryWrite::Open` 发布新计划时的错误正文。`sdk.memory_write.plan_write_failed` 外码、失败返回和原开场状态照留。
AtomicWriteFile 失败时，正文带实际 `atomicCode`、`failureKind`、`outcome` 和底层 `message`；成功回执若不是 `CommittedDurable`，仍拒开，正文带实际 outcome。
不输出计划正文、凭据或环境变量；底层消息只沿原平台文件错误，可能含这次本地写盘路径。
不开重试，不删 target，不将已提交但耐久未确认改称未提交，不改全局 AtomicWrite、路径转换或持久档位。

## 验收与界限

复用原四场隔离场景和公开 SDK Error 消息，远端三平台消费者、focused、全量与必需 ASan 重走鲜头；原断言、预算、册数、同场恢复和 CLI 路径都保留。
这笔只补诊断，不能凭代码静读宣布旧故障已修。旧 `2f3d8bc7` 真红原件另封；下一轮若仍红，按新消息中的真实阶段再定窄修，不随机重跑旧头。
本地只查文档和静态差异；不 configure、不编译、不 CTest、不运行原生夹具。
