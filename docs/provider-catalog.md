# Provider 目录

[文档首页](README.md) · [配置手册](configuration.md) · [架构说明](architecture.md)

常见厂家资料放在仓库的 `catalog/providers.json`，格式约束在 `catalog/providers.schema.json`。构建时，这份 JSON 会嵌进 exe。断网也能选 provider。

## 怎么用

交互会话里执行：

```text
/provider add
```

菜单现有 OpenAI、Anthropic、MiniMax、智谱 GLM、阿里云百炼 Qwen、DeepSeek、Kimi 与 xAI Grok。选定后只需填 Key，再看一眼汇总。地址、协议、默认模型、上下文窗口、推理档位和厂商私有参数都从目录带入。最后一项“自定义”仍走旧向导。

手工更新目录：

```text
/provider refresh
```

`/provider add` 发现缓存超过一天，也会先试着更新。网络不通、远端 JSON 损坏、Schema 版本不认或文件超过 2MB，都不会挡住向导，只会退回缓存或 exe 内置快照。

## 文件与优先级

```text
仓库 catalog/providers.json
        │ 构建时嵌入
        ▼
exe 内置快照 ──断网回退
        ▲
~/.lubancode/cache/provider-catalog.json
        ▲ GitHub + ETag
在线目录
```

目录只给默认值。真正生效时，优先级如下：

1. 用户本地 provider 配置。
2. 当前模型 variant 参数。
3. 在线缓存目录。
4. exe 内置快照。
5. 保守默认值。

`/models` 返回值只说明当前端点放出了哪些模型。目录负责补展示名、窗口、推理档位与能力资料，不会把全世界模型强塞进某个中转站。

## 安全边界

- 仓库 JSON 不存任何 Key。
- `extra_headers` 里只许写 `${LUBANCODE_API_KEY}` 占位符，发送前才在内存里替换。
- 下载内容先限长、解析、查 Schema 版本，再替换缓存。
- 缓存用临时文件写完，再原子替换。
- 在线更新不会改已经落盘的 provider，也不会暗中切换当前端点。

## 维护目录

添模型时，先改 `catalog/providers.json`，再校验 Schema、跑 `test_provider_catalog` 与全量测试。模型窗口和参数应以厂家官方文档为准。第三方目录只可作线索，不能压过官方资料与用户配置。
