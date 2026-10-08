# SDK 显式网络搜索合同

[SDK](lubancore-sdk.md) · [阶段进度](sdk-stage-status.md) · [网络抓取](sdk-web-fetch.md)

这片属第 2 步内置工具补齐。基线 `5aaad8b5`，实现与三平台验收尚未收口。
公开 `web_search::v1::Options`，宿主同时填 `SessionOptions::web_search`
并显式选择 `builtin_tools={"web_search"}`。缺配置、密钥或声明便拒绝开场。
不读 CLI 配置、环境变量或个人目录；模型不能选服务、改 endpoint 或传密钥。

## 公开什么、谁拥有、怎样关场

支持 Tavily、Brave、Serper 三种现有响应。每场工具持配置与凭据副本，
工具销毁时释放；不写入计划、V3 或能力描述。恢复须重新声明工具、服务与
密钥，按本次开场配置执行，不从历史续出网络权限。

内部 `WebSearchTransport` 收一笔 owned 请求、预算和借用取消旗，默认复用
现有 `PerformFullHttpRequest`。不加网络库，不起工具私有线程。
Close 取消本场、等请求实际返回，再释放工具；不 detach。取消沿底座
Progress/Write 回调，不能保证任意 DNS/OS 调用立刻返回。

CLI 原 SearchConfig 构造与解析行为保留；SDK 用新增有界构造。两路共用
已有响应格式化函数。SDK 配置不改变 CLI 默认搜索行为。

## 配置与边界

| 字段 | 默认 | 合法范围 |
| --- | --- | --- |
| provider | Tavily | Tavily / Brave / Serper，未知枚举拒绝 |
| api_key | 空，须填写 | 1～4096 字节可打印 ASCII，不含空白；只注入认证头 |
| endpoint | 所选服务默认 HTTPS 地址 | 最长 8192 字节；HTTPS，或字面 loopback IP 的 HTTP；禁 userinfo、query、fragment、控制字节 |
| connect_timeout_ms | 10000 | 1～30000，且不大于 total |
| total_timeout_ms | 30000 | 1～120000；限制本次网络传输 |
| max_header_bytes | 64 KiB | 1～512 KiB，回调入口限累计头 |
| max_response_bytes | 4 MiB | 1～8 MiB，回调入口限正文 |
| max_output_bytes | 100 KiB | 256 bytes～1 MiB，含截断提示 |
| max_query_bytes | 4096 | 1～8192，按 UTF-8 字节计 |
| max_results | 10 | 1～10 |

模型只收 `query` 和 `count`。查询须非空、合法 UTF-8 且不超宿主帽。
count 缺省或 null 取 min(5, max_results)，整数夹至 1～max_results；
错类型拒绝，超大无符号整数也先比较再转换，不能溢出。
Brave 查询逐字节 URL 编码；另外两家按原请求字段发送 JSON。

一个调用只发一笔 HTTP 请求，3xx 拒绝，不跟随带凭据跳转。端点由可信宿主
指定；配置替换端点也会改变密钥接收方。网络隔离与部署授权仍归宿主，
HTTPS 或 loopback 校验不证明租户隔离、DNS 钉址或 OS 沙箱。

成功响应只采用宿主允许条数，沿原标题、URL、摘要格式，清洗外来文本，
超输出帽在 UTF-8 边界截断并附提示。服务返回空数组仍是成功的空结果。
非 JSON、错结构、非成功状态、异常及传输错误只回固定 `web_search.*` 码，
不回认证头、原响应错误正文或异常正文。成功服务内容仍属外来材料，
不保证恶意服务不会主动反射凭据。

## 最小用法与失败

```cpp
lubancore::SessionOptions options;
// cwd、model、backend 或 connection 仍按原 SDK 设置。
options.builtin_tools = {"web_search"};
options.web_search = lubancore::web_search::v1::Options{};
options.web_search->provider = lubancore::web_search::v1::Provider::Brave;
options.web_search->api_key = host_secret;
```

未选择工具却填配置报 `sdk.web_search.not_selected`；选了却没配置报
`sdk.web_search.missing_options`；非法配置报 `sdk.web_search.invalid_options`。
上述均在 SDK-owned 模型、MCP 或 HTTP 启动前拒绝。
运行错误分 query/count、redirect、HTTP、header/body 帽、timeout、cancelled
与 invalid_response。预算错误不回完整或半截响应冒充成功。

验收须跑真实公开 SDK 工具回环、三服务请求字段与编码、零请求拒绝、
响应/输出帽、取消与关闭、同 cwd 多场不同配置、同 ID 恢复重新声明，
以及安装后源码树外消费。生产与原生测试只走本源远端三平台 CI。
