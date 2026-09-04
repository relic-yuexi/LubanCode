# Workspace 量级基线(收官验收实测)

- 日期:2026-09-04;单子:`todos/Workspace收官验收_跨平台攻击量级与E2E.todo` §一第 5 条。
- 复现:`python scripts/workspace_scale_baseline.py <workspace_scale_driver 可执行>`(驱动源码 `tests/manual/workspace_scale_driver.cpp`,构建目标 `workspace_scale_driver`,EXCLUDE_FROM_ALL 须点名)。数字全部来自本机实跑,不拍预算。
- 量什么:`/sessions` 与 resume 选择器的数据源 `QueryWorkspaceSessions`(冷 = 索引重建后首查,热 = 索引命中二查);生产召回路 `BuildTurnContext`(冷 = 新实例首召,热 = 同实例二召);单场 replay(`FoldStreamReplay` 折叠 + `VerifySessionDir` 整场验账);`/memory rebuild` 同一条路的索引重建;峰值内存(Windows PeakWorkingSet / Linux VmHWM)。

## 造数口径(不碰真主目录)

- **session**:先经真 `TrajectorySessionLedger` 写一场模板会话(6 轮、含工具往返/usage/title,68 事件),再按档克隆成 1/1k/10k 场(目录名换合规 session id,`session.json` 的 session_id 同步改写;`main.jsonl` 逐字节相同——索引与折叠的解析成本形状与真账一致,事件内 session_id 不重铸,如实记此口径)。
- **memory**:按生产 frontmatter 形状(schema 3)直写 1/1k/10k 份主题(十个关键词族,正文与生产主题同量级),再走 `RebuildMemoryIndex` 建派生账;召回查询钉 module0 族,三档命中可比。
- 夹具不带 fingerprints 段(量的是检索与装配成本,不掺陈旧判定)。

## Windows(主基线)

- 机器:Windows 11,x64,VS 2022 Release;模板场 68 事件、main.jsonl 约 30 KiB。

| sessions | list 冷 | list 热 |
| ---: | ---: | ---: |
| 1 | 3.1 ms | 5.9 ms |
| 1 000 | 1 092 ms | 67.5 ms |
| 10 000 | 10 674 ms | 664 ms |

| memory 条目 | 造数 | rebuild | 召回冷 | 召回热 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.3 ms | 7.0 ms | 3.6 ms | 1.9 ms |
| 1 000 | 1 048 ms | 199 ms | 49.4 ms | 50.2 ms |
| 10 000 | 10 482 ms | 2 014 ms | 533 ms | 520 ms |

- 单场 replay:折叠 5.0 ms / 整场验账 4.2 ms(68 事件)。
- 峰值内存 128 MiB;workspace 树 613 MiB(10k 场 + 10k 主题)。

## Linux(WSL2 Ubuntu,对照腿)

| sessions | list 冷 | list 热 |
| ---: | ---: | ---: |
| 1 | 0.5 ms | 0.1 ms |
| 1 000 | 459 ms | 14.6 ms |
| 10 000 | 4 165 ms | 146 ms |

| memory 条目 | 造数 | rebuild | 召回冷 | 召回热 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.1 ms | 0.2 ms | 0.1 ms | 0.1 ms |
| 1 000 | 10.1 ms | 64.1 ms | 34.7 ms | 33.0 ms |
| 10 000 | 90.0 ms | 649 ms | 380 ms | 374 ms |

- 单场 replay:折叠 2.3 ms / 验账 2.1 ms;峰值内存 117 MiB。

## 读表要点

- **list 热路径是索引的天下**:10k 场热查 0.66 s(Windows)——索引命中后仍要对指纹没动的场做摘要投影,数量级在百毫秒级;冷查(全量重建索引)10 s 量级,只在索引丢失/首查发生,`/sessions` 常规轮次不走这条路。
- **recall 线性于条目数**:10k 主题一召 ~0.5 s,预算内(max_results=3、8 KiB 注入上限)注入字节恒小——贵在扫描与打分,不在注入。回合热路径每外层用户消息一召,长库用户可感知;embedding/倒排是后续演进项,不在本批。
- **replay 与场次规模无关**(单场折叠):成本跟事件数走,68 事件 5 ms 内;万场 workspace 里 resume 一场的开销不随库存涨。
- 造数与 rebuild 是维护面成本(`/memory rebuild`、索引重建),一次性操作,秒级到十秒级,可接受。

## 配套测试

- 并发原子写与锁的跨平台册:`tests/integration/workspace/test_identity_cross_platform.cpp`(其中记录了 Windows `MoveFileExW` 换名对并发读句柄的语义:ifstream 默认不带 FILE_SHARE_DELETE,读手正握句柄时换名可能回 `atomic.replace_failed`,调用方按码处置;读侧有界重试后恒见整份,绝不半截)。
