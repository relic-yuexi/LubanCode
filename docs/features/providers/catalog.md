# Provider 目录

[文档首页](../../README.md) · [配置手册](../../reference/configuration.md) · [模型与 Schema 深挖](../../architecture/providers/schema.md) · [命令手册](../../reference/commands.md) · [架构说明](../../architecture/README.md)

Provider 目录是一册“厂家与模型默认值”。它替向导备好地址、协议、模型、窗口和推理参数。密钥不在册内，用户配置也不让它暗改。

维护源是 `catalog/providers/` 下的平台分片与 `catalog/manifest.json`，格式由 `catalog/source.schema.json` 约束。发布产物 `catalog/providers.json` 由 `scripts/generate_provider_catalog.py` 确定性合成，格式仍是 `catalog/providers.schema.json`（schema v2）——它是生成文件，禁止手改；`--check` 只读对账。构建时，目录嵌进可执行文件；断网照样能添加 Provider。

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
| `vllm` | vLLM（本地） | Chat Completions | `qwen3.8-27b` |
| `vllm-anthropic` | vLLM（本地, Messages） | Messages | `qwen3.8-27b` |

预设会随目录更新而变。上表写的是本仓库这版，不拿它当永久承诺。用户已经保存的 Provider 不会跟着在线目录暗中改值。

### 本地端点（自建 vLLM 这类）

目录的 `base_url` 规则给本地端点留了门：回环地址（`http://localhost` / `http://127.0.0.1`，可带端口）可以走明文 HTTP，其余地址仍必须 HTTPS——回环不出网，明文无险。

以 vLLM 0.27 起 qwen3 系思考模型为例（真机实测口径，2026-08-31）。用 `/provider add` 从目录选 vLLM 预设最省事；手写的话，下面两份样例最实用，落进 `~/.lubancode/config.json` 的 `providers` 数组即可。三面帧实录（流式/非流式逐帧摘录）另立一册：[vLLM 兼容手册](vllm.md)。

主路走 Chat 面（思考展示、工具循环、思考回传、usage 记账一样不缺）：

```json
{
  "name": "vllm-local",
  "base_url": "http://localhost:8001/v1",
  "wire": "openai-chat-completions",
  "auth": "none",
  "model": "qwen3.8-27b",
  "context_window": 262144,
  "stream_usage": true,
  "reasoning_replay": "tool_episode",
  "reasoning_replay_field": "reasoning"
}
```

- `base_url` 写到 `/v1`（程序直拼 `/chat/completions`）；端口按本机部署改。
- 无鉴权部署写 `"auth": "none"`；带了 `--api-key` 就照常配 key。
- 思考增量字段（`reasoning`）双别名自动兼容，可不写 `reasoning_delta_field`；写了则钉死单字段。
- 模型名与目录条目对上（如 `qwen3.8-27b`）后，`/think` 的开关按方言落 `chat_template_kwargs.enable_thinking` 嵌套键——这是 vLLM/qwen3 模板唯一真生效的关思考路（顶层 `enable_thinking` 这台端不认）。模型名对不上目录时，想静态关思考可加 `"extra_body": {"chat_template_kwargs": {"enable_thinking": false}}`。

Anthropic 兼容面（`/v1/messages`，思考块带假签回传也走得通）：

```json
{
  "name": "vllm-local-msg",
  "base_url": "http://localhost:8001",
  "wire": "anthropic-messages",
  "auth": "none",
  "model": "qwen3.8-27b",
  "context_window": 262144
}
```

