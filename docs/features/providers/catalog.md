# Provider 目录

[文档首页](../../README.md) · [配置手册](../../reference/configuration.md) · [模型与 Schema 深挖](../../architecture/providers/schema.md) · [命令手册](../../reference/commands.md) · [架构说明](../../architecture/README.md)

Provider 目录是一册“厂家与模型默认值”。它替向导备好地址、协议、模型、窗口和推理参数。密钥不在册内，用户配置也不让它暗改。

仓库源文件是 `catalog/providers.json`，格式由 `catalog/providers.schema.json` 约束。构建时，目录嵌进可执行文件；断网照样能添加 Provider。

若要追 OpenCode/Codex 参考边界、本地 `models.json`、三种 JSON Schema、能力画像与角色路由，读[模型、Provider 与 JSON Schema 深挖](../../architecture/providers/schema.md)。

## 1. 用户怎么用

交互会话里执行：

```text
/provider add
```

从预设菜单选厂家，填 API key 或环境变量，再核对汇总。地址、`wire`、默认模型、上下文窗口、推理档和私有参数会随预设带入。菜单末尾留着“自定义”，可手填任意兼容端点。

手工拉新目录：

```text
/provider refresh
```

裸敲 `/provider` 可看当前 Provider 与管理动作；`/provider switch` 切换已有配置；`/model` 只换当前端点下的模型。

## 2. 当前内置预设

以下表格来自仓库当前 `catalog/providers.json`：

| ID | 展示名 | 协议 | 默认模型 |
| --- | --- | --- | --- |
| `openai` | OpenAI | Responses | `gpt-5.4` |
| `anthropic` | Anthropic | Messages | `claude-sonnet-5` |
| `minimax` | MiniMax | Chat Completions | `MiniMax-M3` |
| `glm` | 智谱 GLM | Chat Completions | `glm-5.2` |
| `glm-cn` | 智谱开放平台（国内） | Chat Completions | `glm-5` |
| `qwen` | 阿里云百炼 Qwen | Responses | `qwen3.7-plus` |
| `deepseek` | DeepSeek | Chat Completions | `deepseek-v4-pro` |
| `kimi` | Kimi（国内） | Chat Completions | `kimi-k2.6` |
| `grok` | xAI Grok | Responses | `grok-4.5` |

预设会随目录更新而变。上表写的是本仓库这版，不拿它当永久承诺。用户已经保存的 Provider 不会跟着在线目录暗中改值。

## 3. 三层目录

```text
仓库 catalog/providers.json
        │ 构建时嵌入
        ▼
可执行文件内置快照 ─────────┐
                            │ 断网或缓存坏时回退
~/.lubancode/cache/         │
  provider-catalog.json ◄───┘
        ▲
        │ HTTPS + ETag
在线目录
```

读取时先看有效缓存，再退回可执行文件内置快照。`/provider add` 若发现缓存超过一天，会顺手试更新。更新失败只报提示，不拦向导。

ETag 另存一份。下次刷新发送条件请求；远端没变，便不重写正文。

## 4. 目录顶层格式

```json
{
  "schema_version": 2,
  "revision": "2026-08-06",
  "providers": {
    "example": {}
  }
}
```

| 字段 | 规矩 |
| --- | --- |
| `schema_version` | 当前只能是 `2` |
| `revision` | `YYYY-MM-DD` |
| `providers` | 以稳定 ID 为键，至少一项 |

顶层与各层对象都禁多余字段。拼错字段时，校验直接报错，免得默默忽略。

## 5. Provider 字段

一条最小预设如下：

```json
{
  "name": "Example AI",
  "wire": "openai-responses",
  "base_url": "https://api.example.com/v1",
  "key_env": "EXAMPLE_API_KEY",
  "default_model": "example-pro",
  "models": {
    "example-pro": { "name": "Example Pro" }
  }
}
```

| 字段 | 必填 | 含义 |
| --- | --- | --- |
| `name` | 是 | 向导展示名 |
| `wire` | 是 | `anthropic-messages`、`openai-responses`、`openai-chat-completions` 或 `google-generate-content` |
| `base_url` | 是 | HTTPS API 根地址 |
| `key_env` | 是 | 推荐保存密钥的环境变量名 |
| `default_model` | 是 | 添加后默认启用的模型 ID |
| `models` | 是 | 模型资料表 |
| `description` | 否 | 菜单补充说明 |
| `model_reasoning_effort` | 否 | Provider 级默认推理档 |
| `native_web_search` | 否 | 是否声明协议原生搜索 |
| `docs_url` | 否 | 厂商文档地址 |
| `extra_body` | 否 | 并入每次请求顶层的 JSON |
| `extra_headers` | 否 | 额外 HTTP headers |

