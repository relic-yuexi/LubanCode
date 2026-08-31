# vLLM 兼容手册（本地端点）

[文档首页](../../README.md) · [Provider 目录](catalog.md) · [配置手册](../../reference/configuration.md) · [命令手册](../../reference/commands.md)

自建 vLLM 接 LubanCode 的实测口径。底稿是真机勘察：vLLM 0.27.1，`qwen3.8-27b`，tp8，`max_model_len` 262144，system_fingerprint `vllm-0.27.1-tp8-e0441ef0`，端点 `http://localhost:8001`，勘察日期 2026-08-31。三面 API（chat/completions、responses、messages）各两路（流式/非流式）逐帧摘录在册——tests/fixtures/api/ 下的 vLLM 夹具即从本册缩样，`source_section` 指回对应段落。

换 vLLM 小版本先重探 §3 的三桩负例再对表：reasoning 字段名在版本间有迁徙史（`reasoning_content` → `reasoning`）。

## 1. 端点与三面地址

`/version` 回 `{"version":"0.27.1"}`。`/v1/models` 回：

```json
{"id":"qwen3.8-27b","owned_by":"vllm","max_model_len":262144}
```

openapi 全量路径：`/v1/chat/completions`（+batch/render/derender）、`/v1/responses`（+`{id}` get/cancel）、`/v1/messages`（+count_tokens）、`/v1/completions`（+render/derender）、`/tokenize`、`/detokenize`、`/metrics`、`/health`。三面齐。

| 面 | base_url 写法 | 程序拼的路 |
|---|---|---|
| openai-chat-completions | `http://localhost:8001/v1` | `base + "/chat/completions"` |
| openai-responses | `http://localhost:8001/v1` | `base + "/responses"` |
| anthropic-messages | `http://localhost:8001`（根，不带 `/v1`） | `base + "/v1/messages"` |

无鉴权部署配 `"auth": "none"`；带 `--api-key` 起的端照常配 key。回环地址可走明文 HTTP，其余地址仍须 HTTPS（见 [Provider 目录](catalog.md)「本地端点」一节）。

## 2. 配置样例

`/provider add` 从目录选 vLLM 预设最省事（`vllm` 主路 Chat 面、`vllm-anthropic` Messages 面）。手写样例与字段说明见 [Provider 目录](catalog.md)「本地端点」一节，此处不重抄。要点三条：

- 主路走 Chat 面：思考展示、工具循环、思考回传、usage 记账一样不缺。
- Messages 面思考关不掉（`thinking.type=disabled` 被端无视），要关思考走 Chat 面。
- Responses 面文本、工具、思考、usage 全通（思考事件 `response.reasoning_text.delta`，LubanCode 0.26.139 起认）。

## 3. 思考方言定论

三面三种方言，各有出处：

| 面 | 流式 | 非流式 | 关思考真生效的写法 |
|---|---|---|---|
| chat/completions | `delta.reasoning`（每帧一片） | `message.reasoning` | `"chat_template_kwargs":{"enable_thinking":false}` |
| responses | `response.reasoning_text.delta` 事件 | `output[].content[].type=="reasoning_text"`；`summary` 恒空 | （未单测；chat_template_kwargs 同源推测，未验证不写死） |
| messages | `thinking_delta` + `signature_delta` | `content[]{type:"thinking",thinking,signature}` | 无——`thinking.type=disabled` 收下但被无视 |

三桩负例，都真测过：

- 顶层 `"enable_thinking": false` → 200，思考照吐（思考文照来，content 为 null）。**被无视。**
- messages 面 `"thinking":{"type":"disabled"}` → 200，思考照吐，stop_reason 撞 max_tokens。**被无视。**
- `"chat_template_kwargs":{"enable_thinking":false}` → 思考消失，`content:"2"` 干净无前导空行。**唯一生效。**

`message.reasoning_content` 这台端不发（流式非流式都无此键）。vLLM 0.27 用新名 `reasoning`。思考正文里模型偶尔自带 `Thinking:\n\n` 或 `Thinking Process:\n\n` 开头——模型行为，非模板注入，解析侧无需处理。思考转正文后 content 首帧常带 `\n\n` 前导（模板产物），原样透传。

## 4. Chat 面帧实录

非流式（基础，max_tokens=200）：

```json
{"choices":[{"message":{"role":"assistant","content":"\n\n2","reasoning":"Thinking:\n\n1. **Identify..."},
  "finish_reason":"stop"}],
 "usage":{"prompt_tokens":59,"completion_tokens":38}}
```

流式帧序（截关键帧）：

