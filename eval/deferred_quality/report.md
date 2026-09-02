# 动态工具延迟挂载 §12.5 质量对照报告(真机)

- 单子:`todos/动态工具PromptCache守恒与按需调用设计.todo` §12.5 / §11.3 / §十五红线 8 / P4
- 日期:2026-09-03
- 对照档:`disabled` / `proxy_reference` / `legacy_expand`
- 模型:ccmoon 中转 `gpt-5.6-sol`(openai-responses wire,温度 0,reasoning effort high)——端点与钥匙不落本文
- 可执行:`lubancode.exe` 0.26.176(主仓 `D:/lubancode/build/release` 构建),one_shot 管道 `--yes`
- 工具环境:MCP stub `demosuite`(`mcp_stub_server.py`,30 枚假工具,schema 正文约 19KB,延迟声明 token 本金远超 `tool_search_token_floor=1500`,枚数超 `tool_search_threshold=20`,两道闸全开)
- 任务集:`tasks.json` 8 个任务,覆盖 §12.5 十形状中可真机跑的八种;`schema 中途漂移`、`子 Agent 调用`、`compact 后再调`三种不在 one_shot 单进程形态里覆盖(结构侧各有单测册钉死,见 tasks.json 的 shapes_not_covered)
- 判定:全部机判,runner `scripts/deferred_quality_compare.py`,数据源为 trajectory `main.jsonl`(`tool.input.effective` 的 resolved 工具名与复验后参数、`model.usage.recorded` 的 token 账、`model.request.prepared` 的轮数与 cache epoch)
- 复跑:T2 于全量后再复测一轮(三档),两轮结果一致(温度 0 下稳定,非抖动)

## 一、任务结果表

| 任务 | 形状 | disabled | proxy_reference | legacy_expand |
| --- | --- | --- | --- | --- |
| T1_direct_simple | 名字直白、参数简单 | PASS,首发OK | PASS,首发OK | PASS,首发OK |
| T2_confusable_pair | 名字相近易误选 | **FAIL×2**(未调工具) | **PASS×2**,首发OK | **FAIL×2**(未调工具) |
| T3_nested_schema | 嵌套 object/array/enum | PASS,首发OK | PASS,首发OK | PASS,首发OK |
| T4_longname_multi | MCP 长命名+一轮搜多枚 | PASS,首发OK | PASS,首发OK | PASS,首发OK |
| T5_side_effect | 副作用 | PASS,首发OK | PASS,首发OK | PASS,首发OK |
| T6_reuse_after_gap | 搜后隔轮再调 | PASS,首发OK | PASS,首发OK(只搜 1 次,两次调用之间隔一轮) | PASS,首发OK |
| T7_confusable_draft | 名字相近易误选 | PASS,首发OK | PASS,首发OK | PASS,首发OK |
| T8_confusable_algo | 名字相近易误选 | PASS,首发OK | PASS,首发OK | PASS,首发OK |

**汇总(8 任务 + T2 复测 = 9 跑/档)**:

| 指标 | disabled | proxy_reference | legacy_expand |
| --- | --- | --- | --- |
| 任务成功 | 7/9 | **9/9** | 7/9 |
| 参数首发合格(被调用的 required 目标) | 9/9 | **11/11** | 9/9 |
| 误选工具执行 | 0 | 0 | 0 |
| tool_search 次数 | 0 | 10 | 7 |
| 模型请求轮数 | 19 | 41 | 29 |

T2 两档翻车形态:模型一轮纯文本反问("……这样对吗?"),零工具调用——面对副作用型日历工具且"明天"需换算日期,disabled/legacy 档模型选择反问确认;proxy 档模型先 `tool_search` 再直调 `calendar_add_event`(title/duration 首发 OK)。两轮一致,是档位间的稳定行为差异,对 proxy 有利。三档全零误选(`calendar_add_event_series`/`invoice_create`/`file_checksum_sha1` 等诱饵无一被调)。

## 二、成本账(§11.3 口径)

| 栏 | disabled | proxy_reference | legacy_expand |
| --- | --- | --- | --- |
| 完整 input(非缓存+缓存读) | 204,701 | 336,968 | 269,433 |
| 非缓存 input | 39,709 | 124,104 | **215,801** |
| cache read | 164,992 | **212,864** | 53,632 |
| cache creation | unknown(端点不回此字段,表中不冒充) | unknown | unknown |
| output | 1,688 | 4,132 | 2,213 |
| tool search 额外轮次 | 0 | +22 轮(每任务约 +1~2 拍) | +10 轮 |
| 每轮均摊 input(完整/轮数) | 10,774 | **8,219** | 9,291 |
| 逐任务全程墙钟合计 | 121s | 349s | 252s |
| 首 token 延迟 | unknown(轨迹无流式首块时间戳) | unknown | unknown |

