// Plugin 与 MCP 挂载事务的实现(统一 Package 封装单阶段 5)。头文件讲了
// 三段形状;这里只补实现口径:
//   - plugin 探针走 runtime::RunProcessToolCall(process 协议 v1 的唯一执行
//     路,不另造第二套起进程逻辑);"起得来" = 收到一帧合法响应(含插件
//     自报 ok=false)——进程起了、协议说了话;
//   - MCP 起服用 mcp::Client(现有 MCP runtime),失败原样透传 StartProcess/
//     Initialize/ListTools 的人话;
//   - 回滚 = 已握手 client 逐只 Shutdown(杀进程),暂存 vector 清空即丢
//     (Client 析构也会 Shutdown,这里显式做是为了失败路径与发布路径同形)。
#include "package/code_mounting.hpp"

#include <cstdlib>
#include <iterator>
#include <system_error>

#include "platform/paths.hpp"
#include "runtime/plugin_process.hpp"

namespace lubancode::package {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

// 挂载侧的包根越界闩(与 component.cpp 解析期同一式):展开后的值以包根
// 开头,词法规范化后相对包根若走到 ".." 外面,就是越界。不碰盘(symlink
// 归盘点层的账)。
bool EscapesPackageRoot(const std::filesystem::path& expanded, const std::filesystem::path& package_root) {
    const std::filesystem::path root = package_root.lexically_normal();
    const std::filesystem::path normalized = expanded.lexically_normal();
    const std::filesystem::path rel = normalized.lexically_relative(root);
    if (rel.empty()) {
        // 相对算不出(盘符不同/根本不相关):不在包根里,按越界拒;恰好
        // 等于包根本身的算在(整根引用没有越界)。
        return normalized != root;
    }
    const std::string rel_utf8 = PathToUtf8(rel);
    return rel_utf8 == ".." || rel_utf8.rfind("../", 0) == 0 || rel_utf8.rfind("..\\", 0) == 0;
}

// env 缺省取值口:宿主 getenv。Windows 的 _dupenv 报错路不走,直接 getenv
// (单线程装配期调用,无重入竞争)。
std::optional<std::string> HostEnvLookup(const std::string& name) {
    const char* raw = std::getenv(name.c_str());
    if (raw == nullptr) return std::nullopt;
    return std::string(raw);
}

// 展开后的值若带着 ${package_dir} 头,须整体仍在包根内。逐占位符展开:
// 头一枚占位符决定路径前缀,后继明文与占位符依次拼接。
struct Expansion {
    std::string value;
    bool touched_package_dir = false;  // 展开里用过 ${package_dir}(要过越界闩)
    bool dropped = false;              // env 变量宿主没有:这对不递(notes 已记名)
};

std::expected<Expansion, std::string> ExpandInternal(const std::string& raw,
                                                     const McpExpansionContext& ctx, bool env_value,
                                                     const EnvLookup& lookup,
                                                     std::vector<std::string>* notes) {
    // env 值:只认整值 ${env:NAME}(解析期已验形状,这里对账一遍再取值)。
    if (env_value) {
        if (raw.find("${env:") == 0 && raw.ends_with("}")) {
            const std::string name = raw.substr(6, raw.size() - 7);
            const auto value = lookup != nullptr ? lookup(name) : HostEnvLookup(name);
            if (!value.has_value()) {
                if (notes != nullptr) {
                    // 只报名不报值(契约:值不落清单不进日志)。
                    notes->push_back("env 变量 " + name + " 宿主没有,这一对不递给 MCP server");
                }
                return Expansion{"", false, /*dropped=*/true};
            }
            return Expansion{*value, false};
        }
        return std::unexpected("env 值只认整值 ${env:NAME} 占位,给了: " + raw);
    }

    // args 值:逐枚展开 ${package_dir}/${package_data};别的一概不认。
    Expansion out;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t hit = raw.find("${", pos);
        if (hit == std::string::npos) {
            out.value += raw.substr(pos);
            break;
        }
        out.value += raw.substr(pos, hit - pos);
        const std::size_t close = raw.find('}', hit);
        if (close == std::string::npos) {
            return std::unexpected("占位符没有闭合的 }: " + raw.substr(hit));
        }
        const std::string token = raw.substr(hit, close - hit + 1);
        if (token == "${package_dir}") {
            out.value += PathToUtf8(ctx.package_root.lexically_normal());
            out.touched_package_dir = true;
        } else if (token == "${package_data}") {
            if (ctx.package_data.empty()) {
                return std::unexpected(
                    "${package_data} 展不开:拿不到持久数据目录(主目录缺失时不硬造)");
            }
            out.value += PathToUtf8(ctx.package_data.lexically_normal());
        } else if (token.starts_with("${env:")) {
            return std::unexpected("args 里不认 " + token + "(env 占位只进 env 值)");
        } else {
            return std::unexpected("认不得的占位符: " + token);
        }
        pos = close + 1;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 占位符展开
// ---------------------------------------------------------------------------

std::expected<std::string, std::string> ExpandMcpValue(const std::string& raw,
                                                       const McpExpansionContext& ctx, bool env_value,
                                                       const EnvLookup& lookup,
                                                       std::vector<std::string>* notes) {
    auto expanded = ExpandInternal(raw, ctx, env_value, lookup, notes);
    if (!expanded.has_value()) return std::unexpected(expanded.error());
    // 第二道闩:${package_dir} 展开后的整值不得逃包根(契约 §5:规范化后
    // 逃出包根即拒——解析期查过一遍,挂载侧再核一道)。
    if (expanded->touched_package_dir &&
        EscapesPackageRoot(Utf8ToPath(expanded->value), ctx.package_root)) {
        return std::unexpected("path_escape: 展开后逃出包根: " + raw + " -> " + expanded->value);
    }
    return std::move(expanded->value);
}

std::expected<PackageMcpRuntimePlan, std::string> BuildMcpRuntimePlan(
    const McpComponentDefinition& def, const McpExpansionContext& ctx, const EnvLookup& lookup,
    std::vector<std::string>* notes) {
    PackageMcpRuntimePlan plan;
    plan.timeout_ms = def.timeout_ms > 0 ? def.timeout_ms : 30000;
    if (def.command.find("${") != std::string::npos) {
        // 契约:command 是可执行文件名,占位符只在 args 与 env 值里出现
        //(解析期限过字段;这里再拒一道,双保险)。
        return std::unexpected("runtime.command 不认占位符: " + def.command);
    }
    plan.server.command = def.command;
    for (const auto& arg : def.args) {
        auto value = ExpandMcpValue(arg, ctx, /*env_value=*/false, lookup, notes);
        if (!value.has_value()) return std::unexpected("runtime.args: " + value.error());
        plan.server.args.push_back(std::move(*value));
    }
    for (const auto& [name, raw] : def.env) {
        auto value = ExpandMcpValue(raw, ctx, /*env_value=*/true, lookup, notes);
        if (!value.has_value()) return std::unexpected("runtime.env." + name + ": " + value.error());
        if (value->empty()) continue;  // 宿主没这变量:这对不递(notes 已记名)
        plan.server.env.emplace_back(name, std::move(*value));
    }
    return plan;
}

// ---------------------------------------------------------------------------
// 插件探针
// ---------------------------------------------------------------------------

PluginProbeReport ProbeProcessPlugin(const runtime::PluginManifest& manifest,
                                     const std::string& cwd_utf8) {
    PluginProbeReport report;
    if (manifest.tools.empty()) {
        report.detail = "manifest 没有任何工具(解析期本该拒)";
        return report;
    }
    // 探针帧:首件工具 + 空参。多数插件会回 ok=false 的 execution_failed
    //(缺必填参),那也算"起得来"——进程起了、协议说了话;判的是通道,
    // 不是业务成败。
    runtime::plugin_protocol::ProcessRequest request;
    request.call_id = "package-mount-probe";
    request.plugin = manifest.id;
    request.tool = manifest.tools.front().name;
    request.entry = manifest.tools.front().entry;
    request.arguments = nlohmann::json::object();
    request.context_cwd = cwd_utf8;

    runtime::ProcessCallLimits limits;
    limits.timeout_ms = manifest.timeout_ms;
    const runtime::ProcessCallOutcome outcome =
        runtime::RunProcessToolCall(manifest, request, cwd_utf8, /*cancel=*/nullptr, limits);
    if (outcome.code == runtime::PluginErrorCode::Ok ||
        outcome.code == runtime::PluginErrorCode::PluginReportedError) {
        report.ok = true;
        return report;
    }
    // detail 里已带清过洗的 stderr 尾巴(plugin_process 拼的),不再另拼
    // 生字节——外来编码的原文进日志就是乱码。
    report.detail = std::string(runtime::PluginErrorCodeName(outcome.code)) + ": " + outcome.detail;
    return report;
}

// ---------------------------------------------------------------------------
// 挂载事务
// ---------------------------------------------------------------------------

std::string PackageCodeDiagnostic::Format() const {
    std::string out = "[package] " + package_id + ": ";
    if (!component_id.empty()) {
        out += (kind_text.empty() ? std::string("组件") : kind_text) + " " + component_id + " 起不来: ";
    } else {
        out += "挂载事务失败: ";
    }
    out += message;
    out += "(整包回滚,一件不挂;诊断: /package doctor " + package_id + ")";
    return out;
}

PackageCodeMountResult MountPackageCode(const PackageMount& mount, const PackageCodeMountOptions& options) {
    PackageCodeMountResult result;
    const EnvLookup lookup =
        options.env_lookup != nullptr ? options.env_lookup : &HostEnvLookup;

    for (const auto& record : mount.records) {
        // 信任门联动(阶段 4 语义不动):没过门的包压根不进事务——code 件
        // 连暂存都不进。NoCode 的包没有 code 件,自然也不进。
        if (record.code_trust != CodeTrustStatus::Trusted || !record.mount_plan.has_value()) {
            continue;
        }
        bool has_code = false;
        for (const auto& plan_entry : record.mount_plan->entries) {
            if (plan_entry.code_bearing) {
                has_code = true;
                break;
            }
        }
        if (!has_code) continue;

        ++result.attempted_packages;
        const std::string& package_id = record.inventory.package_id;
        const std::string& version = record.inventory.version_text;
        const McpExpansionContext expansion_ctx{
            record.inventory.package_root,
            options.package_data_root.has_value()
                ? *options.package_data_root / Utf8ToPath(package_id)
                : std::filesystem::path()};

        // ---- Stage:逐件起,坏一件就回滚整包 ----
        std::vector<StagedPackageMcp> staged_mcp;
        std::vector<StagedPackagePlugin> staged_plugins;
        std::optional<PackageCodeDiagnostic> failure;

        for (const auto& component : record.components) {
            if (component.kind != ComponentKind::Plugin && component.kind != ComponentKind::McpServer) {
                continue;
            }
            if (failure.has_value()) break;  // 已有坏件,只收尾不再起

            if (component.kind == ComponentKind::Plugin) {
                if (!component.plugin.has_value()) {
                    failure = PackageCodeDiagnostic{package_id, component.canonical_id, "plugin",
                                                    "plugin.json 解析产物缺失(静态账与挂载账对不上)"};
                    break;
                }
                const runtime::PluginManifest& manifest = *component.plugin;
                if (manifest.kind == runtime::RuntimeKind::EmbeddedLua) {
                    // v2 embedded-lua(阶段 4):与 process 探针同构的暂存
                    // 一步——LoadManifestLuaPlugin 读 entry、LuaHostState::
                    // Load 顶层零副作用加载 + handler 对账,再配齐 resolver/
                    // transport/limits(数据目录:<package-data>/<pkg>/plugins/
                    // <local>,§7.2)。任一步坏即诊断回滚,暂存 state 不留。
                    auto staged_manifest = std::make_shared<const runtime::PluginManifest>(manifest);
                    runtime::ManifestLuaLoadOptions lua_options;
                    if (expansion_ctx.package_data.empty()) {
                        // 拿不到持久数据目录时不硬造:无 dotenv 来源,只查宿主
                        // 环境(与 MCP 的 ${package_data} 同一条规矩)。
                        lua_options.plugin_data_dir = std::nullopt;
                    } else {
                        lua_options.plugin_data_dir =
                            expansion_ctx.package_data / "plugins" /
                            platform::Utf8ToPath(component.local_id);
                    }
                    lua_options.env_lookup = lookup;
                    lua_options.package_id = package_id;
                    lua_options.local_id = component.local_id;
                    lua_options.package_version = version;
                    auto staged_lua =
                        runtime::LoadManifestLuaPlugin(std::move(staged_manifest), std::move(lua_options));
                    if (!staged_lua.has_value()) {
                        failure = PackageCodeDiagnostic{package_id, component.canonical_id, "plugin",
                                                        "Lua 挂载失败: " + staged_lua.error()};
                        break;
                    }
                    StagedPackagePlugin staged;
                    staged.package_id = package_id;
                    staged.package_version = version;
                    staged.canonical_id = component.canonical_id;
                    staged.manifest = (*staged_lua)->manifest;  // shared_ptr 拷贝(adapter 共享)
                    staged.lua = std::move(*staged_lua);
                    staged_plugins.push_back(std::move(staged));
                    continue;
                }
                if (manifest.kind != runtime::RuntimeKind::Process) {
                    // 契约 §10 的退出码里有"runtime 不支持"一档:native-library
                    // 的 manifest 化挂载另开单,明说不猜。
                    failure = PackageCodeDiagnostic{
                        package_id, component.canonical_id, "plugin",
                        "runtime 不支持: kind=" + std::string(runtime::RuntimeKindName(manifest.kind)) +
                            "(packaged 插件现收 process 与 embedded-lua)"};
                    break;
                }
                const PluginProbeReport probe = ProbeProcessPlugin(manifest, options.cwd_utf8);
                if (!probe.ok) {
                    failure = PackageCodeDiagnostic{package_id, component.canonical_id, "plugin",
                                                    "探针进程走不通协议: " + probe.detail};
                    break;
                }
                StagedPackagePlugin staged;
                staged.package_id = package_id;
                staged.package_version = version;
                staged.canonical_id = component.canonical_id;
                staged.manifest = std::make_shared<const runtime::PluginManifest>(manifest);
                staged_plugins.push_back(std::move(staged));
                continue;
            }

            // MCP:折 runtime config -> 起服 -> 握手 -> 列工具。
            if (!component.mcp.has_value()) {
                failure = PackageCodeDiagnostic{package_id, component.canonical_id, "mcp_server",
                                                "mcp.yaml 解析产物缺失(静态账与挂载账对不上)"};
                break;
            }
            const auto plan = BuildMcpRuntimePlan(*component.mcp, expansion_ctx, lookup, &result.notes);
            if (!plan.has_value()) {
                failure = PackageCodeDiagnostic{package_id, component.canonical_id, "mcp_server",
                                                "mcp.yaml 折 runtime 配置失败: " + plan.error()};
                break;
            }
            StagedPackageMcp staged;
            staged.package_id = package_id;
            staged.package_version = version;
            staged.canonical_id = component.canonical_id;
            staged.wire_server_name =
                runtime::EncodeToolWireId(package_id + "." + component.local_id);
            staged.display_server_name = package_id + "." + component.local_id;
            staged.client = std::make_unique<mcp::Client>(staged.wire_server_name);
            const auto start =
                staged.client->StartProcess(plan->server.command, plan->server.args, plan->server.env);
            if (!start.success) {
                failure = PackageCodeDiagnostic{package_id, component.canonical_id, "mcp_server",
                                                "起服失败: " + start.error};
                break;
            }
            staged.client->SetTimeouts(plan->timeout_ms, plan->timeout_ms);
            const auto init = staged.client->Initialize();
            if (!init.has_value()) {
                failure = PackageCodeDiagnostic{package_id, component.canonical_id, "mcp_server",
                                                "initialize 握手失败: " + init.error()};
                break;
            }
            const auto tools = staged.client->ListTools();
            if (!tools.has_value()) {
                failure = PackageCodeDiagnostic{package_id, component.canonical_id, "mcp_server",
                                                "tools/list 失败: " + tools.error()};
                break;
            }
            staged.tools = std::move(*tools);
            staged_mcp.push_back(std::move(staged));
        }

        // ---- Rollback 或 Publish ----
        if (failure.has_value()) {
            // 回滚:已握手的 MCP 进程全停(Shutdown 杀进程;析构另有一道,
            // 这里显式走是为了跟失败诊断同帧)。插件探针进程短命,已自退;
            // 暂存 Lua state 随 staged_plugins 弃置一并 lua_close(§10.2:
            // 任一步失败关掉暂存 state,整包不挂)。
            for (auto& staged : staged_mcp) {
                if (staged.client != nullptr) {
                    staged.client->Shutdown();
                }
            }
            result.diagnostics.push_back(std::move(*failure));
            continue;  // 三件都不进正式账
        }
        result.mcp_servers.insert(result.mcp_servers.end(),
                                  std::make_move_iterator(staged_mcp.begin()),
                                  std::make_move_iterator(staged_mcp.end()));
        result.plugins.insert(result.plugins.end(),
                              std::make_move_iterator(staged_plugins.begin()),
                              std::make_move_iterator(staged_plugins.end()));
    }
    return result;
}

}  // namespace lubancode::package