- `base_url` 写根（不带 `/v1`，程序直拼 `/v1/messages`），与 Chat 面不同。
- 这面 `thinking.type=disabled` 端上无效，思考关不掉；要关思考走 Chat 面。
- 流式无 ping 帧、signature 是 32 位 hex 假签，程序都不依赖格式，原样回传即可。

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
| `base_url` | 是 | HTTPS API 根地址；本地端点可用 `http://localhost…` / `http://127.0.0.1…`（见「本地端点」一节） |
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
  "deferred_tools": {
    "mode": "native_reference",
    "tool_reference": true,
    "server_tool_search": "regex"
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
| `capabilities` | 能力名到布尔值；供展示和选择使用。几枚有行为的键:`image`/`input_modalities` 声明纯文本的模型,带图片附件的输入在发送前拦住;`always_think` 或 `off_unsupported` 声明思考关不掉的模型,`/think none` 切换前后明说"此端点未证实可关" |
| `deferred_tools` | Anthropic 原生工具搜索能力(动态工具 P3):`mode` 只认 `native_reference`,`tool_reference` 声明 wire 认 `defer_loading`/`tool_reference`,`server_tool_search` 是搜索变体(`regex`/`bm25`,可省)。用户配置 `deferred_tool_mode=native_reference` 或 `auto`(P4 能力驱动档,门开即落原生)且 wire 为 anthropic-messages 时启用;目录不写 = 不声明,第三方兼容端不声明即不开——能力按目录判,不按厂名猜 |
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
5. 必填字段、类型、地址（HTTPS，或 localhost/127.0.0.1 回环）、环境变量名均合 schema。
6. 临时文件写完整，再原子替换缓存。

任一道不成，旧缓存不动。旧缓存也不可用，便退回内置快照。在线更新不会改 `~/.lubancode/config.json`，不会切当前 Provider，也不会碰密钥。

## 10. 维护流程

目录是"分片维护、确定性合成、单文件发布"。`catalog/providers.json` 是生成产物，别手改它。

```text
catalog/
  manifest.json            # 维护格式版本、发布 revision、分片顺序
  providers/<平台>.json     # 36 个平台分片:同平台的协议/地区/套餐变体归一处
  models/<owner>.json      # 公共模型池(按需,见「公共模型与端点」)
  source.schema.json       # 维护格式合同
  providers.schema.json    # 发布格式合同(schema v2,旧客户端认的这份)
  providers.json           # 合成产物,继续入库,禁止手改
scripts/generate_provider_catalog.py   # 生成 / --check 对账 / --self-test 自测
```

添厂家或模型时：

1. 查厂商官方 API 文档，确认端点、协议、模型 ID、窗口和推理参数。
2. 改对应平台分片 `catalog/providers/<平台>.json`；厂家是新平台就新开分片并登记进 `catalog/manifest.json` 的 `shards`（顺序即产物顺序）。
3. 资料有实质变化才更新 `manifest.json` 的 `revision`；纯搬迁、重排版不伪造新 revision。
4. 跑 `python scripts/generate_provider_catalog.py` 重新合成，再跑 `--check` 对账（CI 与 ctest 的 `catalog.consistency` 也挂了同一道门）。
5. 确认 `default_model` 在同一 Provider 的 `models` 里——生成器会拦悬空引用。
6. 不把临时活动模型、未经证实的参数写进稳定目录。
7. 跑 provider catalog 专项测试，再跑全量测试；分片、清单、生成器、schema、产物一起提交。
8. 若 schema 要添字段，先考虑旧版客户端如何回退。

第三方聚合页只能作线索。目录里的事实应以厂商正式文档为准。

生成器先全量校验、后原子替换：重复 JSON 键、Provider ID 撞车、路径越界、未登记分片、默认模型悬空、产物超 2 MiB，任何一道不过都拒绝落盘，原产物不动；同一份维护源重复生成字节恒定。

### 公共模型与端点（维护格式 v2）

多协议平台（`deepseek` 三兄弟、`dashscope` 三兄弟这类）在分片里写成"平台 + 端点"形态：`models_ref` 指向 `catalog/models/<owner>.json` 公共模型池，端点各自声明模型集合与覆写。合成时按"公共模型 → 端点模型覆写"展开成旧 Provider 条目，端点 ID、模型 ID、能力、方言与旧产物逐字段一致。覆写按字段白名单合并：标量替换、数组整体替换、`capabilities` 按键覆盖（不删键）；显式 `false` 是值不是缺失；想给某端点删掉继承来的可选字段，就把整份模型写进该端点的 `endpoint_models`，不走覆写。维护层的 `evidence`（证据来源）与 `aliases`（别名记录）只在维护源里，不进发布产物——runtime 不认识别名，别把它当已生效的映射。

### 2026-09-09 模型资料核对

本次用 [OpenRouter 公开模型列表](https://openrouter.ai/api/v1/models) 发现新增模型，再交叉核对原厂资料。没有发真实模型请求；新增条目的方言验证标记不冒充真机通过。

| 目录条目 | 窗口 token | 最大输出 token | 依据 |
| --- | ---: | ---: | --- |
| `openai/gpt-6-astra` | 1,050,000 | 128,000 | [OpenAI 模型页](https://developers.openai.com/api/docs/models/gpt-6-astra.md)：`low/medium/high/xhigh/max`；不支持 `none`；工具调用使用 Responses。 |
| `anthropic/claude-fable-5-1` | 1,000,000 | 128,000 | [Claude 模型页](https://platform.claude.com/docs/en/models/fable-5-1/overview.md)、[effort](https://platform.claude.com/docs/en/build-with-claude/effort.md)：五档 effort，默认 high，adaptive 始终开启。 |
| `zai`、`zhipu` 的 `glm-5.3-flash` | 1,048,576 | 131,072 | [Z.ai 模型页](https://docs.z.ai/guides/vlm/glm-5.3-flash)、[API 参数](https://docs.z.ai/api-reference/llm/chat-completion.md)、[智谱参数](https://docs.bigmodel.cn/cn/guide/start/concept-param.md)、[官方模型配置](https://huggingface.co/zai-org/GLM-5.3-Flash/raw/main/config.json)：`low/high/max`，思考不可关闭。 |
| `zhipu/glm-5.3`；已有四个 `zai*` 同名条目输出上限纠正 | 1,048,576 | 131,072 | 上述 API 参数、[官方模型配置](https://huggingface.co/zai-org/GLM-5.3/raw/main/config.json)；纠正旧值 12,800，不改已有窗口。 |

边界：

- 表中的 `provider/model` 是目录定位，不是全部原厂 API 的模型 ID 格式。
- OpenRouter 的 GLM 顶层 `context_length`、`top_provider` 和原厂上限存在不同值，不能整表覆盖；本地 `1M=1,000,000` 档不改写厂商原始窗口。
- `gpt-6-astra-pro` 在 OpenRouter 有记录，但本次未取得原厂同 ID 的可靠模型页；未加入 OpenAI 官方条目。`:batch` 变体也未作为普通原厂模型添加。
- 本次新 GLM 条目先补常规 Chat 平台，不机械扩散至全部套餐和 Messages 变体；账户与端点的实际可用列表仍以服务端返回为准。
- GPT-6 Astra 文档还给出最大输入 922,000；当前目录的 `context_window` 表达完整上下文，不是独立输入上限。输出预留仍按既有预算算法，1M 选择本身不构成不会超限的保证。
- 新增窗口档位、目录解析与协议回归由 GitHub Actions 执行，本次不在本地编译或跑 CI。

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
