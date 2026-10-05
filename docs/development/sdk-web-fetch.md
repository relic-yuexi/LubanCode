# SDK web_fetch 首批合同

这笔让宿主在 `SessionOptions::builtin_tools` 显式选 `web_fetch`。空名单仍不开。可选 `SessionOptions::web_fetch` 只给这一场配置上限；未选工具却填配置、坏上限、重复名字或自定义重名，在开场前拒绝。恢复时沿现有内置工具规则重新显式选择，不从旧历史暗中启用网络。

公开 `web_fetch::v1::Options` 是 owned 值：User-Agent、连接与整次超时、累计响应头与下载字节、整条返回文本上限、最大重定向次数。默认连接 10 秒、整次 30 秒、头 64 KiB、下载 4 MiB、返回 100 KiB、最多 5 次跳转。硬帽为连接 30 秒、整次 120 秒、头 512 KiB、下载 8 MiB、返回 1 MiB、跳转 10 次；返回帽至少 256 bytes，连接不得超过整次时限。模型的 `max_bytes` 只收窄正文，不能加大宿主预算。URL 至多 8192 bytes，User-Agent 至多 256 bytes；拒控制字节、坏 UTF-8、URL 内嵌凭据。

请求只走 HTTP/HTTPS GET。沿用现有网络库与 `net::PerformFullHttpRequest`，关闭传输层自动跳转，逐跳核 Location；相对地址用现有 libcurl URL API 解析。重定向次数、循环、非 HTTP(S)、HTTPS 降到 HTTP、重复 Location 均明确拒绝。每跳扣剩余整次时间和累计头／体字节。下载帽在回调入口执行，不先收完整响应再截；头部字节含状态行、CRLF 和中间响应头。成功正文继续走现有 HTML 清洗、外来文本清洗与 UTF-8 截断；整条返回含元信息也受帽约束。超下载帽报错，不把半截内容冒充完整页面。

每场工具持自己的配置与内部 Transport。Transport 仅收一跳 GET、剩余预算、借用取消旗，返回 owned 状态／头／体及计数；默认实现复用现有 CPR 底座。本笔不公开任意网络回调，不引新网络库、数据库、会话线程或后台 Job。这个窄接口可供后续替换传输，不替宿主提供网络隔离或鉴权。

CLI 同用这套有界执行与取消路径，保留现有 User-Agent、HTML 清洗及 HTTP 错误口径。网络地址范围仍由部署环境、宿主策略决定，不移植 Lua 的公网 allowlist。该工具仍声明 `ReadOnlyRemote`、沿 CLI 免确认规则；SDK 宿主显式选工具即开放此能力，已有 Action 可在调用前裁决。

取消旗只借到本次 execute 返回；调用前取消零请求，传输中由现有 Progress/Write 回调停止，跳转前再查取消。Close 沿 SDK 先取消、等真正返回、再销毁工具／资源的次序；不 detach 逃过关闭。取消是合作式，等待首字节时取决于 libcurl 回调唤醒（既有底座约一秒），不承诺强杀任意 DNS/OS 调用。整次时限另交 libcurl 原生总超时，避免回调周期把硬帽拖长。

验收用本机 HTTP 服务与真实公开 SDK：默认关闭／坏配置零请求；HTML 与 UTF-8 截断；真实头／体帽和跨跳累计；相对跳转／循环／次数门；调用前及等首字节取消；同项目两场、不同项目两场配置隔离；Close／Shutdown 等待收尾，返回值在关场后仍自持。原 CLI 纯函数案保留。源码与纯数据可本地查；配置、编译、CTest、原生执行一律交远端 CI。

libcurl 相对 URL 与凭据拒绝行为按官方 [`curl_url_set`](https://curl.se/libcurl/c/curl_url_set.html) 合同实现。
