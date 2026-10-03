# SDK 依赖瘦身：Release 查询归 CLI

状态：合同先行。基线 `483b2c1a26ec8e991d22a8255ebdcd6f604e641a`。本笔只收残留 Release 查询实现与 Package 禁回门，原生验收只走远端 CI。

前笔已把 `src/updater/*` 与 miniz 收进 CLI 专有 `lubancode_updater`。`src/config/update_checker.cpp` 仍落 engine，SDK-only 与组合 SDK 都会编译它。它供更新查询、Release/资产解析、SemVer 比较；现有生产调用只在 CLI 更新命令和设置命令。Package 实现已在 CLI core，本笔不迁 Package。

将这件 cpp 从 `lubancode_engine` 源表移到既有 `lubancode_updater`，只编译一份。原文件、函数、声明与调用不变。updater 仍向下链 engine/miniz；CLI core 仍显式链 runtime/updater。engine、runtime、SDK 不反向链 updater，不加循环静库，不复制源码。公开 SDK 不添 API、配置、更新请求或回调。

查询与安装更新均由 CLI 宿主持有。会话不持查询服务，不添关闭钩子；现有查询失败、超时和错误返回照旧。SDK 内置 Provider HTTP、渠道、Gateway、Lua、通用 Config 与 Memory YAML 不在这笔范围里。

SDK-only 边界同时拒查询 cpp/hpp 和 `src/package/`；递归 include 也不得把它们带回。真实 SDK File API 闭包继续拒 updater/miniz，再拒残留查询与 Package 源码，不能改个 target 名便躲过。组合构建允许 CLI updater/core 在图中，禁止 SDK 依赖它们；定义 updater 时，查询实现须由它独占一份。

纯数据反例覆盖直接/间接依赖、伪装目标名下的查询/Package、源码重归 engine、遗漏或重复 CLI 查询拥有者、SDK-only 隐藏宿主源码与反向头引用。它们只造 JSON/文本，不跑 CMake、编译器或原生程序。

远端取本笔新头与真实受测 merge/tree：三平台 SDK-only testing OFF/ON 默认 ALL、三平台组合构建及九份真实闭包；移位安装消费和既有 SDK/Worker/Runner 门照跑。完整 CLI 保留 `unit.config.update_checker` 原十案、设置命令和更新命令，updater 原册不删。构建图证明源码归属，CLI 原生回归证明旧调用仍能链接、执行；不借旧绿，也不称所有依赖已瘦完。

十案补一扇 CLI 专用收证门。三平台组合构建后，单独登记并执行原 `unit.config.update_checker`；命令须指向真实 `lubancode_tests`，只带精确 `--source-file=*test_update_checker.cpp`。登记缺件、重件、禁用、无界超时、换册或另加过滤均拒。JUnit 须一册实际运行，无失败、跳过；原始 LastTest 须十案全过，断言非零。保存本头、原源码摘要、登记、JUnit、LastTest 与摘要，各平台独立上传。原全量 Test 不减，SDK focused 不添 CLI 册，SDK-only 闭包不变；纯数据反例不调用 CTest。`66defc37` 首轮原件另存，补门新头只认自身远端实跑。

`0314a967` 原轮只有 Windows/macOS 组合门接独立十案，manylinux 漏了运行与上传入口；Linux 全量中的原册通过不能抵这份缺件。本次同步母线补齐 manylinux 同一 helper 和独立原件上传，两扇 CLI 门与既有全量 Test 都保留。旧 Windows 暂拒开场与其他全量失败各留原账，不用新头绿灯改旧结论。