目录与运行配置共用四条规范名。旧配置里的 `anthropic`、`responses`、`chat_completions`、`chat` 仍可读；目录 schema 不收旧名，程序展示与写回也只吐规范名。

## 6. 模型与 reasoning

模型项只强制一个 `name`，其余按需写：

```json
{
  "name": "Example Pro",
  "description": "通用推理模型",
  "context_window": "256k",
  "max_output": 32768,
  "default_think": "medium",
  "capabilities": {
    "image": true,
    "tools": true,
    "reasoning": true
  },
  "reasoning": {
    "controls": [{"kind": "effort", "values": ["low", "medium", "high"]}],
    "supportedEfforts": ["low", "medium", "high"]
  }
}
```

| 字段 | 含义 |
| --- | --- |
| `context_window` | 正整数，或 `256k`、`1m` 这类写法 |
| `max_output` | 最大输出 token |
| `default_think` | 默认推理档 |
| `capabilities` | 能力名到布尔值；供展示和选择使用 |
| `reasoning` | 该模型支持的 effort、toggle、budget 与 wire 方言 |

推理控制按模型直写。`/think` 只展示当前模型声明的档位，请求层也读同一份档案。

## 7. 默认值怎么落进配置

目录只参与“新建 Provider”和“补展示资料”。最终请求看用户保存配置。大致次序如下：

```text
协议内置请求字段
  ← Provider extra_body
  ← Request extra_body
```

对于用户可见资料，则按这一路取：

1. 用户本地 Provider 配置。
2. 当前模型的 reasoning 与能力参数。
3. 在线缓存目录。
4. 可执行文件内置快照。
5. 程序保守默认值。

`/model` 裸敲时查询当前端点真实放出的模型。目录不会把别家模型硬塞给这个端点；它只为命中的 ID 补名称、窗口、推理档和能力资料。

## 8. 密钥与 headers

目录不存 API key。`key_env` 只存环境变量名。

`extra_headers` 若要引用当前密钥，只能写 `${LUBANCODE_API_KEY}` 占位符。请求发出前，程序在内存里替换。缓存、日志与目录文件仍看不见明文。

目录里的 headers 属于默认值。用户手工配置可覆盖；空值还可删除基础 header。覆盖 `Authorization` 这类关键头时，责任落在配置者身上。

## 9. 下载校验

在线目录过这几道门才落盘：

1. HTTPS 请求成功，或返回“未修改”。
2. 响应不超过 2 MiB。
3. JSON 能解析。
4. `schema_version` 认得。
5. 必填字段、类型、HTTPS 地址、环境变量名均合 schema。
6. 临时文件写完整，再原子替换缓存。

任一道不成，旧缓存不动。旧缓存也不可用，便退回内置快照。在线更新不会改 `~/.lubancode/config.json`，不会切当前 Provider，也不会碰密钥。

## 10. 维护流程

添厂家或模型时：

1. 查厂商官方 API 文档，确认端点、协议、模型 ID、窗口和推理参数。
2. 改 `catalog/providers.json`，同时更新 `revision`。
3. 确认 `default_model` 确实存在于同一 Provider 的 `models`。
4. 不把临时活动模型、未经证实的参数写进稳定目录。
5. 跑 provider catalog 专项测试，再跑全量测试。
6. 若 schema 要添字段，先考虑旧版客户端如何回退。

第三方聚合页只能作线索。目录里的事实应以厂商正式文档为准。

## 11. 排错

**`/provider refresh` 成功，菜单却没变**

先看下载目录的 `revision`。也可能远端返回 304，或新条目没过 schema，程序仍在用旧缓存。

**新 Provider 能添加，请求却 404**

`base_url` 是 API 根，未必等于厂商网页地址。再核对 `wire`；把 Messages 端点当 Chat 调，路径再像也不成。

**模型列表少了目录里的模型**

`/model` 的列表以端点响应为准。账号权限、地区或中转站可能只开放一部分。

**在线目录坏了，程序还能启动**

这是预定行为。缓存与内置快照正为这时兜底。

**改了仓库 JSON，现有 exe 没变化**

内置快照在编译时生成。要么重建可执行文件，要么把新版目录放到线上缓存渠道再 `/provider refresh`。
