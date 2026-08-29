// Plugin 与 MCP 挂载事务(统一 Package 封装单阶段 5)——单子 §十"整包成,
// 整包败"的执行版。契约 docs/reference/packages.md §8 第七步:
//
//   Stage(暂存)   包内全部 code 组件逐件起:plugin 起一只探针进程走一遍
//                   协议(process 插件本就一调用一进程,探针证明命令起得来、
//                   协议说得上);MCP 起服 + initialize 握手 + tools/list。
//   Publish(发布) 全件起得来,暂存材料移交调用方:MCP client(已握手)
//                   与 plugin manifest(已验)由装配方并进正式 McpServer-
//                   Runtime 与 ToolRegistry(带 ToolOrigin 来源账)。
//   Rollback(回滚)任何一件起不来,已起的 MCP 进程全停(Shutdown 杀进
//                   程),插件探针进程短命已自退,暂存表整包丢弃——三件都
//                   不进正式账,诊断指到坏件。
//
// 事务单位是包:一只包里的 plugin 与 MCP 同生共死;两只包互不连坐(各自
// 整包成整包败)。standalone MCP 维持"坏一只跳一只"的旧语义,与此路不混。
//
// 信任门联动(阶段 4 语义不动):code_trust != Trusted 的包压根不进事务
// ——code 件连暂存都不进;那道门归 PackageTrustStore,这里只认账。
//
// mcp.yaml 折 McpServerConfig(契约 §5):${package_dir}/${package_data}/
// ${env:NAME} 结构化展开。env 只认整值 ${env:NAME} 占位(解析期已验),
// 真值在起进程那一刻从宿主取;值不落日志,缺的变量只报名不报值。展开后
// 再核一道包根越界(解析期查过,挂载侧再核——占位符规矩的头尾两道闩)。
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"        // McpServerConfig
#include "mcp/client.hpp"           // Client/ToolInfo(暂存 runtime 的本体)
#include "package/component.hpp"    // McpComponentDefinition/ComponentKind
#include "package/mounting.hpp"     // PackageMount(会话钉快照)
#include "runtime/plugin_contract.hpp"  // PluginManifest

