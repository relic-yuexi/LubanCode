// ManifestLuaRuntime 的实现(阶段 4)。头文件讲了形状;这里只补口径:
//   - 脚本从盘上读一次(manifest 解析期已过越界/symlink/扩展名校验,这
//     里再核一道"还是普通文件"——解析与挂载之间盘面可能变,坏路径宁可
//     拒挂不硬跑);
//   - 生产 resolver 是 EnvDotEnvSecretResolver(§7.1 顺序:宿主环境 ->
//     插件数据目录 .env,每次调用现读),transport 是 CprBoundedHttpTransport
//     (manifest 网络账照单全收——越权/DNS 安全/字节帽都在那边落锤);
//   - adapter 的 execute 每次现造 LuaCallContext:secrets 账/resolver/
//     transport/limits/cancel 全在调用作用域里拼,调用结束即弃(§九)。
#include "runtime/plugin_lua_manifest.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "platform/paths.hpp"

namespace lubancode::runtime {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

// 读 entry 脚本(manifest.plugin_dir / runtime_entry)。普通文件再核一道;
// 读不动给人话。脚本字节走内存进 LuaHostState::Load,不落第二份盘账。
std::expected<std::string, std::string> ReadEntryScript(const PluginManifest& manifest) {
    const std::filesystem::path entry = manifest.plugin_dir / Utf8ToPath(manifest.runtime_entry);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(entry, ec) || ec) {
        return std::unexpected("runtime.entry 不是普通文件(解析后盘面变过?): " +
                               manifest.runtime_entry);
    }
    std::ifstream in(entry, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected("runtime.entry 读不到: " + manifest.runtime_entry);
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// manifest.tools[].entry 全表(handler 对账的期望侧)。
std::vector<std::string> EntryNames(const PluginManifest& manifest) {
    std::vector<std::string> entries;
    entries.reserve(manifest.tools.size());
    for (const auto& tool : manifest.tools) {
        entries.push_back(tool.entry);
    }
    return entries;
}

// LuaHostState::Options 的共用拼法(生产与 doctor 探针同一份口径)。
LuaHostState::Options HostStateOptions(const PluginManifest& manifest, std::string script) {
    LuaHostState::Options options;
    options.script = std::move(script);
    options.chunk_name = manifest.id + "/" + manifest.runtime_entry;
    options.entries = EntryNames(manifest);
    options.profile = tools::LuaProfile::PureDefault();  // §10.3 profile: pure + host-http
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// ManifestLuaPlugin
// ---------------------------------------------------------------------------

std::string ManifestLuaPlugin::ToolWireName(std::string_view tool_name) const {
    if (package_id.empty()) {
        return BuildPluginToolName(manifest != nullptr ? std::string_view(manifest->id)
                                                       : std::string_view(),
                                   tool_name);
    }
    return BuildPackagedToolWireName("plugin", package_id, local_id, tool_name);
}

std::expected<std::unique_ptr<ManifestLuaPlugin>, std::string> LoadManifestLuaPlugin(
    std::shared_ptr<const PluginManifest> manifest, ManifestLuaLoadOptions options) {
    if (manifest == nullptr) {
        return std::unexpected("manifest 缺失(静态账与挂载账对不上)");
    }
    if (manifest->kind != RuntimeKind::EmbeddedLua || manifest->manifest_version != kPluginManifestVersionV2) {
        return std::unexpected("插件 " + manifest->id + " 不是 v2 embedded-lua(manifest-backed Lua 的唯一合同)");
    }

    auto script = ReadEntryScript(*manifest);
    if (!script.has_value()) {
        return std::unexpected(script.error());
    }

    // 顶层零副作用加载 + handler 对账(§九/§6.1):任一步坏整件拒挂,
    // 不带半个 state——这正是 Package 事务"暂存 Lua state"那一步。
    auto state = LuaHostState::Load(HostStateOptions(*manifest, std::move(*script)));
    if (!state.has_value()) {
        return std::unexpected("Lua 加载失败(" + manifest->runtime_entry + "): " + state.error());
    }

    auto plugin = std::make_unique<ManifestLuaPlugin>();
    plugin->manifest = std::move(manifest);
    plugin->state = std::move(*state);
    plugin->limits = ApplyHttpLimits(plugin->manifest->http_limits);
    plugin->package_id = std::move(options.package_id);
    plugin->local_id = std::move(options.local_id);
    plugin->package_version = std::move(options.package_version);
    plugin->data_dir = options.plugin_data_dir;

    // resolver:注入优先(测试);生产走 env -> 插件数据目录 .env(§7.1)。
    if (options.resolver != nullptr) {
        plugin->resolver = std::move(options.resolver);
    } else {
        SecretResolverOptions resolver_options;
        resolver_options.plugin_data_dir = options.plugin_data_dir;
        resolver_options.declarations = plugin->manifest->secret_declarations;
        resolver_options.env_lookup = std::move(options.env_lookup);
        plugin->resolver = std::make_unique<EnvDotEnvSecretResolver>(std::move(resolver_options));
    }
    // transport:注入优先(测试);生产带 manifest 网络账(DNS 安全、字节
    // 帽、五道边界全在 CprBoundedHttpTransport 落锤)。
    if (options.transport != nullptr) {
        plugin->transport = std::move(options.transport);
    } else {
        CprBoundedHttpTransport::Options transport_options;
        transport_options.permissions = plugin->manifest->network_permissions;
        plugin->transport = std::make_unique<CprBoundedHttpTransport>(std::move(transport_options));
    }
    return plugin;
}

std::optional<std::string> DoctorProbeManifestLua(const PluginManifest& manifest) {
    if (manifest.kind != RuntimeKind::EmbeddedLua ||
        manifest.manifest_version != kPluginManifestVersionV2) {
        return "不是 v2 embedded-lua,探针不适用";
    }
    auto script = ReadEntryScript(manifest);
    if (!script.has_value()) {
        return script.error();
    }
    // 加载即探针:context 为空,顶层 Host API 只能拿 no_active_tool_call,
    // 零网络零 Secret 解析(§九);handler 对账也在这趟里。state 当场丢弃。
    auto state = LuaHostState::Load(HostStateOptions(manifest, std::move(*script)));
    if (!state.has_value()) {
        return state.error();
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ManifestLuaToolAdapter
// ---------------------------------------------------------------------------

ManifestLuaToolAdapter::ManifestLuaToolAdapter(ManifestLuaPlugin* owner,
                                               const PluginDefinition* definition)
    : owner_(owner), definition_(definition) {}

std::string ManifestLuaToolAdapter::name() const { return owner_->ToolWireName(definition_->name); }

std::string ManifestLuaToolAdapter::description() const {
    // 模型可见文本:只有 description 本身。entry/network/secrets/limits 等
    // 宿主元数据一个字节不进 prompt(与 PluginToolAdapter 同一条铁律)。
    return definition_->description;
}

nlohmann::json ManifestLuaToolAdapter::input_schema() const { return definition_->input_schema; }

tools::Tool::Result ManifestLuaToolAdapter::execute(const nlohmann::json& input) {
    const std::atomic<bool>* cancel = owner_ != nullptr ? owner_->cancel : nullptr;
    return Run(input, cancel);
}

tools::Tool::Result ManifestLuaToolAdapter::execute(const nlohmann::json& input,
                                                    const tools::ToolExecutionContext& context) {
    // context 的取消旗优先(本次调用那根);没递进来退回 owner 灌的。
    return Run(input, context.cancel != nullptr ? context.cancel : owner_->cancel);
}

tools::Tool::Result ManifestLuaToolAdapter::Run(const nlohmann::json& input,
                                                const std::atomic<bool>* effective_cancel) {
    // 调用前统一验参(manifest 是合同,与 process 插件同一道门)。
    if (auto problem = ValidateArgumentsAgainstSchema(input, definition_->input_schema);
        problem.has_value()) {
        return {*problem, true};
    }

    // §九第 5/6 步的调用侧:LuaCallContext 只活在这一次 execute 里,
    // secrets/resolver/transport/limits/cancel 全在动态作用域拼装;Call
    // 返回即清空,下一次调用不见这次的 Secret 与取消旗。
    LuaCallContext context;
    context.http.secrets = owner_->manifest->secret_declarations;
    context.http.secret_resolver = owner_->resolver.get();
    context.http.transport = owner_->transport.get();
    context.http.limits = owner_->limits;
    context.http.cancel = effective_cancel;

    const LuaHostState::CallResult call = owner_->state->Call(definition_->entry, input, context);

    // CallResult 与 Tool::Result 阶段 3 已对齐:字段直折,不再猜。
    tools::Tool::Result result{call.content, call.is_error};
    if (call.is_error) {
        result.outcome = call.outcome.empty() ? std::string("plugin_exception") : call.outcome;
        result.error_code = call.error_code;
    } else {
        result.outcome = "succeeded";
    }
    result.details = nlohmann::json{{"plugin", owner_->manifest->id}, {"tool", definition_->name}};
    result.effect_summary = "plugin " + owner_->manifest->id + "/" + definition_->name + "(embedded-lua)";
    return result;
}

// ---------------------------------------------------------------------------
// ManifestLuaRuntime
// ---------------------------------------------------------------------------

std::vector<std::string> ManifestLuaRuntime::LoadFromManifests(
    const std::vector<std::shared_ptr<const PluginManifest>>& manifests) {
    std::vector<std::string> warnings;
    if (loaded_) {
        return warnings;  // 幂等:第二遍只造 adapter,不重挂
    }
    loaded_ = true;
    for (const auto& manifest : manifests) {
        if (manifest->kind != RuntimeKind::EmbeddedLua) {
            continue;  // process 走 PluginToolAdapter,这里只收 v2 embedded-lua
        }
        ManifestLuaLoadOptions options;
        // standalone/project 插件的数据目录同一条路(§7.2):<home>/.lubancode/
        // plugin-data/<id>。源码树 .env 永不进这条路。
        options.plugin_data_dir = StandalonePluginDataDir(manifest->id);
        auto plugin = LoadManifestLuaPlugin(manifest, std::move(options));
        if (!plugin.has_value()) {
            warnings.push_back("[plugin] " + manifest->id + ": " + plugin.error() + ",跳过");
            continue;
        }
        plugins_.push_back(std::move(*plugin));
    }
    return warnings;
}

ManifestLuaPlugin* ManifestLuaRuntime::Adopt(std::unique_ptr<ManifestLuaPlugin> plugin) {
    plugins_.push_back(std::move(plugin));
    return plugins_.back().get();
}

std::vector<std::unique_ptr<ManifestLuaToolAdapter>> ManifestLuaRuntime::MakeAdapters() const {
    std::vector<std::unique_ptr<ManifestLuaToolAdapter>> out;
    for (const auto& plugin : plugins_) {
        if (!plugin->package_id.empty()) {
            // packaged 件的发布归 PublishPackagedLuaPlugins(wire 名 + 来源
            // 账,主/子表各一遍);这里再造会撞名。
            continue;
        }
        for (const auto& tool : plugin->manifest->tools) {
            out.push_back(std::make_unique<ManifestLuaToolAdapter>(plugin.get(), &tool));
        }
    }
    return out;
}

void ManifestLuaRuntime::SetCancel(const std::atomic<bool>* cancel) {
    for (auto& plugin : plugins_) {
        plugin->cancel = cancel;
    }
}

}  // namespace lubancode::runtime
