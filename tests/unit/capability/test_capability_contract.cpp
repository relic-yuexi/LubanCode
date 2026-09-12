// 能力裁剪与工具扩展冻结合同的静态校验(工业化多协议接入单 P0)。
//
// 合同正文:docs/reference/capability-contract.md。本册只做静态可验的
// 三件事,不接模型、不联网、不起任何监听:
//   1. 两份部署档 golden(最小测试工具集/零工具)过 deployment.schema.json
//      的机器校验——这是 P0 退出条件"配置和协议 golden 示例通过 schema
//      校验"的 CI 侧执法;
//   2. ToolPolicySpec 新旧 schema 不混读:旧写法(无 mode)按冻结合同
//      显式换算,空 allow 永不解释成 none;新写法未知/矛盾组合全拒;
//   3. 依赖全解释:mcp:<server>:<tool> 名字要求 components+features 放行,
//      零工具档零启动依赖;2.0 报文 golden 的信封与反向 id 空间自洽。
//
// 校验器是 JSON Schema 的受限子集(type/enum/const/required/properties/
// additionalProperties/items/minItems/minLength/minimum/minProperties/
// 内部 $ref),住在测试侧——P2 统一 feature/module/工具选择解析时,本册
// 的语义检查随合同搬进生产解析器,这里退回只对生产实现做对账。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "capability";

// nlohmann 纪律:const json 上 operator[] 查缺键是 UB,取值一律先
// contains();本册所有取值函数都照此办理。
std::optional<json> ReadJsonFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const json parsed = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return std::nullopt;
    }
    return parsed;
}

// ---------------------------------------------------------------------------
// 受限 JSON Schema 校验器
// ---------------------------------------------------------------------------

const json* ResolveRef(const std::string& ref, const json& root, std::string* error) {
    // 只支持本文件内部的 #/definitions/<名字>(#/properties/... 一类不收)。
    const std::string prefix = "#/definitions/";
    if (ref.rfind(prefix, 0) != 0 || ref.size() <= prefix.size()) {
        *error = "不支持的 $ref(只认 #/definitions/...): " + ref;
        return nullptr;
    }
    const std::string name = ref.substr(prefix.size());
    if (!root.contains("definitions") || !root["definitions"].contains(name)) {
        *error = "$ref 指到的 definition 不存在: " + ref;
        return nullptr;
    }
    return &root["definitions"][name];
}

std::string TypeNameOf(const json& value) {
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_null()) return "null";
    return "unknown";
}

bool TypeMatches(const json& value, const std::string& expected) {
    if (expected == "object") return value.is_object();
    if (expected == "array") return value.is_array();
    if (expected == "string") return value.is_string();
    if (expected == "boolean") return value.is_boolean();
    if (expected == "integer") return value.is_number_integer() && !value.is_boolean();
    if (expected == "number") return value.is_number() && !value.is_boolean();
    if (expected == "null") return value.is_null();
    return false;
}