namespace lubancode::package {

// ---------------------------------------------------------------------------
// 占位符展开(契约 §5 占位符规矩)
// ---------------------------------------------------------------------------

// 一只包的展开材料:${package_dir} = 包根(只读);${package_data} = 持久
// 数据目录(<home>/.lubancode/package-data/<package-id>,契约 §9——更新包
// 不丢数据)。
struct McpExpansionContext {
    std::filesystem::path package_root;
    std::filesystem::path package_data;
};

// env 变量取值口:缺省 getenv;测试注入假账,不碰真环境。
using EnvLookup = std::function<std::optional<std::string>(const std::string& name)>;

// 展开一段值(args 元素或 env 值)。规矩:
//   - args 值:${package_dir}/${package_data} 逐枚展开(可夹明文);
//     ${env:*} 与 ${project_dir} 在 args 里不认(解析期已拦,这里再拒一道);
//   - env 值:只认整值 ${env:NAME}(别的形状解析期已拒),真值从 lookup 取;
//   - ${package_dir} 展开后词法规范化逃出包根即拒(挂载侧的第二道闩);
//   - 认不得的占位符一概报错,不猜。
// 返回展开后的串;notes(可空)收"env 变量缺,丢了一对"一类不挡路的账。
std::expected<std::string, std::string> ExpandMcpValue(const std::string& raw,
                                                       const McpExpansionContext& ctx, bool env_value,
                                                       const EnvLookup& lookup,
                                                       std::vector<std::string>* notes = nullptr);

// mcp.yaml 的定义折成 MCP runtime 的配置(契约:解析后落为现有
// McpServerConfig,起服/握手/注册全走现有 MCP runtime)。env 占位在这时
// 取真值;缺的变量丢那对并记 notes(只报名,不报值)。
struct PackageMcpRuntimePlan {
    config::McpServerConfig server;  // command/args/env(已结构化展开)
    int timeout_ms = 30000;          // 握手与调用的墙钟帽(起服侧喂 Client)
};
std::expected<PackageMcpRuntimePlan, std::string> BuildMcpRuntimePlan(
    const McpComponentDefinition& def, const McpExpansionContext& ctx, const EnvLookup& lookup,
    std::vector<std::string>* notes = nullptr);

// ---------------------------------------------------------------------------
// 挂载事务
// ---------------------------------------------------------------------------

// 事务输入。cwd_utf8:探针进程的工作目录(缺省项目根,与插件运行时同规矩)。
// package_data_root:${package_data} 的根目录;空 = <home>/.lubancode/
// package-data(拿不到主目录就只记 notes,展开报错——数据目录给不出时不
// 硬造)。env_lookup:缺省 getenv。
struct PackageCodeMountOptions {
    std::string cwd_utf8;
    std::optional<std::filesystem::path> package_data_root;
    EnvLookup env_lookup;
};

// 暂存成品:MCP 一件(client 已握手、tools 已列;调用方接管存活)。
struct StagedPackageMcp {
    std::string package_id;
    std::string package_version;
    std::string canonical_id;      // <pkg>:<local>(展示与来源账)
    std::string wire_server_name;  // %2E 编码段:mcp__<这截>__<tool> 即 wire 名
    std::string display_server_name;  // 带点 canonical 段(给人看)
    std::unique_ptr<mcp::Client> client;
    std::vector<mcp::ToolInfo> tools;
};

// 暂存成品:plugin 一件(探针已过;manifest 的 argv 原封,协议帧用本地
// id——注册名由装配方按 wire 编码另起,见 PluginToolAdapter 的覆盖名)。
struct StagedPackagePlugin {
    std::string package_id;
    std::string package_version;
    std::string canonical_id;
    std::shared_ptr<const runtime::PluginManifest> manifest;
};

// 一条事务诊断:指到坏件(canonical id + 人话)。包级问题(整包没有可用的
// code 件入口之类)component_id 留空。
struct PackageCodeDiagnostic {
    std::string package_id;
    std::string component_id;
    std::string kind_text;  // "plugin" / "mcp_server" / ""
    std::string message;

    std::string Format() const;  // "[package] <pkg>: <component> 起不来: ..." 一句人话
};

// 事务结果:成功的暂存材料 + 失败包的诊断。同一只包的材料要么全在、要么
// 全不在(整包成整包败);attempted_packages 记进过事务的包数(信任门没过
// 的不算——压根没进)。
struct PackageCodeMountResult {
    std::vector<StagedPackageMcp> mcp_servers;
    std::vector<StagedPackagePlugin> plugins;
    std::vector<PackageCodeDiagnostic> diagnostics;
    std::vector<std::string> notes;  // env 变量缺一类的非致命账(只报名不报值)
    std::size_t attempted_packages = 0;

    bool empty() const { return mcp_servers.empty() && plugins.empty() && diagnostics.empty(); }
};

// 跑挂载事务。次序:mount.records 按包 id 序(快照已定);包内按组件账序
// (plugins 目录序、mcp 目录序——AnalyzePackage 的解析序)。任何一件起不
// 来即回滚该包全部暂存(已握手 MCP 进程 Shutdown,插件探针进程自退),
// 诊断入账,继续下一只包。全过程同步,不抛异常。
PackageCodeMountResult MountPackageCode(const PackageMount& mount, const PackageCodeMountOptions& options);

// 插件探针:起一只短命进程走一遍协议。请求帧带 manifest 首件工具与空参,
// 收到任何一帧合法响应(含插件自报 ok=false 的 execution_failed)即算
// "起得来"——进程起了、协议说了话。起不来(SpawnFailed/非零退出/超时/
// 坏 JSON...)返回人话诊断。导出给单测。
struct PluginProbeReport {
    bool ok = false;
    std::string detail;  // 失败时的人话(错误码名 + stderr 尾巴摘要)
};
PluginProbeReport ProbeProcessPlugin(const runtime::PluginManifest& manifest,
                                     const std::string& cwd_utf8);

}  // namespace lubancode::package
