# Provider 与模型

### extra_body / extra_headers：任意模型特殊参数

有的模型服务藏着些自家专属开关——GLM 的 `thinking` 思考开关、别家的分级 `reasoning_effort`、某个厂商才认的顶层字段——lubancode 不会挨个内置，靠这两个字段自己往请求上加。顶层单 provider 那份配置、`providers` 数组里的每一条，都认这两个字段。

- `extra_body`：JSON object。每次请求都浅合并进请求体顶层——同名键**整个覆盖**内置逻辑（`thinking`、`native_web_search` 的 `tools` 声明等）算出来的值，不做深合并，改个内层键也得把外层整个键重写一遍。合并发生在所有内置逻辑拼完之后、发送之前，覆盖顺序上 `extra_body` 永远最后拍板。
- `extra_headers`：JSON object，值必须是字符串。每次请求追加/覆盖 HTTP 头，同名覆盖内置头（包括 `Authorization`——自己配自己认）；值留空表示删掉这条头。

```json
{
  "providers": [
    {
      "name": "glm",
      "base_url": "https://open.bigmodel.cn/api/paas/v4",
      "wire": "chat_completions",
      "model": "glm-5.2",
      "model_reasoning_effort": "max",
      "extra_body": { "thinking": { "type": "enabled" }, "tool_stream": true },
      "extra_headers": { "X-Api-Version": "2024-06-01" }
    }
  ]
}
```

不想手改 JSON，用 `/provider set` 也能改（改的是全局配置里 `providers` 数组对应那条,`extra_body` 是整段替换语义,不是往里加键；设成 `{}` 或留空清掉）：

```
/provider set glm extra_body {"thinking":{"type":"enabled"},"reasoning_effort":"max"}
/provider set glm extra_header X-Api-Version 2024-06-01
```

`/provider switch <名字>` 成功后会写入 `active_provider`，下次启动仍用它。只存名字，不复制密钥；`LUBANCODE_*` 专属环境变量仍可临时压过。

`/provider list` 只提示配了几个键/几条头（如 `extra_body=2键`），不会把 JSON 原文糊到屏幕上。

## models.json 模型目录

模型目录放在主目录 `~/.lubancode/models.json`。文件顶层写一个 `models` 数组。每项 `slug` 必填，正是发给 API 的模型名；其余字段都可省。目录缺失、整份 JSON 损坏或某条坏掉，只告警并跳过，不拦启动。

```json
{
  "models": [
    {
      "slug": "example-model",
      "display_name": "Example Model",
      "description": "模型说明",
      "default_think": "high",
      "supported_think_levels": [
        { "effort": "none", "description": "直接回答" },
        { "effort": "high", "description": "较深推理" }
      ],
      "base_instructions": "工具调用要果断。",
      "context_window": "1m",
      "supports_parallel_tool_calls": true,
      "input_modalities": ["text"],
      "truncation_policy": "auto"
    }
  ]
}
```

`display_name` 给 `/model` 列表展示；`description` 给人看；`default_think` 是模型默认推理档；`supported_think_levels` 每项写 `effort` 与 `description`；`base_instructions` 单独注入系统提示；`context_window` 认 `k`、`m` 后缀或裸数字；后三项会解析存储，眼下不启用。用户在环境变量或 `config.json` 明写的 `think`、`context_window` 压过目录默认。

### 换模型

编辑 `~/.lubancode/config.json`；只改顶层 `model`。例如：

```json
{
  "model": "example-model"
}
```

只想这一次生效，设 `LUBANCODE_MODEL`，或在交互里用 `/model`。别为了换模型把整份配置重写掉。
