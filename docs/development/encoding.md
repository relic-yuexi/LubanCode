# UTF-8 编码关口(字节问题防反复)

[文档首页](../README.md) · [开发手册](README.md) · [安全模型](security.md) · [测试指南](testing.md)

lubancode 把"不可信文本进对话历史、进 JSON 序列化"当作一道必须守住的关口。本文核实自 `src/platform/text_encoding.hpp`、`src/platform/json_safe.hpp`、`src/api/types.hpp` 与相关调用点,写给改代码的人看——这个坑修过很多轮,写代码时对照文末[检查清单](#新增代码检查清单),别让它再复发。

## 一句话问题

nlohmann::json 的 `dump()` 遇到树里混着的**非法 UTF-8 字符串**会抛 `type_error.316`;异常一旦穿透到顶层,整场会话被掐死。症状通常长这样:

```
[chat] 请求体序列化失败(非法 UTF-8): [json.exception.type_error.316] invalid UTF-8 byte at index 792: 0x0A; 字段: messages[1].content -> 已按 U+FFFD 清洗后发出
```

注意报错里的 `0x0A` 往往**不是**坏字节本身:它是合法换行符,只是前面紧挨着一个被截断的多字节序列(首字节悬空、续字节缺失),整个串因此在它附近被判非法。`index 792` 告诉你刀口落在大约 792 字节处——典型的"字符串被按字节砍断,砍进了汉字/emoji 的腰上"。

## 为什么这个坑反复踩

1. **JSON 的 parse 不校验 UTF-8,只有 dump 会炸。** 读旧会话档、读入站 JSON、拼 tool_use 入参时,坏字节能"合法"地穿过 parse 进内存,到序列化才爆。
2. **坏串一旦进内存历史就赖着不走。** 早期兜底只清洗"发往网络的那份拷贝",内存里的坏串原样保留——只要那条消息还在窗口内,每轮请求都重打一遍日志。
3. **来源太多。** 用户输入(管道/重定向)、会话文件恢复、模型流(服务端或中转的问题)、子代理投递、app-server 入站……每个口子漏一次,就是一个新的 316。

## 坏串来源清单

| 来源 | 路径 | 为什么会有坏字节 |
| --- | --- | --- |
| 管道/重定向 stdin | `cli/console_input.cpp` 的 `ReadLine` 非交互分支 | `std::getline` 不做编码处理,GBK/ANSI 原始字节直接进 |
| 旧会话文件恢复 | `agent/session_store.cpp` 的 `ParseSessionFile` / `BlockFromJson` | JSONL 是 `nlohmann::json::parse` 读的,不校验;崩溃截断行、老版本写的档都可能带坏串 |
| 模型流输出 | `api/assembler.cpp` 收块 | 服务端/中转把多字节序列劈在 delta 边界,或干脆吐坏字节 |
| compact / microcompact 摘要 | `agent/compact.cpp`、`agent/microcompact.cpp` | 摘要文本来自模型输出,同上一行 |
| 工具结果 / 工具输出 | `agent/loop.cpp`、`tools/run_command.cpp` 等 | PowerShell 5.1 解析期错误走系统 ANSI 代码页(国内是 GBK),原生程序绕开控制台编码直接写 |
| app-server 入站 | `app_server/server.cpp` | JSON parse 不校验 UTF-8 |
| 子代理 inbox 投递 | `tools/agent_tool.cpp` | 跨会话传话/外部投递不清洗 |

## 防线分布(按层)

### 清洗函数库(`src/platform/text_encoding.hpp`)

| 函数 | 用途 | 什么时候用 |
| --- | --- | --- |
| `IsValidUtf8` | 严格校验一段字节是否合法 UTF-8(拒绝过长编码、代理项、越界码点、截断序列) | 判断要不要洗、测试断言 |
| `SanitizeUtf8` | ACP 优先:非法时先整段按系统 ANSI 代码页重解释,转完合法就用它 | `run_command` 那类"输出整段都是本机编码"的老场景 |
| `SanitizeExternalText` | 成分判定:坏字节零星混在合法多字节之间只逐段换 U+FFFD,合法片段保留;整段是 ACP 才试转 | **外来文本的公共边界一律用这个** |
| `Utf8DeltaGate` | 流式增量闸门:扣住疑似被截断的尾巴,拼齐再放行;真坏字节立即替换 | SSE 流式 delta 的显示路径 |
| `Utf8PrefixBoundary` / `Utf8SuffixBoundary` | 按字节长度截 UTF-8 的两把安全尺,截短/截取先过它们 | 一切 `resize`/`substr` 裸砍多字节文本的地方 |

### JSON 序列化兜底(`src/platform/json_safe.hpp`)

- `DumpJsonSanitized`:正常树直接 dump;有坏串先逐串过 `SanitizeExternalText` 再 dump。**会话 JSONL、录制件这些"落盘后还要被 /resume 重新读"的地方用它**——宁可落盘内容被替换字符洗过,也不能落一行解不开的坏行。
- `FindInvalidUtf8Field`:报坏串长在树的哪个字段上(诊断用,只报路径不报内容)。
- `DescribeDumpFailure`:wire client 的窄 catch 拿它包成人话错误,会话活着、错误看得见。

### 主动闸(治本,这次新加的)

- **请求装配闸**:`agent/loop.cpp` 赋 `request.messages` 前,对每条消息过一遍 `api::SanitizeMessage`(定义在 `src/api/types.hpp` / `types.cpp`,递归清洗 TextBlock / ToolResultBlock / ThinkingBlock / ToolUseBlock.input 的 JSON 树)。合法内容零成本原样返回;坏串到这里就被洗掉,四个 wire client(chat / responses / anthropic / gemini)的 `dump()` 兜底变成"几乎不可达"。
- **会话加载清洗**:`agent/session_store.cpp` 的 `BlockFromJson` 各字段加载即清洗——根治旧会话文件这条主源。
- **模型输出进历史前清洗**:`api/assembler.cpp` 收块、`agent/compact.cpp` 摘要文本。
- **输入边界**:管道 stdin(`SanitizeUtf8`,ACP 试转可救 GBK)、app-server 入站文本、子代理 inbox 投递。

### 已有的源头拦截(历史积累,别拆)

- 工具结果三处:`agent/loop.cpp` 的 dispatch_done 出口、PostToolUse 前、SanitizeUtf8 断言式兜底。
- 工具输出类工具:`tools/run_command.cpp`、`tools/web_fetch.cpp`、`tools/background_tasks.cpp`、`tools/shell_info.cpp`、`runtime/plugin_process.cpp`、`runtime/plugin_tool.cpp`。
- 流式显示:Utf8DeltaGate(`agent/loop.cpp`)。
- 落盘边界:session_store 各序列化函数、tool_trace、outbox、schema 全走 `DumpJsonSanitized`——**当前版本写的会话文件是干净的**,脏的只会是旧文件/截断文件。
- 截断对齐:context.cpp、context_events.cpp、artifact_store.cpp。
- read_file 明确拒绝非法 UTF-8;外部编辑器草稿回读校验。

## 新增代码检查清单

改到"文本要进对话历史 / 要序列化 / 要落盘 / 要发给模型"的代码时,逐条对:

- [ ] 外来文本(工具输出、hook 输出、MCP 响应、管道输入、入站 JSON)进历史前,过 `SanitizeExternalText`;整段本机编码的老场景用 `SanitizeUtf8`。
- [ ] 截短/截取多字节文本,用 `Utf8PrefixBoundary` / `Utf8SuffixBoundary`,不许 `resize` / `substr` 裸砍。
- [ ] 流式 delta 拼进历史前,要么过 `Utf8DeltaGate`,要么收块时清洗;两件事至少做一件。
- [ ] 落盘 JSON 用 `DumpJsonSanitized`,不裸 `dump()`——除非你确定这棵树永远只有你亲手写进去的合法字符串。
- [ ] 从 JSON parse 出来的字符串(会话档、入站请求)默认当"可能带坏串"处理,读进来就洗。
- [ ] 写进 `api::Message` 的字符串,统一相信请求装配闸会兜底;但**别依赖兜底**——兜底是最后一道,源头各管一段才是正道。
- [ ] 新增"不可信文本"入口时,先在文末来源清单里补一行,再写清洗。

## 修复时间线(这个坑反复出现的证据)

| 提交 | 修了什么 |
| --- | --- |
| `e671b2e` | filesystem::path 窄口全仓清剿,一律走 u8 通道(GBK 机器 1113 根因) |
| `2cc347d` | platform 宽窄转换不许抛,坏字符替换 U+FFFD |
| `160f78a` | SSE 劈半多字节过 Utf8DeltaGate,拼齐再放行(wire 边界) |
| `7ead05e` | 交互会话异常分级兜底,回合收口不退进程 |
| `8f5f43e` | 宽窄转换异常四管齐下:path 窄口、delta 闸门、转换不抛、顶层兜底分级 |
| `c4a3dc6` | 截短/预览的刀口对齐码点边界,请求 dump 坏串清洗后照发(316 砖死会话) |
| `a0f25bd` / `7acf5c9` / `263a759` | 输出帽刀口对齐 UTF-8 边界;goal 五处裸 dump 换 DumpJsonSanitized(316 双层修复) |
| `ff6de30` | 工具追踪截断非法 UTF-8 |
| `5b796a1` / `9d00606` | preserve UTF-8 artifact boundaries;修复多处 UTF-8 字节截断边界 |
| `b4834b6` | **消息内容上 wire 前统一清洗**(请求装配闸 + 会话加载 + 模型输出 + 输入边界),316 兜底不再每轮重打 |

规律:每批修复都是"堵一个口子";但坏串的入口是分散的,只有把防线前移到公共边界(装配闸、加载即洗),才不用靠一个个口子去补。
