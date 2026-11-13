# SDK 依赖瘦身：更新器归宿主

状态：首笔合同，先写合同再改构建；原生验收只走远端 CI。本笔只拆 updater 与 miniz，不称渠道、Gateway、Lua 或整套依赖已瘦完。

安装 SDK 只供宿主嵌入会话。更新 LubanCode 安装树不归会话核心。当前远端构建图仍把十四份 `src/updater/*.cpp` 和 miniz 收进 SDK 依赖闭包；这些源码没有外部核心调用方。

把更新器收进独立静态目标 `lubancode_updater`，仅 CLI 宿主构建定义它。它向下链接共享 engine 原语和 miniz；engine、runtime、公开 SDK 不反向链接更新器。CLI 兼容 core 显式承接此目标，现有更新器测试、锁竞争夹具和安装更新路仍沿原实现。源码不复制，旧功能不删。

miniz 只在更新器目标需要时下载、配置和编译。SDK-only 的 testing OFF/ON 两种默认 ALL 均不定义更新器或 miniz；组合构建保留更新器，却不能让它或 miniz 进入 SDK 的实际传递构建闭包。公开头与安装接口不添更新器材料。

CI 从本次 CMake File API 读取真实目标、依赖和源码，查 SDK 闭包里有没有更新器目标、miniz 或 `src/updater/`；不靠读 CMake 文本猜依赖。SDK-only 两档另查宿主目标未定义。纯数据反例须抓住直接、间接、伪装目标名下的 updater 源码和缺失依赖。三平台 SDK-only 默认 ALL、移位安装消费、组合构建和完整 CLI 回归都取本笔新头；保留更新器原生册，不借上一轮绿灯。

所有权仍归宿主构建和 CLI 调用方；会话不持更新器，也不添更新器关闭回调。存储 SPI、数据库、移动端打包和其他可选模块另开小笔。
