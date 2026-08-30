// ManifestLuaRuntime:manifest-backed Lua 插件的挂载 owner(Lua 受控 HTTP
// 与 Secret 宿主能力单·阶段 4)。
//
// 阶段 3 的 LuaHostState 是执行核(建 state、注册 luban 模块、顶层零副
// 作用加载、handler 对账、动态作用域调用);这一层是它的东家——把 v2
// manifest 接成一件可挂载、可调用、可诊断的插件:
//
//   ManifestLuaPlugin   一件插件的全部机制件:manifest(shared_ptr 钉住)、
//                       LuaHostState(执行核)、SecretResolver(默认
//                       EnvDotEnv;测试可注 fake)、BoundedHttpTransport
//                       (默认 CprBoundedHttpTransport,带 manifest 网络
//                       账)、生效帽、packaged 身份(wire 名)与取消旗。
//   ManifestLuaToolAdapter
//                       manifest.tools 里一件工具的模型侧形状:name/
//                       description/input_schema 只认 manifest(§6.1:工具
//                       定义不在 Lua 里抄第二份 schema);execute 建
//                       LuaCallContext(§九第 5/6 步的调用侧)后交
//                       LuaHostState::Call,CallResult 与 Tool::Result 已对
//                       齐,直折不再猜。
//   ManifestLuaRuntime  owner 本体:standalone 扫描账挂载(幂等)+ Package
//                       事务成品接管(Adopt)+ 每张 registry 一套轻
//                       adapter + 取消链分发。
//
// 与 process 分支的关系(阶段 4 清单第 3 条):独立 owner,不硬塞进
// PluginToolAdapter 的 process 分支——那边只认 stdin/stdout 协议帧,Lua
// 是同进程直调,协议帧不适用。裸 .lua 也不经这里(§一断语 1:裸 Lua 不
// 开 Host API,仍走 EmbeddedLuaRuntime)。
//
// 寿命规矩:owner 必须声明在 registry 之前(析构反序,adapter 里的裸
// 指针不悬垂)——与 EmbeddedLuaRuntime/PluginHost 同一条规矩,由
// ToolRuntime 的成员声明顺序接手。
#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/plugin_contract.hpp"    // PluginManifest/EffectiveHttpLimits
#include "runtime/plugin_http.hpp"        // BoundedHttpTransport(传输 seam)
#include "runtime/plugin_lua_host.hpp"    // LuaHostState(执行核)
#include "runtime/secret_resolver.hpp"    // SecretResolver/SecretEnvLookup
#include "tools/tool.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// ManifestLuaPlugin:一件 manifest-backed Lua 插件的机制件集合。
// ---------------------------------------------------------------------------
class ManifestLuaPlugin {
public:
    ManifestLuaPlugin() = default;
    ~ManifestLuaPlugin() = default;
    ManifestLuaPlugin(const ManifestLuaPlugin&) = delete;
    ManifestLuaPlugin& operator=(const ManifestLuaPlugin&) = delete;

    // 完整工具名(packaged 用 wire 覆盖段,standalone 用 manifest 本地名)。
    // 独立于 adapter 存一份,台账(/plugins)与诊断不必各拼各的。
    std::string ToolWireName(std::string_view tool_name) const;

    std::shared_ptr<const PluginManifest> manifest;
    std::unique_ptr<LuaHostState> state;                    // 执行核(§九六步)
    std::unique_ptr<SecretResolver> resolver;               // 默认 EnvDotEnv
    std::unique_ptr<BoundedHttpTransport> transport;        // 默认 Cpr(带网络账)
    EffectiveHttpLimits limits;                             // ApplyHttpLimits 产物
    // packaged 身份:package_id 非空时工具名走 wire 编码段;standalone 全空。
    std::string package_id;
    std::string local_id;
    std::string package_version;  // packaged 的包版本(ToolOrigin 来源账)
    // 插件数据目录(.env 的家;§7.2 由宿主算,诊断展示用)。
    std::optional<std::filesystem::path> data_dir;
    // ESC 取消链(SetCancel 灌;execute 时 context 的旗优先)。
    const std::atomic<bool>* cancel = nullptr;
};

// 挂载材料。生产路径只递 manifest 与 data_dir;resolver/transport 是
// 测试注入口(生产留空 -> EnvDotEnvSecretResolver + CprBoundedHttpTransport)。
struct ManifestLuaLoadOptions {
    // .env 的落点(standalone:<home>/.lubancode/plugin-data/<id>;packaged:
    // <package-data>/<pkg>/plugins/<local>)。nullopt = 无 dotenv 来源。
    std::optional<std::filesystem::path> plugin_data_dir;
    SecretEnvLookup env_lookup;                    // 空 = platform::GetEnvVar
    std::unique_ptr<SecretResolver> resolver;      // 注入优先
    std::unique_ptr<BoundedHttpTransport> transport;
    // packaged 身份(wire 名与来源账用);standalone 留空。
    std::string package_id;
    std::string local_id;
    std::string package_version;
};