```text
data: {"choices":[{"delta":{"role":"assistant","content":""}}]}
data: {"choices":[{"delta":{"reasoning":"We"}}]}
data: {"choices":[{"delta":{"reasoning":" need"}}]}          ... (逐 token)
data: {"choices":[{"delta":{"content":"\n\n2"}}]}
data: {"choices":[{"delta":{},"finish_reason":"stop"}]}
data: [DONE]
```

工具调用流（标准 OpenAI 增量形状，id 是 `chatcmpl-tool-` 前缀，非 `call_`）：

```text
data: {"choices":[{"delta":{"reasoning":"The"}}]}                 ... 22 帧思考
data: {"choices":[{"delta":{"tool_calls":[{"id":"chatcmpl-tool-955ccbe40430534b","type":"function","index":0,
        "function":{"name":"get_weather"}}]}}]}
data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"city\": \""}}]}}]}
data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"北京"}}]}}]}
data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"}"}}]}}]}
data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]}
data: [DONE]
```

`stream_options.include_usage:true` → 末尾多一只 usage-only chunk，`choices:[]`：

```text
data: {"choices":[],"usage":{"prompt_tokens":59,"total_tokens":91,"completion_tokens":32}}
data: [DONE]
```

回合续传（工具循环第二轮，assistant 历史带 `reasoning` 字段 + tool_calls，后接 role:tool）→ 200，模型正确用上工具结果作答。换 `reasoning_content` 字段名回传同样 200 不报错——两枚字段名都收（服务端认没认分不出来，记"不拒收"）。

## 5. Responses 面帧实录

请求形状全兼容：tools 平铺、`max_output_tokens`、`store:false` 200 认下、`input` 收字符串。

非流式 output 数组（思考原文在 `content[].reasoning_text`，vLLM 扩展；OpenAI 官方是 `summary[].summary_text`，两者不同源，这台端 `summary` 恒空）：

```json
{"output":[
 {"id":"rs_aeb964886f47be44","type":"reasoning","summary":[],
  "content":[{"text":"We need answer user: ...","type":"reasoning_text"}],"encrypted_content":null},
 {"id":"...","type":"message","content":[{"type":"output_text","text":"\n\n2"}]},
 "status":"completed","usage":{"input_tokens":59,"output_tokens":38,
   "output_tokens_details":{"reasoning_tokens":0}}}
```

思考流事件序（截关键帧；`response.reasoning_text.delta` 是正文增量，done/part 系是收尾冗余）：

```text
data: {"type":"response.created","response":{...}}
data: {"type":"response.output_item.added","item":{"type":"reasoning","summary":[],"content":null},"output_index":0}
data: {"type":"response.reasoning_part.added","part":{"text":"","type":"reasoning_text"},"item_id":"..."}
data: {"type":"response.reasoning_text.delta","delta":"We","item_id":"...","output_index":0}   ... 逐 token
data: {"type":"response.reasoning_text.done","text":"We need answer user: ..."}
data: {"type":"response.reasoning_part.done","part":{"text":"...","type":"reasoning_text"}}
data: {"type":"response.output_item.done","item":{"type":"reasoning","content":[...]}}
data: {"type":"response.output_item.added","item":{"type":"message","role":"assistant"},"output_index":1}
data: {"type":"response.output_text.delta","delta":"\n\n2","output_index":1}
data: {"type":"response.output_item.done",...}
data: {"type":"response.completed","response":{...整份 output+usage...}}
```

工具调用流（reasoning 项之后；流式 `call_id` 是 `call_` 前缀，非流式 function_call 项带双 id：`id:"fc_..."` 与 `call_id:"chatcmpl-tool-..."`——id 生成两副面孔，客户端两副都得吃得下）：

```text
data: {"type":"response.output_item.added","item":{"type":"function_call","call_id":"call_b6db57c845012a73",
        "name":"get_weather","arguments":"","status":"in_progress"},"output_index":1}
data: {"type":"response.function_call_arguments.delta","delta":"{\"city\": \"","output_index":1}
data: {"type":"response.function_call_arguments.delta","delta":"北京","output_index":1}
data: {"type":"response.function_call_arguments.done","arguments":"{\"city\": \"北京\"}"}
data: {"type":"response.output_item.done","item":{"type":"function_call",...,"status":"completed"}}
data: {"type":"response.completed",...}
```

## 6. Messages 面帧实录

非流式（signature 是 32 位 hex 假签——真 Anthropic 是 base64 Ed25519；工具轮 content 换 `{"type":"tool_use","id":"chatcmpl-tool-...","name":"get_weather","input":{...}}`，`stop_reason:"tool_use"`）：