读法:

- **legacy 的 cache-hostile 实测可见**:命中后 schema 扩写回顶层 tools + 延迟索引删行,7/8 任务出现 cache epoch 1→2(见下节),cache read 仅 53k,非缓存重付 215k 全场最高。
- **proxy 单任务短会话并不总比 disabled 便宜**:discovery 结果全文 schema 追加在 history 里逐轮重读,加上每任务多 1~2 拍,总读入与 output 都高于 disabled——§11.3"proxy 多走一步,未必总比全量 tools 便宜"在真机成立。但每轮均摊 input 三档最低(8.2k/轮,顶层 tools 恒定小、前缀稳定),会话越长此项优势越大。
- provider cache usage:ccmoon(gpt-5.6-sol)回报 `cached_tokens`,三档 cache 栏均有实测值;`cache_creation` 字段该端点不回,按单子 §三·3.2 记 unknown。

## 三、cache epoch 真机印证

`model.request.prepared` 的 cache_epoch 逐任务集合:

| 档 | 各任务 epoch | 结论 |
| --- | --- | --- |
| disabled | 全部 `[1]` | 全量常驻,恒定 |
| proxy_reference | 全部 `[1]` | **发现与调用不开新 epoch,前缀守恒在真机复现** |
| legacy_expand | 7/8 任务 `[1, 2]`(T2 未调工具除外) | 命中即断 epoch,cache-hostile 实测 |

## 四、P3 native 顺手试(§7.3 能力不符路径证据)

1. `deferred_tool_mode=native_reference` 点名跑在 ccmoon(openai-responses wire)上:启动横幅大声拒——"native_reference 只在 anthropic wire 下可用(defer_loading/服务端工具搜索是 Anthropic Messages 的原生能力);当前 wire 不认这组形状,已回落 legacy_expand",并跟一行 legacy 档 cache-hostile 提示。回落不悄悄换路,符合单子 §四。
2. `tests/manual/native_tool_search_cache_compare.py` 拿真 config 里 zhipu-anthropic(Anthropic 兼容端点,非官方)钥匙跑:两档**均无 4xx**——端点宽容接收 `defer_loading:true` 与 `tool_search_tool_regex_20251119` 声明,但响应 blocks 全是本地 `tool_use`,无任何 server tool use / `tool_reference` 原生块(cache_read 有回帐,cache_write unknown)。即:兼容端不报错不等于实现了原生引用——`ResolveDeferredToolMode` 的两道门(wire=anthropic + 目录显式声明)不按端点宽容度放行,是对的。P3 真机 cache usage 一栏(Anthropic 官方端点)仍留白,无官方钥匙。

## 五、质量门判定

单子 §12.5 门:"若 proxy 的任务成功、参数首发合格或误选显著退步,就先保留 opt-in"。实测:

- 任务成功:proxy 9/9,不低于 disabled 7/9、legacy 7/9——**不退步,反超**(T2 稳定占优);
- 参数首发合格率:proxy 11/11,与两档同为全合格——**不退步**;
- 误选:三档全零——**不退步**;
- 成本侧如实记:短会话总成本 proxy 高于 disabled,legacy 最伤在非缓存重付。

**判定:过门。**按 `docs/reference/tools.md`"切默认与迁移窗"SOP 三步落 `kRecommendedDeferredToolMode = ProxyReference`、空串解析走 auto、文档默认值行同步。回退无需改码:显式写 `legacy_expand` / `disabled` / `proxy_reference` 任一即压过默认。

## 六、留白项

- **Anthropic 官方端点真机 cache usage**(P3 最后一条):无官方钥匙,`native_tool_search_cache_compare.py` 在官方端点的 provider_reported 栏仍留白;本报告第四节 zhipu 兼容端点的跑不能替代。
- 首 token 延迟:轨迹无流式首块时间戳,本批 unknown;以逐任务全程墙钟代观。
- `schema 中途漂移`、`子 Agent 调用`、`compact 后再调` 三形状未在本对照覆盖(单进程 one_shot 形态做不了,结构侧各有单测册)。
- 模型面:仅 `gpt-5.6-sol` 一枚(openai-responses wire)。切默认后其余 wire(chat/gemini/anthropic)的 proxy 质量未单独真机跑——由 P1 四 wire proxy replay 单测与结构合同兜底。
