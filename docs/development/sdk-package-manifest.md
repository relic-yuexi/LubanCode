# SDK Package 根清单值分析合同

本批基线为渠道宿主分支 `e52dc07f`。先接 `package.yaml` 文本分析，不称完整 Package 装配。扫描目录、七类组件、引用闭合、信任、挂载与 reload 都留原 CLI。

## 公开入口与值

拟公开 `packages::v1::AnalyzeManifest(Input)`。`Input` 持有 YAML 文本，可另给显式 LubanCode 版本和平台；省略即不评那项兼容性，不取进程环境、版本头、HOME 或 cwd。

输入最多 256 KiB，必须为合法 UTF-8 且无 NUL。版本参数沿原 `ParseSemVer`；平台参数只认 `windows/linux/macos`。入口或宿主参数越界报公开错误；清单语法、字段或值错误则返回 owned 分析账，带原 parser 的字段、行号与说明。空清单仍交原 parser 报错，不偷偷变成空包。

返回值含可选根清单、诊断、输入字节摘要，以及版本、平台各自的 `NotDeclared/NotEvaluated/Compatible/Incompatible` 状态。根清单只摊平原 parser 已给出的身份、版本原文、说明、作者、license、homepage、repository、版本范围和平台列表。公开头只用 STL 和公开 SDK 头，不漏 `YAML::Node`、内部 SemVer、组件或宿主对象。

`manifest.has_value()` 仅表示根清单解析成功，不证明整包 valid、组件可用、代码获准或已经挂载。摘要只核输入字节，不认证发布者，不充当目录内容哈希。

## 同一 parser、同一源码 owner

原 `package/semver.cpp` 与 `package/manifest.cpp` 只依赖标准库和已在 engine 使用的 yaml-cpp。两源从 CLI core 机械归到 engine，保正文不变、每源只编一次；旧 CLI 仍调用原 `ParsePackageManifest`。SDK 薄适配层只做输入门和 owned 值投影，不复制 YAML、SemVer 或范围 parser。

原 parser 允许数字、布尔 YAML scalar 转文本。本批保这条 CLI 接受语义，不另宣称清单文本字段只认带引号字符串。版本匹配复用 `VersionSatisfies`，平台匹配按原清单平台名单查值。

SDK source/include/实际 FileAPI 门只为这两 CPP 和两头开精确例外；源必须唯一归 engine。`inventory/component/catalog/mounting/trust/state/code_mounting` 和 Channel、Workflow 宿主仍禁回 SDK。不能放宽整个 `package/` 前缀，也不能让附近文件、递归 include 或重复 target 借例外混进来。

## 拥有与退场

调用同步完成，输入按值接收，结果完整持值。解析节点与临时内部结果随调用销毁。没有 Session 注册、VM、后台线程、回调、writer 或关闭协议；不扫描盘、不写账、不启动模型、工具或子进程。调用方销毁 Runtime/Session 后仍可保留并查询该值。

兼容性必须分清省略与失败。清单未声明约束为 `NotDeclared`；清单声明了但宿主省略参数为 `NotEvaluated`；提供有效参数后才给匹配或不匹配。解析失败不冒充兼容，不产生可执行计划。

## 远端验收与后笔

三平台实际运行原 `test_package_manifest.cpp` 全部 15 CASE，并收精确 source-filter 登记、JUnit、LastTest 和非零断言；CLI 全量原门不减。新增公开安装消费者只用 SDK/STL，验错误字段行号、原 scalar 接受规则、预发布版本、兼容省略/匹配/不匹配、UTF-8/NUL/读取帽和 owned 值寿命。新增 focused/ASan 以真实 source 和最终 CASE 数登记，九份 FileAPI 校两源唯一 owner 与宿主闭包不回流。

第一笔不迁完整 `AnalyzePackage`。后笔先给显式根有界盘点，再拆七类原 parser 的中立材料边界、复用同份字节和引用规则，最后接内容挂载与代码事务；没有这些证据，不称 SDK 已具 CLI 全部 Package 能力。

本地只查文本、文档和纯数据。configure、编译、CTest、原生进程一概交远端 CI。基线 Windows Atomic 夹具仍有真实错误码 5/预期 32 红；须正常合入独立修件后再推新源，不复跑旧头遮红。
