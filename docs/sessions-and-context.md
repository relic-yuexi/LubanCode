# 会话、上下文与存档

[文档首页](README.md) · [命令参考](commands.md) · [项目记忆](memory-system-design.md) · [架构说明](architecture.md)

LubanCode 把三件事分开：**history** 是当前模型要看的对话，**session** 是磁盘上的事件账，**memory** 是跨会话召回的项目知识。三者互相引用，却不混成一团。

## 一场会话有什么

交互会话启动后会在 `~/.lubancode/sessions/` 建 JSONL。第一行 meta 记录会话 id、cwd、模型等信息。后续事件逐行追加：

```text
meta
user / assistant message
tool call / tool result
usage
compact marker
title
```

逐行追加有两个好处：长会话不用反复重写整份 JSON；进程半途退出时，已 flush 的旧行仍可恢复。坏尾行可以报告并停在最后一条完整事件，不必让整场存档报废。

## 当前目录怎样筛会话

`/sessions` 按 meta.cwd 筛当前目录，并按时间倒序列最近 20 场。`/sessions all` 跨目录列。`--continue` 与裸 `/resume` 都优先找 cwd 对得上的存档。

worktree 是独立路径，故 session 仍按各自 cwd 分开列；项目记忆则按 common git dir 合并。这是有意区分：会话讲“当时在哪干活”，记忆讲“这些目录是不是同一仓库”。

## 恢复与重放

```text
/resume
/resume 3
/resume 20260806-...
```

恢复分两步：

1. 读 JSONL，重建消息历史、标题、工具转录与最近 usage。
2. 按原顺序把用户、助手 Markdown、工具摘要和压缩点画回终端。

之后的新消息继续追加到原文件，不另开一场“看起来像续聊、实际断档”的 session。裸命令在真终端打开方向键菜单；Esc 取消，不改当前会话。

## 标题

`/title` 查看当前标题，`/title 新标题` 追加一条 title 事件。最后一条胜出。标题只管列表与导出展示，不改 id，也不重命名 JSONL。

## Markdown 导出

```text
/export
/export docs/session-review.md
```

默认导到 sessions 目录的 `<id>.md`。导出按事件账生成：用户与助手正文、工具摘要、标题、压缩点都保留。项目记忆不混入导出；本轮临时召回包也不写入 history。

## 上下文由什么组成

模型上下文大体分四块：

1. 稳定系统提示：人格、工作方式、工具方针、协议段。
2. 运行环境：cwd、平台、项目指令、Skill 索引、延迟工具索引。
3. 工具 schema：当前已挂载工具的名称、说明与 JSON Schema。
4. 历史：用户、助手、工具调用与结果，以及压缩摘要。

项目记忆命中内容作为本轮 system suffix 追加。它不进入历史，不随着内部工具来回越积越多；下一条外层用户消息再重算。

## token 与缓存命中

后端把各协议 usage 归一成输入、缓存命中、输出等字段。终端状态与 `/context` 据此估算占用百分比。

“缓存命中”不是 LubanCode 在本机缓存回答。它是模型服务报告：本次输入里有多少 token 复用了服务端 prompt cache。通常系统提示、工具 schema 与旧历史前缀越稳定，命中越多。不同服务是否支持、怎样计费，以服务端返回为准；没有字段时只显示能确认的总输入/输出。

## `/context`

裸敲展示系统提示、工具与历史的占用分解。带参数临时改本场窗口：

```text
/context 256k
/context 512k
/context 1m
/context 1000000
```

`k`、`m` 按十进制。这个值告诉压缩器何时该收历史，也供状态栏计算百分比。它不能凭空放大服务端真实窗口；provider 配错过大，最终仍会被远端拒绝。

## 自动压缩

当历史逼近 `context_window`，主循环会把较旧内容压成摘要，保住最近消息与继续完成任务所需事实。压缩事件写进 session，恢复和导出时能看见边界。

`compact_model` 可指定更便宜或更擅长摘要的模型。留空沿用当前会话模型。压缩失败会保留原历史并报错，不拿半份摘要覆盖旧账。

## 手工压缩

```text
/compact
/compact 保住 API 兼容性决定、失败测试和用户尚未回答的问题
```

参数会并入本次压缩要求。适合长任务中途收束，或自动阈值尚未到、用户已知前文不再需要的场景。

## 两道窗口防线

| 配置 | 单位 | 用处 |
| --- | --- | --- |
| `context_window` | token | 正常预算、状态百分比、自动压缩阈值。 |
| `max_context_chars` | UTF-8 文本字符近似 | 老的硬安全网，防异常历史无限长。 |

二者不能互换。token 因模型 tokenizer 而异，字符硬限只作兜底。

## 工具输出与上下文

- `read_file`、`search`、命令与网页工具各自先限输出，免得一件工具吞掉整窗。
- 终端 transcript 可保留截断后的全文供 `Ctrl+E` 查看；真正回填模型的仍是工具结果。
- 子代理内部翻找不进入主 history，只把末尾结论带回。
- 延迟工具减少每轮 schema 体积；挂载后才开始计入工具预算。
- 图片会以协议 ImageBlock 进入当轮消息；文件本身不复制进 session 文本。

## 清理与删除的区别

| 动作 | 清什么 | 不清什么 |
| --- | --- | --- |
| `/clear` | 当前内存 history 与当前屏面 | 磁盘 session、项目记忆、配置 |
| `/memory forget` | 一条项目记忆移入 archive | 会话历史 |
| 删除 JSONL | 一场磁盘会话 | 其他会话、记忆 |
| `/worktree exit remove` | 工作树目录与分支（须满足安全条件） | 主仓库、共享项目记忆 |

## 并发与落盘

- session 事件由主会话顺序追加。
- 工具转录的屏幕快照另有锁；`Ctrl+O` 监听线程不直接遍历主线程正在改的 vector。
- 项目记忆写入只排 job，后台进程拿项目锁并用临时文件原子替换。
- 后台命令日志放系统临时目录，不写进 session 正文。

## 常见问题

### `--continue` 没恢复我想要的那场

它只找当前 cwd 最近一场。用 `/sessions all` 找 id，再 `/resume <id>`。

### `/context` 显示很满，服务端却没报错

百分比按当前配置窗口算。窗口配小会提前压缩，配大又可能晚于真实限制。先核对 provider/model 目录与服务官方窗口。

### 压缩后还能导出旧内容吗

能。session 是全量事件账，compact 只改后续请求使用的 history 形状。导出会标出压缩点。

### 项目记忆会污染会话吗

不会写入 history、JSONL 或 Markdown 导出。它只作每轮临时 system suffix，且开头明写“只作线索”。

### 缓存命中为何忽高忽低

那是服务端行为。切模型、切 provider、改系统提示、改变工具集合或压缩历史，都可能改掉可复用前缀。LubanCode 只展示远端报告，不伪造命中数。