// 返回空串=通过;否则是人话错误(带实例路径)。
std::string ValidateAgainst(const json& instance, const json& schema, const json& root,
                            const std::string& path) {
    std::string ref_error;
    if (schema.contains("$ref")) {
        if (!schema["$ref"].is_string()) {
            return path + ": $ref 须是字符串";
        }
        const json* target = ResolveRef(schema["$ref"].get<std::string>(), root, &ref_error);
        if (target == nullptr) {
            return path + ": " + ref_error;
        }
        return ValidateAgainst(instance, *target, root, path);
    }

    if (schema.contains("type")) {
        if (!schema["type"].is_string()) {
            return path + ": schema 的 type 须是字符串";
        }
        if (!TypeMatches(instance, schema["type"].get<std::string>())) {
            return path + ": 期望 " + schema["type"].get<std::string>() + ",实得 " + TypeNameOf(instance);
        }
    }

    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool matched = false;
        for (const auto& allowed : schema["enum"]) {
            if (instance == allowed) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return path + ": 值不在 enum 内";
        }
    }

    if (schema.contains("const")) {
        if (instance != schema["const"]) {
            return path + ": 值不等于 const 约束";
        }
    }

    if (schema.contains("minLength") && instance.is_string()) {
        if (instance.get<std::string>().size() < schema["minLength"].get<std::uint64_t>()) {
            return path + ": 短于 minLength";
        }
    }

    if (schema.contains("minimum") && instance.is_number_integer()) {
        if (instance.get<std::int64_t>() < schema["minimum"].get<std::int64_t>()) {
            return path + ": 低于 minimum";
        }
    }

    if (instance.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& key : schema["required"]) {
                if (!key.is_string() || !instance.contains(key.get<std::string>())) {
                    return path + ": 缺必填键 " + (key.is_string() ? key.get<std::string>() : "<非串>");
                }
            }
        }
        if (schema.contains("minProperties")) {
            if (instance.size() < schema["minProperties"].get<std::uint64_t>()) {
                return path + ": 键数少于 minProperties";
            }
        }
        if (schema.contains("properties") && schema["properties"].is_object()) {
            for (auto it = instance.begin(); it != instance.end(); ++it) {
                if (schema["properties"].contains(it.key())) {
                    const std::string child_error =
                        ValidateAgainst(it.value(), schema["properties"][it.key()], root,
                                        path + "/" + it.key());
                    if (!child_error.empty()) {
                        return child_error;
                    }
                } else if (schema.contains("additionalProperties") &&
                           schema["additionalProperties"].is_boolean() &&
                           !schema["additionalProperties"].get<bool>()) {
                    return path + "/" + it.key() + ": 未知键(additionalProperties=false)";
                }
            }
        }
        if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object()) {
            for (auto it = instance.begin(); it != instance.end(); ++it) {
                if (schema.contains("properties") && schema["properties"].contains(it.key())) {
                    continue;  // 具名键走上面的分支
                }
                const std::string child_error =
                    ValidateAgainst(it.value(), schema["additionalProperties"], root,
                                    path + "/" + it.key());
                if (!child_error.empty()) {
                    return child_error;
                }
            }
        }
    }

    if (instance.is_array()) {
        if (schema.contains("minItems")) {
            if (instance.size() < schema["minItems"].get<std::uint64_t>()) {
                return path + ": 元素少于 minItems";
            }
        }
        if (schema.contains("items") && !instance.empty()) {
            for (std::size_t i = 0; i < instance.size(); ++i) {
                const std::string child_error =
                    ValidateAgainst(instance[i], schema["items"], root,
                                    path + "[" + std::to_string(i) + "]");
                if (!child_error.empty()) {
                    return child_error;
                }
            }
        }
    }

    return std::string();
}

// ---------------------------------------------------------------------------
// ToolPolicySpec 合同语义(schema 之外的部分)
// ---------------------------------------------------------------------------

// 功能名表(冻结合同 §4,十六枚)。golden 的 features 名单只认这些键。
const std::vector<std::string>& FrozenFeatureNames() {
    static const std::vector<std::string> names = {
        "filesystem.read", "filesystem.write", "process.exec", "ptc",
        "web",             "browser",          "lsp",          "mcp",
        "plugins",         "skills",           "memory.read",  "memory.write",
        "subagents",       "workflow",         "goal",         "loop",
    };
    return names;
}

