// harness_profile.hpp 的实现。语义与 tests/unit/capability/
// test_capability_contract.cpp 的 P0 冻结校验逐条对齐(那边测 golden 与
// 合同,这边供生产消费;两册对账的用例见 tests/unit/app_server/
// test_harness_profile.cpp)。
#include "app_server/harness_profile.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace lubancode::app_server {

namespace {

// 功能名表(冻结合同 §4 十六枚;与测试册 FrozenFeatureNames 同一张)。
const std::vector<std::string>& FrozenFeatureNames() {
    static const std::vector<std::string> names = {
        "filesystem.read", "filesystem.write", "process.exec", "ptc",
        "web",             "browser",          "lsp",          "mcp",
        "plugins",         "skills",           "memory.read",  "memory.write",
        "subagents",       "workflow",         "goal",         "loop",
    };
    return names;
}

// nlohmann 纪律:const json 上 operator[] 查缺键是 UB——取值一律先
// contains(),本文件所有取值都照此办理。

bool GetString(const nlohmann::json& parent, const char* key, std::string* out) {
    if (!parent.is_object() || !parent.contains(key) || !parent[key].is_string()) {
        return false;
    }
    *out = parent[key].get<std::string>();
    return true;
}

// 字符串数组读取:元素全须是串;混入非串是配置错误(明败,不静默跳过)。
bool GetStringArray(const nlohmann::json& parent, const char* key, std::vector<std::string>* out,
                    std::string* error) {
    if (!parent.is_object() || !parent.contains(key)) {
        return true;  // 缺省 = 空名单,合法
    }
    if (!parent[key].is_array()) {
        *error = std::string(key) + " 须是字符串数组";
        return false;
    }
    for (const auto& item : parent[key]) {
        if (!item.is_string()) {
            *error = std::string(key) + " 名单里混入了非字符串元素";
            return false;
        }
        out->push_back(item.get<std::string>());
    }
    return true;
}

// canonical mcp 工具名(mcp:<server>:<tool>)拆 server 段;形状不对回空。
std::string McpServerSegmentOf(const std::string& canonical) {
    const std::size_t first = canonical.find(':');
    if (first == std::string::npos || canonical.substr(0, first) != "mcp") {
        return std::string();
    }
    const std::size_t second = canonical.find(':', first + 1);
    if (second == std::string::npos) {
        return std::string();
    }
    return canonical.substr(first + 1, second - first - 1);
}

}  // namespace