```json
{"type":"message","role":"assistant","content":[
 {"type":"thinking","thinking":"We need answer user: ...","signature":"533ec53d66e64b2cb30d34753e2914f8"},
 {"type":"text","text":"\n\n2"}],
 "stop_reason":"end_turn","usage":{"input_tokens":59,"output_tokens":34}}
```

思考与工具流帧序（截关键帧；无 ping 帧——真 Anthropic 有，parser 不依赖，无碍。usage 只在 message_start/message_delta 两处，无 cache 字段）：

```text
data: {"type":"message_start","message":{"id":"chatcmpl-b4f43dae7f10a182","role":"assistant",
        "content":[],"usage":{"input_tokens":312,"output_tokens":0}}}
data: {"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}}
data: {"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"用户"}}
data: {"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"4bbde37d..."}}
data: {"type":"content_block_stop","index":0}
data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"chatcmpl-tool-a31f7da795ee7a5b","name":"get_weather","input":{}}}
data: {"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"city\": \""}}
data: {"type":"content_block_stop","index":1}
data: {"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"input_tokens":312,"output_tokens":91}}
data: {"type":"message_stop"}
```

回合续传（assistant 历史带 thinking 块 + hex 假签 + tool_use，后接 tool_result）→ 200，模型接着思考并作答。假签原样回传即可，服务端不验格式。`thinking:{"type":"enabled","budget_tokens":512}` 200 收下（budget 有没有真被约束，小预算探测分辨不出，不写死）。

## 7. metrics 面与 /doctor

`/metrics` 吐 Prometheus 文本。`/doctor cache` 配 `metrics_url`（如 `http://localhost:8001/metrics`）后读数，认这些行：

- `vllm:cache_config_info{...,enable_prefix_caching="True",...}` — 缓存开关（label 形式，True/False 首字母大写）。
- `vllm:prefix_cache_queries_total` / `vllm:prefix_cache_hits_total` — v0 引擎的前缀缓存计数（同名多行取末行总计）。
- `vllm:gpu_prefix_cache_queries_total` / `vllm:gpu_prefix_cache_hits_total`（及 `cpu_` 系）— v1 引擎的新名；旧名缺席时按 gpu+cpu 合并读。
- `vllm:prompt_tokens_cached_total` — 命中折 token 数。
- `vllm:num_requests_running` / `vllm:num_requests_waiting` — 常见负载 gauge，读数行顺带报一句（不是缓存指标，只作现场语境）。

`# HELP`/`# TYPE` 注释行与别的 `vllm:` 计数行（`prompt_tokens_total`、`generation_tokens_total`、直方图 `_bucket` 系）一律跳过不碰。实测样张见 tests/fixtures/metrics/live_vllm_qwen38_prometheus.txt。

思考档位用 `/doctor effort` 探：这台端 qwen3 不吃 `reasoning_effort` 参数（走模板开关），Messages 面 `thinking.type=disabled` 又被无视——探针的判词会如实报"已发关闭请求，端仍吐思考"，别拿 2xx 当档位生效。

## 8. 夹具对照

tests/fixtures/api/ 下的 vLLM 册，`source_section` 指回本册段落：

| 夹具 | 段落 | 钉什么 |
|---|---|---|
| openai_chat/vllm_qwen_reasoning_delta | 4. Chat 面帧实录 | 思考流 + usage-only 尾帧（内部来源） |
| openai_chat/live_vllm_qwen38_tool_episode_stream | 4. Chat 面帧实录 | chatcmpl-tool- 前缀首帧 + include_usage 尾帧 |
| openai_responses/live_vllm_qwen38_reasoning_text_stream | 5. Responses 面帧实录 | reasoning_text.delta 系思考流 |
| openai_responses/live_vllm_qwen38_function_call_stream | 5. Responses 面帧实录 | function_call 项 + arguments 分片 + 双 id |
| anthropic_messages/live_vllm_qwen38_thinking_then_tool_use_stream | 6. Messages 面帧实录 | thinking 块 + hex 假签 + tool_use 一轮 |
| anthropic_messages/live_vllm_minicpm5_post_tool_raw_think | 6. Messages 面帧实录 | 裸 `<think>` 标签负路径（内部来源，MiniCPM5） |

wire 回环两轮集成册（tests/integration/api/test_wire_replay.cpp）里 vLLM 三案：chat 面两轮（`chat_template_kwargs` 开关落线 + `reasoning` 字段回传）、responses 面两轮（`function_call`/`function_call_output` 形状、思考项不回传）、messages 面两轮（thinking 块带 hex signature 原样回传）。