bool IsKnownFeature(const std::string& name) {
    const auto& names = FrozenFeatureNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

// 新 schema 的 mode/allow/deny 组合约束(冻结合同 §2.1)。schema 只能管
// "mode 是三个枚举值之一";谁与谁互斥,在这里执法。
std::string CheckToolPolicySemantics(const json& tools) {
    if (!tools.is_object() || !tools.contains("mode") || !tools["mode"].is_string()) {
        return "tools.mode 缺失或非串";
    }
    const std::string mode = tools["mode"].get<std::string>();
    const bool has_allow = tools.contains("allow") && tools["allow"].is_array();
    const bool allow_nonempty = has_allow && !tools["allow"].empty();
    if (mode == "inherit" && allow_nonempty) {
        return "inherit 不接受非空 allow(语义不清,冻结合同 §2.1)";
    }
    if (mode == "only" && !has_allow) {
        return "only 必须带 allow(空数组合法,表示零工具)";
    }
    if (mode == "none" && allow_nonempty) {
        return "none 不接受非空 allow";
    }
    return std::string();
}

// 旧 schema(AgentToolRules,无 mode 键)→ 新写法的显式换算(冻结合同
// §2.2)。换算结果可再过 CheckToolPolicySemantics;旧写法的空 allow 永远
// 换成 inherit,不解释成 none。
struct LegacyConversion {
    json converted;
    std::string rule;  // 命中的换算规则,测试断言用
};

std::optional<LegacyConversion> ConvertLegacyToolRules(const json& tools) {
    if (!tools.is_object() || tools.contains("mode")) {
        return std::nullopt;  // 只收无 mode 键的旧写法;有 mode 走新规矩,不混读
    }
    LegacyConversion result;
    const bool has_allow = tools.contains("allow") && tools["allow"].is_array();
    const bool allow_nonempty = has_allow && !tools["allow"].empty();
    result.converted = json::object();
    if (allow_nonempty) {
        result.converted["mode"] = "only";
        result.converted["allow"] = tools["allow"];
        result.rule = "legacy-allow-nonempty->only";
    } else {
        result.converted["mode"] = "inherit";
        result.rule = "legacy-allow-empty->inherit";
    }
    if (tools.contains("deny") && tools["deny"].is_array()) {
        result.converted["deny"] = tools["deny"];
    }
    return result;
}

// ---------------------------------------------------------------------------
// 依赖解释(冻结合同 §3:解释不全的档不许采用)
// ---------------------------------------------------------------------------

struct DependencyExplanation {
    std::string tool;        // 触发解释的工具名(空=汇总行)
    std::string requirement; // 依赖内容
    bool satisfied;
};

std::vector<DependencyExplanation> ExplainDependencies(const json& profile) {
    std::vector<DependencyExplanation> out;
    if (!profile.is_object()) {
        out.push_back({"", "档不是对象", false});
        return out;
    }
    const bool mcp_allowed = [&] {
        if (!profile.contains("features") || !profile["features"].is_object()) {
            return false;
        }
        const json& features = profile["features"];
        const std::string def =
            features.contains("default") && features["default"].is_string()
                ? features["default"].get<std::string>() : "enabled";
        if (def == "enabled") {
            return true;  // 默认全开,名单只收窄
        }
        return features.contains("enabled") && features["enabled"].is_array() &&
               std::find(features["enabled"].begin(), features["enabled"].end(), "mcp") !=
                   features["enabled"].end();
    }();
    out.push_back({"", "features 放行 mcp", mcp_allowed});

    std::vector<std::string> mounted_servers;
    if (profile.contains("components") && profile["components"].is_object() &&
        profile["components"].contains("mcpServers") && profile["components"]["mcpServers"].is_array()) {
        for (const auto& server : profile["components"]["mcpServers"]) {
            if (server.is_string()) {
                mounted_servers.push_back(server.get<std::string>());
            }
        }
    }

    if (profile.contains("tools") && profile["tools"].is_object() &&
        profile["tools"].contains("allow") && profile["tools"]["allow"].is_array()) {
        for (const auto& tool : profile["tools"]["allow"]) {
            if (!tool.is_string()) {
                continue;
            }
            const std::string name = tool.get<std::string>();
            // canonical 文本形如 <origin>:<server>:<tool>;本阶段只解释
            // mcp: 前缀(内置/插件工具的 canonical 映射在 P2 与 ToolOrigin
            // 对账后补)。
            if (name.rfind("mcp:", 0) != 0) {
                continue;
            }
            const std::size_t first = name.find(':');
            const std::size_t second = name.find(':', first + 1);
            if (second == std::string::npos) {
                out.push_back({name, "canonical 名缺 server 段", false});
                continue;
            }
            const std::string server = name.substr(first + 1, second - first - 1);
            const bool mounted = std::find(mounted_servers.begin(), mounted_servers.end(), server) !=
                                 mounted_servers.end();
            out.push_back({name, "components.mcpServers 含 " + server, mounted});
        }
    }
    return out;
}

// features 名单里的键必须全在冻结名表内——"同一功能两枚不同键"在这里拦。
std::string CheckFeatureNames(const json& features) {
    if (!features.is_object()) {
        return "features 不是对象";
    }
    for (const char* key : {"enabled", "disabled"}) {
        if (!features.contains(key) || !features[key].is_array()) {
            continue;
        }
        for (const auto& name : features[key]) {
            if (!name.is_string() || !IsKnownFeature(name.get<std::string>())) {
                return std::string("features.") + key + " 含未知功能名: " +
                       (name.is_string() ? name.get<std::string>() : "<非串>");
            }
        }
    }
    return std::string();
}

// 暴露模式与工具面的互斥(冻结合同 §3):零工具配 deferred 是矛盾——
// 没有可延迟暴露的东西,发现器依赖解释不出来。
std::string CheckExposureAgainstTools(const json& profile) {
    if (!profile.is_object() || !profile.contains("tools") || !profile["tools"].is_object()) {
        return std::string();
    }
    const json& tools = profile["tools"];
    const bool mode_is_only = tools.contains("mode") && tools["mode"] == "only";
    const bool mode_is_none = tools.contains("mode") && tools["mode"] == "none";
    const bool empty_face = mode_is_none ||
                            (mode_is_only && tools.contains("allow") && tools["allow"].is_array() &&
                             tools["allow"].empty());
    const bool deferred = profile.contains("exposure") && profile["exposure"].is_object() &&
                          profile["exposure"].contains("default") &&
                          profile["exposure"]["default"] == "deferred";
    if (empty_face && deferred) {
        return "零工具面配 exposure=deferred:发现器依赖解释不出(冻结合同 §3)";
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// 取档与 golden
// ---------------------------------------------------------------------------

const json* FindProfile(const json& deployment, const std::string& name) {
    if (!deployment.is_object() || !deployment.contains("harnessProfiles") ||
        !deployment["harnessProfiles"].is_object() ||
        !deployment["harnessProfiles"].contains(name)) {
        return nullptr;
    }
    return &deployment["harnessProfiles"][name];
}

TEST_CASE("部署档 golden:两份样例都过机器 schema") {
    const auto schema = ReadJsonFile(kFixturesRoot / "deployment.schema.json");
    REQUIRE(schema.has_value());

    for (const char* golden : {"profile.minimal-tools.json", "profile.zero-tools.json"}) {
        CAPTURE(golden);
        const auto deployment = ReadJsonFile(kFixturesRoot / golden);
        REQUIRE(deployment.has_value());
        const std::string error =
            ValidateAgainst(*deployment, *schema, *schema, "<root>");
        CHECK(error.empty());
    }
}

TEST_CASE("部署档 golden:schemaVersion 只认 1,未知键拒绝") {
    const auto schema = ReadJsonFile(kFixturesRoot / "deployment.schema.json");
    REQUIRE(schema.has_value());

    const auto base = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
    REQUIRE(base.has_value());

    SUBCASE("schemaVersion=2 被拒") {
        json bad = *base;
        bad["schemaVersion"] = 2;
        const std::string error = ValidateAgainst(bad, *schema, *schema, "<root>");
        CHECK_FALSE(error.empty());
    }
    SUBCASE("档内未知键被拒,拼错的键不许静默生效") {
        json bad = *base;
        bad["harnessProfiles"]["zero-tools"]["toolz"] = json::object();
        const std::string error = ValidateAgainst(bad, *schema, *schema, "<root>");
        CHECK_FALSE(error.empty());
    }
    SUBCASE("tools.mode 未知值被拒,不落成全工具") {
        json bad = *base;
        bad["harnessProfiles"]["zero-tools"]["tools"]["mode"] = "everything";
        const std::string error = ValidateAgainst(bad, *schema, *schema, "<root>");
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("ToolPolicySpec:新 schema 的矛盾组合在语义层全拒") {
    SUBCASE("inherit 带非空 allow") {
        json policy = {{"mode", "inherit"}, {"allow", json::array({"read_file"})}};
        CHECK_FALSE(CheckToolPolicySemantics(policy).empty());
    }
    SUBCASE("only 缺 allow") {
        json policy = {{"mode", "only"}, {"deny", json::array()}};
        CHECK_FALSE(CheckToolPolicySemantics(policy).empty());
    }
    SUBCASE("none 带非空 allow") {
        json policy = {{"mode", "none"}, {"allow", json::array({"read_file"})}};
        CHECK_FALSE(CheckToolPolicySemantics(policy).empty());
    }
    SUBCASE("合法三式都过") {
        CHECK(CheckToolPolicySemantics({{"mode", "inherit"}}).empty());
        CHECK(CheckToolPolicySemantics({{"mode", "only"}, {"allow", json::array()}}).empty());
        CHECK(CheckToolPolicySemantics({{"mode", "none"}}).empty());
    }
}

TEST_CASE("旧 schema 换算:空 allow 永远是继承,不解释成 none") {
    SUBCASE("无 tools 段 → inherit") {
        const auto converted = ConvertLegacyToolRules(json::object());
        REQUIRE(converted.has_value());
        CHECK(converted->rule == "legacy-allow-empty->inherit");
        CHECK(converted->converted["mode"] == "inherit");
        CHECK(CheckToolPolicySemantics(converted->converted).empty());
    }
    SUBCASE("allow 空 → inherit") {
        const auto converted = ConvertLegacyToolRules(json{{"allow", json::array()}});
        REQUIRE(converted.has_value());
        CHECK(converted->rule == "legacy-allow-empty->inherit");
        CHECK(converted->converted["mode"] == "inherit");
    }
    SUBCASE("allow 非空 → only,名单原样") {
        const auto converted =
            ConvertLegacyToolRules(json{{"allow", json::array({"read_file", "search"})}});
        REQUIRE(converted.has_value());
        CHECK(converted->rule == "legacy-allow-nonempty->only");
        CHECK(converted->converted["mode"] == "only");
        CHECK(converted->converted["allow"].size() == 2);
        CHECK(CheckToolPolicySemantics(converted->converted).empty());
    }
    SUBCASE("带 mode 键的不是旧写法,换算器不收——新旧不混读") {
        CHECK_FALSE(ConvertLegacyToolRules(json{{"mode", "only"}}).has_value());
    }
    SUBCASE("旧写法过新 schema 会被 required(mode) 拒——不混读机器可验") {
        const auto schema = ReadJsonFile(kFixturesRoot / "deployment.schema.json");
        REQUIRE(schema.has_value());
        REQUIRE(schema->contains("definitions"));
        REQUIRE((*schema)["definitions"].contains("toolPolicy"));
        const json& tool_policy_schema = (*schema)["definitions"]["toolPolicy"];
        // 旧写法直接塞进 toolPolicy 定义:required mode 不满足。
        const std::string legacy_error = ValidateAgainst(
            json{{"allow", json::array()}}, tool_policy_schema, *schema, "<tools>");
        CHECK_FALSE(legacy_error.empty());
        // 换算结果再过 schema:过。
        const std::string converted_error = ValidateAgainst(
            json{{"mode", "inherit"}}, tool_policy_schema, *schema, "<tools>");
        CHECK(converted_error.empty());
    }
}

TEST_CASE("依赖解释:minimal-tools 的两只工具都解释得出来") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
    REQUIRE(deployment.has_value());
    const json* profile = FindProfile(*deployment, "minimal-tools");
    REQUIRE(profile != nullptr);

    const auto explanations = ExplainDependencies(*profile);
    bool mcp_ok = false;
    int tool_rows = 0;
    int tool_rows_ok = 0;
    for (const auto& row : explanations) {
        if (row.tool.empty()) {
            mcp_ok = row.satisfied;
            continue;
        }
        ++tool_rows;
        if (row.satisfied) {
            ++tool_rows_ok;
        }
    }
    CHECK(mcp_ok);           // features.enabled 含 mcp(default=disabled 被名单翻转)
    CHECK(tool_rows == 2);   // echo + describe 两只
    CHECK(tool_rows_ok == 2);

    CHECK(CheckFeatureNames((*profile)["features"]).empty());
    CHECK(CheckExposureAgainstTools(*profile).empty());
}

TEST_CASE("依赖解释:zero-tools 零启动依赖,零工具面成立") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
    REQUIRE(deployment.has_value());
    const json* profile = FindProfile(*deployment, "zero-tools");
    REQUIRE(profile != nullptr);

    REQUIRE(profile->contains("tools"));
    CHECK((*profile)["tools"]["mode"] == "none");
    CHECK(CheckToolPolicySemantics((*profile)["tools"]).empty());

    // 零工具档不挂发现器、不启动 MCP:components 空,解释里没有任何
    // 工具依赖行(§2.3 无工具会话)。
    const auto explanations = ExplainDependencies(*profile);
    int tool_rows = 0;
    for (const auto& row : explanations) {
        if (!row.tool.empty()) {
            ++tool_rows;
        }
    }
    CHECK(tool_rows == 0);
    CHECK(CheckExposureAgainstTools(*profile).empty());
}

TEST_CASE("依赖解释:点名未放行的组件是配置错误,解释不全不许采用") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
    REQUIRE(deployment.has_value());
    const json* profile = FindProfile(*deployment, "minimal-tools");
    REQUIRE(profile != nullptr);

    SUBCASE("allow 点名未挂载的 server") {
        json bad = *profile;
        bad["tools"]["allow"].push_back("mcp:tools-unapproved:secret");
        const auto explanations = ExplainDependencies(bad);
        bool secret_row = false;
        bool secret_ok = false;
        for (const auto& row : explanations) {
            if (row.tool == "mcp:tools-unapproved:secret") {
                secret_row = true;
                secret_ok = row.satisfied;
            }
        }
        CHECK(secret_row);
        CHECK_FALSE(secret_ok);  // components 里没有它——解释失败,档不许采用
    }
    SUBCASE("features 禁了 mcp,工具依赖跟着解释失败") {
        json bad = *profile;
        bad["features"]["enabled"] = json::array();  // default=disabled 且名单空
        const auto explanations = ExplainDependencies(bad);
        bool mcp_ok = true;
        for (const auto& row : explanations) {
            if (row.tool.empty()) {
                mcp_ok = row.satisfied;
            }
        }
        CHECK_FALSE(mcp_ok);
    }
    SUBCASE("features 名单出现表外键被拒") {
        json bad = *profile;
        bad["features"]["disabled"].push_back("fs-write");
        CHECK_FALSE(CheckFeatureNames(bad["features"]).empty());
    }
    SUBCASE("零工具面配 deferred 被拒") {
        json bad = *profile;
        bad["tools"] = json{{"mode", "none"}};
        bad["exposure"]["default"] = "deferred";
        CHECK_FALSE(CheckExposureAgainstTools(bad).empty());
    }
}

TEST_CASE("协议 2.0 golden:信封、配对与反向 id 空间自洽") {
    const auto golden = ReadJsonFile(kFixturesRoot / "appserver-v2.messages.golden.json");
    REQUIRE(golden.has_value());
    REQUIRE(golden->contains("clientRequests"));
    REQUIRE(golden->contains("clientResponses"));
    REQUIRE(golden->contains("serverRequests"));
    REQUIRE(golden->contains("executorResponses"));

    // 出站/入站一律标准 2.0 信封:jsonrpc 字段必须在、值必须是 "2.0"。
    // 这与 1.2 冻结的"出站不带 jsonrpc"是两版的显式差别,不混读。
    for (const auto& section : {"clientRequests", "clientResponses", "serverRequests", "executorResponses"}) {
        for (const auto& message : (*golden)[section]) {
            REQUIRE(message.contains("jsonrpc"));
            CHECK(message["jsonrpc"] == "2.0");
        }
    }

    // 请求必须有 method;响应必须回配请求 id。
    for (const auto& request : (*golden)["clientRequests"]) {
        CHECK(request.contains("method"));
        CHECK(request.contains("id"));
    }
    for (const auto& response : (*golden)["clientResponses"]) {
        REQUIRE(response.contains("id"));
        bool paired = false;
        for (const auto& request : (*golden)["clientRequests"]) {
            if (request.contains("id") && request["id"] == response["id"]) {
                paired = true;
                break;
            }
        }
        CHECK(paired);
    }

    // 反向请求(引擎→执行器)id 用独立空间:srv-req- 前缀,不与客户端
    // 数字 id 混用;transport id 只配对消息,不当执行身份。
    for (const auto& request : (*golden)["serverRequests"]) {
        REQUIRE(request.contains("id"));
        CHECK(request["id"].is_string());
        const std::string id = request["id"].get<std::string>();
        CHECK(id.rfind("srv-req-", 0) == 0);
        // 执行身份另有一整套字段,不许拿 transport id 充数(合同 §11)。
        REQUIRE(request.contains("params"));
        const json& params = request["params"];
        for (const char* key : {"sessionId", "turnId", "actionId", "toolCallId", "executionId",
                                "attemptId", "registrationId", "executorId", "ownerEpoch",
                                "argumentsDigest", "deadlineMs", "authorizationRef"}) {
            CAPTURE(key);
            CHECK(params.contains(key));
        }
    }

    // 执行器对反向请求的响应回配同一 id;结果提交是通知(无 id),按
    // executionId 关联,不靠 transport id。
    for (const auto& response : (*golden)["executorResponses"]) {
        if (response.contains("id")) {
            bool paired = false;
            for (const auto& request : (*golden)["serverRequests"]) {
                if (request.contains("id") && request["id"] == response["id"]) {
                    paired = true;
                    break;
                }
            }
            CHECK(paired);
        } else {
            CHECK(response.contains("method"));
            CHECK(response["method"] == "toolExecution/result");
            REQUIRE(response.contains("params"));
            CHECK(response["params"].contains("executionId"));
        }
    }

    // tools/register 的受理回执区分"目录已受理"与"会话已采用":首版
    // 只有 pending_next_session,不谎报 adopted。
    bool register_pending = false;
    for (const auto& response : (*golden)["clientResponses"]) {
        if (response.contains("id") && response["id"] == 42 && response.contains("result") &&
            response["result"].contains("status")) {
            register_pending = response["result"]["status"] == "pending_next_session";
        }
    }
    CHECK(register_pending);
}

TEST_CASE("协议 2.0 golden:客户端请求都带 clientOperationId,幂等键不落在 transport id 上") {
    const auto golden = ReadJsonFile(kFixturesRoot / "appserver-v2.messages.golden.json");
    REQUIRE(golden.has_value());
    for (const auto& request : (*golden)["clientRequests"]) {
        REQUIRE(request.contains("method"));
        REQUIRE(request.contains("params"));
        CHECK(request["params"].contains("clientOperationId"));
    }
}
