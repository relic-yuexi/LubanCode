# SDK 依赖瘦身：渠道与 Gateway 宿主

状态：合同先行。基线 `ed69b8d67ddc3dbfff5edacfb5c614574ab4fdd5`，树 `ca61ea2748bdb24bcbed92e117bb35ac37d67ae1`。本笔只迁真实宿主实现与构建依赖；原生验收只走远端 CI。

`lubancore_sdk` 私链 runtime，runtime 公链 engine。engine 现编渠道 47 件、Gateway 11 件；runtime 还编相关宿主 11 件。SDK 没装渠道账号、Gateway 或自动任务泵，这些实现却仍落 SDK 编译闭包。源码归属不等于实际链接保留、安装体积或端侧部署证据。

Core 留 `src/channel/types.cpp`、`channel_config.cpp` 及其纯数据头。`config/config.hpp` 存渠道配置；`config.cpp` 真调 `ParseChannelsUserConfig`，后者又用 `ConversationKindFromName`。全局 channels 合并、项目 channels 明拒、字段与严格解析照旧。本笔不拆 Config，不改进程全局 hooks，不删名字含 channel 的中立类型。

目标图按既有 `LUBANCODE_BUILD_CLI` 条件建立，不添公开 SDK 组件：

| 实现束 | 唯一源码拥有者 | 向下依赖 |
| --- | --- | --- |
| 非中立渠道 45 件与 Gateway 11 件 | 新内部静库 `lubancode_channel_host` | `lubancode_engine`；TLS 私链 mbedTLS |
| 渠道/自动任务宿主 11 件 | 新内部静库 `lubancode_channel_runtime` | `lubancode_runtime`、`lubancode_channel_host` |
| CLI、App、AppServer 旧装配 | 既有 `lubancode_core` / `lubancode_app` | 显式带入 `lubancode_channel_runtime` |
| 中立会话与公开 SDK | 既有 engine / runtime / SDK | 不得回链任一新宿主目标 |

宿主 11 件为 `turn_ingress`、`channel_session_host`、`agent_channel_engine`、`headless_executor`、`automation_pump`、`channel_work_pump`、`channel_interaction_broker`、`channel_automation`、`channel_media_service`、`channel_file_delivery`、`headless_progress`。只改这些 cpp 归属，原函数、头、正文与调用保留。`TurnSource` / `TurnIngress` 还供终端，头与类型不迁；渠道转换 cpp 随宿主。Headless 执行器仍供旧 Gateway、渠道与 App 自动任务，不把它另立成第二套 SDK 运行栈。

mbedTLS 的 FetchContent、目标创建与链接一起随真实宿主条件走。SDK-only 不下载、创建或编译 mbedTLS；combined CLI 默认保持完整宿主。`crypt32` 只供渠道 TLS，随实现留在 `lubancode_channel_host`；`ws2_32` 还供中立 `net/http_transport`，engine 照留，`advapi32`、`windowscodecs` 也不动。Lua、Provider HTTP/cpr、Memory YAML 不拆。Package 已 CLI-only，其 ChannelManifest 借用经宿主链取得，不能为它把渠道实现绑回 SDK。平台链接、include、标准与原编译定义须由新唯一拥有者承接；不复制源码、不造静库环。

资源仍由旧宿主装配、持有并关闭。本笔不改账号锁、工作泵、审批、取消、线程、会话和 writer 次序，不承诺已清理后台借用。若静核发现 Config 或中立 hooks 必须反向调用宿主，先收窄、另立接口合同；不得顺手改全局接口来凑图。

边界门须按实际目标与来源同时检查。SDK 闭包只准上述两件中立渠道 cpp，拒其余渠道实现、Gateway、宿主 11 件和两个新 target；递归 include 保留中立配置/类型例外。SDK-only 不定义宿主与 mbedTLS 三 target。combined 图核原 67 件各有唯一正确拥有者，SDK 不能通过间接边取回；updater、Release query、Package 禁回门与 CLI 查询十案门都保留。

本地只读源码、查文档、解析 AST 和纯数据。远端三平台收九份真实 File API 图（combined 各一，SDK-only testing OFF/ON 各一），分别登记真实 source、checkout、双亲与 tree。原 CLI/channel/Gateway/headless/automation 全量与原来源册照跑；SDK 安装消费、focused、ASan、Worker、Runner 门不减。JUnit 和完整 LastTest 须实跑、断言非零；源码计数不能充运行证据。没有原生证据便不宣称链接已过，也不宣称手机或任意端侧最小包已经交付。