// 挂载一件 v2 embedded-lua 插件:读 entry 脚本(越界/symlink/非 .lua 在
// manifest 解析期已拒,这里再核一道"还是普通文件")-> LuaHostState::Load
// (顶层零副作用 + handler 对账;任一步坏整件拒挂,不带半个 state)->
// 配齐 resolver/transport/limits。失败给人话(挂载警告/事务诊断用)。
std::expected<std::unique_ptr<ManifestLuaPlugin>, std::string> LoadManifestLuaPlugin(
    std::shared_ptr<const PluginManifest> manifest, ManifestLuaLoadOptions options);

// doctor 探针(§10.4):读脚本 + LuaHostState::Load(顶层零副作用探针——
// context 为空,Host API 在加载期拿 no_active_tool_call,零网络零 Secret
// 解析)+ handler 对账。产出的 state 当场丢弃,只回结论。不给 packaged
// 身份、不接 resolver/transport——探针本来就用不到它们。
std::optional<std::string> DoctorProbeManifestLua(const PluginManifest& manifest);

// ---------------------------------------------------------------------------
// ManifestLuaToolAdapter:manifest.tools 一项的模型侧形状。
// ---------------------------------------------------------------------------
class ManifestLuaToolAdapter final : public tools::Tool {
public:
    // owner(ManifestLuaPlugin)须活得比本对象久——由 owner 在 registry
    // 之前声明保证。definition 指向 owner->manifest->tools 的一项,manifest
    // 由 shared_ptr 钉住。
    ManifestLuaToolAdapter(ManifestLuaPlugin* owner, const PluginDefinition* definition);

    std::string name() const override;         // wire 覆盖名或 manifest 本地名
    std::string description() const override;  // 模型可见说明(不带宿主元数据)
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }  // 外部代码,一律先问
    bool deferred() const override { return true; }       // tool_search:延迟挂载
    tools::Tool::Result execute(const nlohmann::json& input) override;
    // 取消旗随调用递进(context 优先,owner 的 SetCancel 兜底)。
    tools::Tool::Result execute(const nlohmann::json& input,
                                const tools::ToolExecutionContext& context) override;

private:
    tools::Tool::Result Run(const nlohmann::json& input, const std::atomic<bool>* effective_cancel);

    ManifestLuaPlugin* owner_;
    const PluginDefinition* definition_;
};

// ---------------------------------------------------------------------------
// ManifestLuaRuntime:owner 本体。
// ---------------------------------------------------------------------------
class ManifestLuaRuntime {
public:
    ManifestLuaRuntime() = default;
    ~ManifestLuaRuntime() = default;
    ManifestLuaRuntime(const ManifestLuaRuntime&) = delete;
    ManifestLuaRuntime& operator=(const ManifestLuaRuntime&) = delete;

    // standalone/project 扫描账里的 v2 manifest 逐件挂(数据目录走
    // StandalonePluginDataDir:<home>/.lubancode/plugin-data/<id>,§7.2)。
    // 幂等:已挂过再调直接回空(第二遍是给别的 registry 造 adapter 用的,
    // 与 EmbeddedLuaRuntime 同一条规矩)。单件坏:一条警告跳过,不连累
    // 其余。返回本次的警告。
    std::vector<std::string> LoadFromManifests(
        const std::vector<std::shared_ptr<const PluginManifest>>& manifests);

    // Package 挂载事务的成品接管(发布段):暂存 state 移交 owner,回裸
    // 指针供 adapter/台账用。事务回滚路的 state 根本到不了这里。
    ManifestLuaPlugin* Adopt(std::unique_ptr<ManifestLuaPlugin> plugin);

    const std::vector<std::unique_ptr<ManifestLuaPlugin>>& plugins() const { return plugins_; }

    // 给一张 registry 造全套轻 adapter(调用方逐个 Register)。
    std::vector<std::unique_ptr<ManifestLuaToolAdapter>> MakeAdapters() const;

    // ESC 取消链:装配层每轮灌给全部插件(不设 = 不检查)。
    void SetCancel(const std::atomic<bool>* cancel);

private:
    std::vector<std::unique_ptr<ManifestLuaPlugin>> plugins_;
    bool loaded_ = false;
};

}  // namespace lubancode::runtime