bool IsFrozenFeatureName(std::string_view name) {
    const auto& names = FrozenFeatureNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool HarnessProfile::FeatureEnabled(std::string_view feature) const {
    const std::string key(feature);
    if (features_disabled.count(key) > 0) {
        return false;  // deny 一律优先
    }
    if (features_enabled.count(key) > 0) {
        return true;
    }
    return features_default_enabled;
}

std::set<std::string> HarnessProfile::ReferencedMcpServers() const {
    std::set<std::string> servers;
    for (const std::string& canonical : tools.allow) {
        std::string server = McpServerSegmentOf(canonical);
        if (!server.empty()) {
            servers.insert(std::move(server));
        }
    }
    return servers;
}

HarnessParseResult ParseHarnessDeployment(const nlohmann::json& deployment,
                                           const std::string& profile_name) {
    HarnessParseResult result;
    if (!deployment.is_object()) {
        result.error = "部署档不是 JSON 对象";
        return result;
    }
    if (!deployment.contains("schemaVersion") || !deployment["schemaVersion"].is_number_integer() ||
        deployment["schemaVersion"].get<int>() != 1) {
        result.error = "schemaVersion 须为 1(未知值拒绝,冻结合同 §3)";
        return result;
    }
    if (!deployment.contains("harnessProfiles") || !deployment["harnessProfiles"].is_object() ||
        !deployment["harnessProfiles"].contains(profile_name) ||
        !deployment["harnessProfiles"][profile_name].is_object()) {
        result.error = "harnessProfiles 里没有档: " + profile_name;
        return result;
    }
    const nlohmann::json& raw = deployment["harnessProfiles"][profile_name];
    // 未知键拒绝(additionalProperties=false 的等价执法):拼错的键不许
    // 静默生效。合法键表 = 冻结合同 §3 的档内字段。
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        if (it.key() != "agentRef" && it.key() != "features" && it.key() != "components" &&
            it.key() != "tools" && it.key() != "exposure" && it.key() != "updates" &&
            it.key() != "limits") {
            result.error = "档内未知键(拒绝采用): " + it.key();
            return result;
        }
    }

    HarnessProfile profile;
    profile.name = profile_name;
    GetString(raw, "agentRef", &profile.agent_ref);

    // ---- features 面 ----
    if (raw.contains("features")) {
        const nlohmann::json& features = raw["features"];
        if (!features.is_object()) {
            result.error = "features 须是对象";
            return result;
        }
        for (auto it = features.begin(); it != features.end(); ++it) {
            if (it.key() != "default" && it.key() != "enabled" && it.key() != "disabled") {
                result.error = "features 未知键(拒绝采用): " + it.key();
                return result;
            }
        }
        std::string feature_default;
        if (GetString(features, "default", &feature_default)) {
            if (feature_default != "enabled" && feature_default != "disabled") {
                result.error = "features.default 只认 enabled|disabled: " + feature_default;
                return result;
            }
            profile.features_default_enabled = feature_default == "enabled";
        }
        std::vector<std::string> enabled;
        std::vector<std::string> disabled;
        if (!GetStringArray(features, "enabled", &enabled, &result.error) ||
            !GetStringArray(features, "disabled", &disabled, &result.error)) {
            return result;
        }
        for (const std::string& name : enabled) {
            if (!IsFrozenFeatureName(name)) {
                result.error = "features.enabled 含未知功能名: " + name;
                return result;
            }
            if (profile.features_disabled.count(name) > 0) {
                result.error = "features 同键两名单同现: " + name;
                return result;
            }
            profile.features_enabled.insert(name);
        }
        for (const std::string& name : disabled) {
            if (!IsFrozenFeatureName(name)) {
                result.error = "features.disabled 含未知功能名: " + name;
                return result;
            }
            if (profile.features_enabled.count(name) > 0) {
                result.error = "features 同键两名单同现: " + name;
                return result;
            }
            profile.features_disabled.insert(name);
        }
    }

    // ---- components.mcpServers / components.plugins(P2 点名通道)----
    if (raw.contains("components")) {
        const nlohmann::json& components = raw["components"];
        if (!components.is_object()) {
            result.error = "components 须是对象";
            return result;
        }
        for (auto it = components.begin(); it != components.end(); ++it) {
            if (it.key() != "mcpServers" && it.key() != "plugins") {
                result.error = "components 未知键(拒绝采用): " + it.key();
                return result;
            }
        }
        if (!GetStringArray(components, "mcpServers", &profile.mcp_servers, &result.error) ||
            !GetStringArray(components, "plugins", &profile.plugins, &result.error)) {
            return result;
        }
    }

    // ---- tools(ToolPolicySpec,新 schema;mode 必带)----
    if (!raw.contains("tools") || !raw["tools"].is_object()) {
        result.error = "档缺 tools 段(部署档 schema 1 必带,冻结合同 §2)";
        return result;
    }
    const nlohmann::json& tools = raw["tools"];
    for (auto it = tools.begin(); it != tools.end(); ++it) {
        if (it.key() != "mode" && it.key() != "allow" && it.key() != "deny") {
            result.error = "tools 未知键(拒绝采用): " + it.key();
            return result;
        }
    }
    std::string mode;
    if (!GetString(tools, "mode", &mode)) {
        result.error = "tools.mode 缺失或非串(部署档必带 mode,新旧不混读)";
        return result;
    }
    if (mode == "inherit") {
        profile.tools.mode = HarnessToolPolicy::Mode::Inherit;
    } else if (mode == "only") {
        profile.tools.mode = HarnessToolPolicy::Mode::Only;
    } else if (mode == "none") {
        profile.tools.mode = HarnessToolPolicy::Mode::None;
    } else {
        result.error = "tools.mode 未知值(不落成全工具): " + mode;
        return result;
    }
    if (!GetStringArray(tools, "allow", &profile.tools.allow, &result.error) ||
        !GetStringArray(tools, "deny", &profile.tools.deny, &result.error)) {
        return result;
    }
    const bool allow_nonempty = !profile.tools.allow.empty();
    if (profile.tools.mode == HarnessToolPolicy::Mode::Inherit && allow_nonempty) {
        result.error = "inherit 不接受非空 allow(语义不清,冻结合同 §2.1)";
        return result;
    }
    if (profile.tools.mode == HarnessToolPolicy::Mode::Only && !tools.contains("allow")) {
        result.error = "only 必须带 allow(空数组合法,表示零工具)";
        return result;
    }
    if (profile.tools.mode == HarnessToolPolicy::Mode::None && allow_nonempty) {
        result.error = "none 不接受非空 allow";
        return result;
    }

    // ---- exposure ----
    GetString(raw, "exposure", &profile.exposure);
    if (raw.contains("exposure")) {
        const nlohmann::json& exposure = raw["exposure"];
        if (!exposure.is_object()) {
            result.error = "exposure 须是对象";
            return result;
        }
        for (auto it = exposure.begin(); it != exposure.end(); ++it) {
            if (it.key() != "default") {
                result.error = "exposure 未知键(拒绝采用): " + it.key();
                return result;
            }
        }
        std::string exposure_default;
        if (GetString(exposure, "default", &exposure_default)) {
            if (exposure_default != "direct" && exposure_default != "deferred" &&
                exposure_default != "host_only") {
                result.error = "exposure.default 只认 direct|deferred|host_only: " + exposure_default;
                return result;
            }
            profile.exposure = exposure_default;
        }
    }
    const bool empty_face = profile.tools.mode == HarnessToolPolicy::Mode::None ||
                            (profile.tools.mode == HarnessToolPolicy::Mode::Only &&
                             profile.tools.allow.empty());
    if (empty_face && profile.exposure == "deferred") {
        result.error = "零工具面配 exposure=deferred:发现器依赖解释不出(冻结合同 §3)";
        return result;
    }

    // ---- limits(P1 只消费 stepsPerInput;其余字段收形状不生效)----
    if (raw.contains("limits")) {
        const nlohmann::json& limits = raw["limits"];
        if (!limits.is_object()) {
            result.error = "limits 须是对象";
            return result;
        }
        for (auto it = limits.begin(); it != limits.end(); ++it) {
            if (it.key() != "stepsPerInput" && it.key() != "wallTimeMs" &&
                it.key() != "toolCallsPerInput" && it.key() != "maxOutputTokens") {
                result.error = "limits 未知键(拒绝采用): " + it.key();
                return result;
            }
        }
        if (limits.contains("stepsPerInput")) {
            if (!limits["stepsPerInput"].is_number_integer() ||
                limits["stepsPerInput"].get<int>() < 0) {
                result.error = "limits.stepsPerInput 须是非负整数";
                return result;
            }
            profile.steps_per_input = limits["stepsPerInput"].get<int>();
        }
    }

    // ---- 依赖解释(冻结合同 §3:解释不全的档不许采用)----
    // mcp:<server>:<tool> 须 components.mcpServers 含 <server> 且 features
    // 放行 mcp。P2(应用Worker接入单)起另解释内置 skill 工具:裸名
    // "skill" 须 features 放行 skills。其余非 mcp 前缀的 allow 名仍明拒
    // ——解释不出的名字不允许进 allow。
    for (const std::string& canonical : profile.tools.allow) {
        if (canonical == "skill") {
            if (!profile.FeatureEnabled("skills")) {
                result.error = "tools.allow 点名 skill 工具但 features 未放行 skills: " + canonical;
                return result;
            }
            continue;
        }
        if (canonical.rfind("mcp:", 0) != 0) {
            result.error = "tools.allow 名暂只支持 mcp:<server>:<tool> 与内置名 skill(其余 "
                           "canonical 映射待接线): " +
                           canonical;
            return result;
        }
        const std::string server = McpServerSegmentOf(canonical);
        if (server.empty()) {
            result.error = "canonical 名缺 server 段: " + canonical;
            return result;
        }
        if (std::find(profile.mcp_servers.begin(), profile.mcp_servers.end(), server) ==
            profile.mcp_servers.end()) {
            result.error = "tools.allow 点名的服务不在 components.mcpServers 里: " + canonical;
            return result;
        }
        if (!profile.FeatureEnabled("mcp")) {
            result.error = "tools.allow 点名 mcp 工具但 features 未放行 mcp: " + canonical;
            return result;
        }
    }
    // 零工具档零启动依赖(合同 §2.3):mode=none/only+空 allow 时点名
    // components 也解释不出依赖——档允许点名(MCP 不启动),但 features
    // 未放行 mcp 时点名 mcpServers 是配置矛盾,明拒。
    if (!profile.mcp_servers.empty() && !profile.FeatureEnabled("mcp")) {
        result.error = "components.mcpServers 点名服务但 features 未放行 mcp";
        return result;
    }
    // plugins 点名同理:features 未放行 plugins 是配置矛盾,明拒(放行了
    // 也尚未接线——装配层按 component_unavailable 拒,见 session_assembly)。
    if (!profile.plugins.empty() && !profile.FeatureEnabled("plugins")) {
        result.error = "components.plugins 点名插件但 features 未放行 plugins";
        return result;
    }

    // deny 与 allow 交叠:deny 胜出(裁掉即可,不是错误;deny 作用于 MCP
    // 工具与内置 skill 工具)。
    for (const std::string& denied : profile.tools.deny) {
        if (denied == "skill") {
            profile.tools.allow.erase(
                std::remove(profile.tools.allow.begin(), profile.tools.allow.end(), "skill"),
                profile.tools.allow.end());
            continue;
        }
        if (denied.rfind("mcp:", 0) != 0) {
            continue;  // 其余 deny 名无面可裁,静默(无内置面)
        }
        if (std::find(profile.tools.allow.begin(), profile.tools.allow.end(), denied) !=
            profile.tools.allow.end()) {
            profile.tools.allow.erase(
                std::remove(profile.tools.allow.begin(), profile.tools.allow.end(), denied),
                profile.tools.allow.end());
        }
    }

    result.profile = std::move(profile);
    return result;
}

HarnessParseResult ParseHarnessDeploymentDefault(const nlohmann::json& deployment) {
    HarnessParseResult result;
    if (!deployment.is_object() || !deployment.contains("service") ||
        !deployment["service"].is_object()) {
        result.error = "部署档缺 service 段";
        return result;
    }
    std::string default_profile;
    if (!GetString(deployment["service"], "defaultProfile", &default_profile) ||
        default_profile.empty()) {
        result.error = "service.defaultProfile 缺失(点名档名须显式)";
        return result;
    }
    return ParseHarnessDeployment(deployment, default_profile);
}

HarnessParseResult LoadHarnessDeploymentFile(const std::filesystem::path& path,
                                              std::string profile_name) {
    HarnessParseResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        result.error = "部署档打不开: " + path.generic_string();
        return result;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const nlohmann::json parsed =
        nlohmann::json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        result.error = "部署档不是合法 JSON: " + path.generic_string();
        return result;
    }
    if (profile_name.empty()) {
        return ParseHarnessDeploymentDefault(parsed);
    }
    return ParseHarnessDeployment(parsed, profile_name);
}

}  // namespace lubancode::app_server
