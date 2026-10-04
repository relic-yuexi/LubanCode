# SDK 主场 todo_write

本笔把已有 `TodoWriteTool` 接入 SDK 主会话。宿主只填公开
`SessionOptions::builtin_tools`，不复制 CLI 装配栈，也不借私有清单。

## 准入与所有权

- 宿主显式选择 `todo_write`。空名单仍不开；重复名字、与自定义工具重名
  仍走 SDK 原有拒绝门。公开头、`SessionOptions` 布局不变。
- `CreateLocalTool` 每次造一份 `TodoListState`，交给这枚工具持有。
  同一场多轮共用；同项目、异项目、不同 Runtime 各场都不共用。
- 主场执行仍串行。沿用工具原有保守副作用声明和免确认规则；不把它
  标成并行只读工具，不扩 SDK 子 Agent 名单。子场仍拒 `todo_write`。
- 清单只存本场内存。工具不打开文件、线程、进程或网络连接。SDK 现有
  Agent → registry 销毁次序收掉工具及清单；预检失败也收掉候选。

## 写入与恢复

`items` 每次整表替换。`content` 非空；`status` 只认 `pending`、
`in_progress`、`completed`。空数组清空。所有项先验完再换表；坏项不得
留下半张新表。原有创建、更新、无变化、清空回执沿用，不改 CLI 工具正文。

同一 V3 ID 恢复时，历史仍按 SDK 原路径恢复，工具结果仍在历史里；清单
重新起空表。这沿 CLI 当前内存清单习惯。宿主每次开场仍显式选工具，
本笔不增清单快照、重放、持久计划或恢复接口。

CLI 的 `/todos` 展示、回合收口提醒、压缩时保留活动项，以及子 Agent
私有清单，仍归各自宿主。这笔只交主场工具接线，不冒称 CLI 全部对齐。

## 验收

新原生册为 `tests/integration/sdk/test_lubancore_todo_write.cpp`。
六个 CASE 走实际公开 SDK、Backend 请求、工具执行及历史回传：

1. `admission`：默认关闭、显式准入、重复及自定义重名拒绝、子场仍拒。
2. `replacement`：跨轮创建、整表替换、重复写入及空表清空。
3. `validation`：坏枚举、坏对象、空内容及错形数组拒绝；后续重写旧表
   仍报无变化，证明旧清单未受污染。
4. `isolation`：同项目两场、异项目及异 Runtime 各场第一次写入都报创建；
   交错继续写，各自比较本场旧表。
5. `recovery`：关闭后同 ID 恢复，历史可见，第一次写入仍报创建。
6. `lifetime`：取消、关闭、销毁沿原 SDK 寿命门；已返回结果仍自持，
   关闭后不得再提交，新场不得接走旧清单。

每册打印 `[sdk-todo-write-path] <name>`。安装消费者为
`examples/sdk-consumer/todo_write.cpp`，入口 `TodoWrite(args)`；仅用公开
SDK 与 STL，实际跑模型工具轮次，不包含私有 Todo 类型。

Linux、macOS、Windows 在远程 CI 验安装迁位及原生册；Lua ON/OFF 都要跑。
本地只核文本、路径、源码守卫。原 CLI todo 原生册不删案、不改断言。
