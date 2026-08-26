# 命名与计数规范

[文档首页](../README.md) · [架构说明](../architecture/README.md) · [Query 数据流](../architecture/query-data-flow.md)

名字是给读代码的人看的账本。同一个"轮",有时指用户问答,有时指一次模型请求,有时又指一组工具调用——代码能跑,账却越记越乱。这页把口径钉死:执行层级怎么叫,计数器怎么带单位,新代码 review 时拦哪几条。

## 执行层级词典

| 术语 | 唯一含义 | 起止边界 | 常用计数名 |
| --- | --- | --- | --- |
| `session` | 一份可恢复的会话账 | 建档到关闭、分叉或清空 | `session_id` |
| `turn` | 一次用户轮次 | 接纳 user 消息,到最终 assistant 收口、报错或取消 | `turn_index`、`turn_count` |
| `step` | turn 内一次模型推进 | 拼请求,到收齐一条 assistant 消息;有工具便执行并回填 | `step_index`、`step_count` |
| `attempt` | 同一 step 的一次传输尝试 | 发起 HTTP/SSE,到成功、失败或取消 | `attempt_index`、`attempt_count` |
| `message` | history / session 中一条中立消息 | 一个 role 加若干 content block | `message_count` |
| `tool_call` | assistant 发出的一枚工具调用 | `ToolUseBlock` 到配对 `ToolResultBlock` | `tool_call_count` |
| `task` | 一只子代理或后台命令的任务实体 | 创建到终态、清理 | `task_id` |

边界写死:

```text
session
└─ turn 0
   ├─ user message
   ├─ step 0
   │  ├─ request attempt 0
   │  ├─ assistant message（0..N 个 tool_call）
   │  └─ 0..N 个 tool_result
   ├─ step 1
   │  ├─ request attempt 0
   │  └─ assistant 最终正文
   └─ turn 收口
```

三条铁律:

- 不把一次工具调用叫 step。一个 step 可含多枚工具调用,多开只增 `tool_call_count`。
- 不把一次请求重试叫 step。重试只增 `attempt_count` 与 `request_count`,`step_count` 不动。
- `AgentLoop::Run` 内一次 `send_stream` 就是一枚 step；外层 `TerminalSessionController::RunSessionTurn` 才管一轮 turn。`Agent::Run` 夹在两者之间，持跨 step 状态，不另造层级名。history 裁剪按 user turn 切段，不按模型请求数切。

## 计数器都带单位

- `*_index`:当前第几项,代码里一律 0-based;给用户看时再加一。
- `*_count`:已经发生多少项,空集为 0。
- `max_*`:硬上限。若 `0` 另有"不限"含义,字段旁必须写清。
- `*_remaining` / `*_used`:尚可消费多少 / 已消费多少,名字里带单位,如 `steps_remaining`、`steps_used`。
- 时间带单位:`timeout_ms`、`elapsed_seconds`;字节、字符、token 各写后缀。
- 标识用 `*_id`,位置用 `*_index`;`task_id` 不当数组下标用。

步数上限的配置键是 `max_steps_per_turn`(旧名 `max_turns` 兼容读入,兼容期至少跨一个明确版本窗,删前写 CHANGELOG)。`0 = 不限`,两代同义。

## 通用五条

1. **名字先说领域,再说形态。** `assistant_message`、`tool_results`、`has_tool_use` 是好名字;跨分支、跨回调的 `data`、`info`、`value` 是坏名字。短函数里的 `result`、`out` 可留,一旦跨二十来行就补领域名,如 `send_result`、`trim_report`。
2. **布尔名读起来像一句判断。** 用 `is_`、`has_`、`can_`、`should_`、`needs_`。已有 `TaskState::Running`,就别再存一枚容易走散的 `is_running`。
3. **函数用动词,类型用名词。** 动作 `BuildToolDefinitions`;判断 `ShouldNudgeStepLimit`;数据 `RunOutcome`、`TrimReport`;回调 `on_*` 表事件。public API 用 PascalCase,局部与字段用 snake_case,成员末尾加 `_`,常量加 `k` 前缀。
4. **外部原词与内部术语分账。** `tool_calls`、`finish_reason`、`stop_reason="end_turn"` 属 provider 协议,adapter 边界照录;进了中立层用项目词(`ToolUseBlock` 等)。不为"统一"改写外部 JSON 字段,也不让某家 wire 的叫法漫进主循环。
5. **缩写只留通行词。** `id`、`api`、`url`、`utf8`、`json` 可留;`req`、`resp`、`ctx` 只许待在极短局部,进成员、参数、公共接口就写全。

## review 清单

守门现状:项目只配了 `.clang-format`,没有命名 lint。`clang-tidy` 的 `readability-identifier-naming` 可以在改过的目录试跑,但常见开发机上未必有 clang-tidy,且全量跑误报多——与其上一套人人绕开的假门禁,不如 review 时拦下面四条高风险回归(首期清单,发现新的再补):

- [ ] `AgentLoop` 内新添裸 `turn`,却实指一次模型请求(step)。
- [ ] 无单位的公共计数参数,如 `rounds`、裸 `count`。
- [ ] 同一结构同时存 `state` 与重复布尔状态(如 `state` + `is_running`)。
- [ ] 新公开配置沿用已弃用的 `max_turns`(应写 `max_steps_per_turn`)。

命中任何一条,当场改名,不欠账。
