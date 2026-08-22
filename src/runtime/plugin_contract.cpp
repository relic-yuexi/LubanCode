// 冻结合同的实现:manifest 强校验、${plugin_dir} 结构化替换、进程协议
// 帧的序列化/解析、入参 Schema 子集校验。全部纯函数,不碰进程、不碰
// Lua、不碰 DLL——那是 runtime 实现层的事。
#include "runtime/plugin_contract.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::runtime {

namespace {

// id/name/version/language 的代码点规矩:ASCII 字母/数字/下划线/连字符,
// 首字符须字母或数字(不许 _- 开头,防误当标志位)。
bool CodePointAllowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// 从 JSON 拿一个非空字符串字段;拿不到/不是串/空串都报人话。manifest 是
// 合同,缺项即坏,不悄悄给默认值。
std::expected<std::string, std::string> RequireString(const nlohmann::json& root, const char* key) {
    const auto it = root.find(key);
    if (it == root.end()) {
        return std::unexpected(std::string("缺字段 ") + key);
    }
    if (!it->is_string()) {
        return std::unexpected(std::string(key) + " 字段必须是字符串");
    }
    const std::string value = it->get<std::string>();
    if (value.empty()) {
        return std::unexpected(std::string(key) + " 字段是空串");
    }
    return value;
}

// 拿可选字符串字段;没写返回 nullopt,写了但不是字符串 = 坏。
std::expected<std::optional<std::string>, std::string> OptionalString(const nlohmann::json& root, const char* key) {
    const auto it = root.find(key);
    if (it == root.end() || it->is_null()) {
        return std::optional<std::string>{};
    }
    if (!it->is_string()) {
        return std::unexpected(std::string(key) + " 字段必须是字符串");
    }
    return it->get<std::string>();
}

// 入参 schema 的静态强校验(manifest 加载期跑一遍;声明怪也拒)。
// 校验的是"这份 schema 是不是我们认得的子集、形状对不对",不是拿它去
// 验任何输入。递归,深度上限防作者写出千层嵌套。
std::optional<std::string> ValidateSchemaShape(const nlohmann::json& schema, int depth) {
    if (depth > 32) {
        return "schema 嵌套超过 32 层";
    }
    if (!schema.is_object()) {
        return "schema 必须是 object";
    }
    // type:认得的七种之一,或者 type 数组(v1 不认——JSON Schema 允许,我们
    // 的验证子集不实现联合类型,写了就明说)。
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        if (t.is_string()) {
            const std::string type_name = t.get<std::string>();
            if (type_name != "object" && type_name != "array" && type_name != "string" && type_name != "number" &&
                type_name != "integer" && type_name != "boolean" && type_name != "null") {
                return "type 不在认得的集合里: " + type_name;
            }
        } else {
            return "type 必须是单个字符串(联合类型 v1 不认)";
        }
    }
    if (schema.contains("properties") && !schema["properties"].is_object()) {
        return "properties 必须是 object";
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) {
            return "required 必须是数组";
        }
        for (const auto& key : schema["required"]) {
            if (!key.is_string()) {
                return "required 数组里的元素必须是字符串";
            }
        }
    }
    if (schema.contains("enum") && !schema["enum"].is_array()) {
        return "enum 必须是数组";
    }
    if (schema.contains("items")) {
        const auto& items = schema["items"];
        if (!items.is_object()) {
            return "items 必须是 object(联合 items v1 不认)";
        }
        if (const auto inner = ValidateSchemaShape(items, depth + 1); inner.has_value()) {
            return "items." + *inner;
        }
    }
    if (schema.contains("additionalProperties") && !schema["additionalProperties"].is_boolean() &&
        !schema["additionalProperties"].is_object()) {
        return "additionalProperties 必须是布尔或 object";
    }
    // 数值/长度界:得是数字(或长度得是非负整数)。
    for (const char* numeric_key : {"minimum", "maximum"}) {
        if (schema.contains(numeric_key) && !schema[numeric_key].is_number()) {
            return std::string(numeric_key) + " 必须是数字";
        }
    }
    for (const char* length_key : {"minLength", "maxLength", "minItems", "maxItems"}) {
        if (schema.contains(length_key)) {
            const auto& v = schema[length_key];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0) {
                return std::string(length_key) + " 必须是非负整数";
            }
        }
    }
    // 递归 properties。
    if (schema.contains("properties")) {
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            if (const auto inner = ValidateSchemaShape(it.value(), depth + 1); inner.has_value()) {
                return "properties." + it.key() + ": " + *inner;
            }
        }
    }
    if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object()) {
        if (const auto inner = ValidateSchemaShape(schema["additionalProperties"], depth + 1); inner.has_value()) {
            return "additionalProperties: " + *inner;
        }
    }
    return std::nullopt;
}

// 路径 b 是否在目录 a 之内(含 a 本身)。两边都先 canonical 的事由调用方
// 做;这里只做词法前缀比较(分界必须是目录分隔符,不许 /foo/bar 匹配
// /foo/barbar)。
bool PathIsInside(const std::filesystem::path& dir, const std::filesystem::path& candidate) {
    auto dir_it = dir.begin();
    auto cand_it = candidate.begin();
    for (; dir_it != dir.end(); ++dir_it, ++cand_it) {
        if (cand_it == candidate.end() || *dir_it != *cand_it) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool IsValidPluginIdentifier(std::string_view value, std::size_t max_len) {
    if (value.empty() || value.size() > max_len) {
        return false;
    }
    const char first = value.front();
    if (!std::isalnum(static_cast<unsigned char>(first))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) { return CodePointAllowed(c); });
}

// version 的规矩:id 之外再收 '.'(语义版本 1.0.0 是常态);同样不许
// shell 元字符、不许非 ASCII。
bool IsValidPluginVersion(std::string_view value, std::size_t max_len) {
    if (value.empty() || value.size() > max_len) {
        return false;
    }
    const char first = value.front();
    if (!std::isalnum(static_cast<unsigned char>(first))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](char c) { return CodePointAllowed(c) || c == '.'; });
}

std::string BuildPluginToolName(std::string_view plugin_id, std::string_view tool_name) {
    std::string out;
    out.reserve(plugin_id.size() + tool_name.size() + 11);
    out += "plugin__";
    out += plugin_id;
    out += "__";
    out += tool_name;
    return out;
}

std::expected<std::string, std::string> ExpandPluginDirPlaceholder(std::string_view text,
                                                                   const std::filesystem::path& plugin_dir) {
    constexpr std::string_view kPlaceholder = "${plugin_dir}";
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t hit = text.find(kPlaceholder, pos);
        if (hit == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, hit - pos));
        // 目录的 UTF-8 写法走 u8 通道(GBK 机器上 path 窄口是 ACP,
        // .string() 会抛/坏字节,见 paths.hpp 的规矩)。
        const std::u8string u8 = plugin_dir.u8string();
        out.append(reinterpret_cast<const char*>(u8.data()), u8.size());
        pos = hit + kPlaceholder.size();
    }
    return out;
}

std::expected<PluginManifest, std::string> ParsePluginManifest(const std::string& manifest_json,
                                                               const std::filesystem::path& plugin_dir) {
    // 解析失败即坏,不宽化。
    nlohmann::json root = nlohmann::json::parse(manifest_json, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return std::unexpected("plugin.json 不是合法 JSON");
    }
    if (!root.is_object()) {
        return std::unexpected("plugin.json 顶层必须是 object");
    }

    // manifest_version:只认 1。
    const auto version_value = root.find("manifest_version");
    if (version_value == root.end() || !version_value->is_number_integer()) {
        return std::unexpected("缺 manifest_version 或不是整数");
    }
    if (version_value->get<int>() != kPluginManifestVersion) {
        return std::unexpected("manifest_version=" + version_value->dump() + " 宿主只认 " +
                               std::to_string(kPluginManifestVersion) + ",不静默宽化");
    }

    // id/version:字符集与长度规矩。
    auto id = RequireString(root, "id");
    if (!id.has_value()) {
        return std::unexpected(id.error());
    }
    if (!IsValidPluginIdentifier(*id, 64)) {
        return std::unexpected("id 只能是字母数字_-、字母数字开头、至多 64 字符: " + *id);
    }
    auto version = RequireString(root, "version");
    if (!version.has_value()) {
        return std::unexpected(version.error());
    }
    if (!IsValidPluginVersion(*version, 32)) {
        return std::unexpected("version 只能是字母数字与 ._- 字符: " + *version);
    }
    // language:可选;只作诊断/展示,删掉也能跑。写了就查一下形状,防 typo
    // 悄悄变成"未知语言"。
    auto language = OptionalString(root, "language");
    if (!language.has_value()) {
        return std::unexpected(language.error());
    }
    if (language->has_value() && !IsValidPluginIdentifier(**language, 32)) {
        return std::unexpected("language 只能是字母数字_-: " + **language);
    }

    // runtime 段。
    const auto runtime_it = root.find("runtime");
    if (runtime_it == root.end() || !runtime_it->is_object()) {
        return std::unexpected("缺 runtime 段或不是 object");
    }
    const auto& runtime_json = *runtime_it;
    auto kind_text = RequireString(runtime_json, "kind");
    if (!kind_text.has_value()) {
        return std::unexpected(kind_text.error());
    }
    RuntimeKind kind;
    if (*kind_text == "process") {
        kind = RuntimeKind::Process;
    } else if (*kind_text == "embedded-lua") {
        kind = RuntimeKind::EmbeddedLua;
    } else if (*kind_text == "native-library") {
        return std::unexpected("runtime.kind=native-library 属后续批次,当前宿主不支持,整件拒绝");
    } else {
        return std::unexpected("runtime.kind 不认得: " + *kind_text);
    }

    PluginManifest manifest;
    manifest.id = std::move(*id);
    manifest.version = std::move(*version);
    manifest.language = language->value_or("");
    manifest.kind = kind;

    // 插件目录:canonical(不存在/不可达时按 weakly_canonical 的尽力而为
    // 口径,拿词法绝对路径比一比——目录是宿主自己枚举出来的,枚举得到就
    // 存在;真不存在的极端情况留给路径校验去拦)。
    std::error_code ec;
    std::filesystem::path canonical_dir = std::filesystem::weakly_canonical(plugin_dir, ec);
    if (ec || canonical_dir.empty()) {
        canonical_dir = std::filesystem::absolute(plugin_dir, ec);
        if (ec) {
            return std::unexpected("插件目录路径解析失败: " + platform::PathToUtf8(plugin_dir));
        }
    }
    manifest.plugin_dir = canonical_dir;

    if (kind == RuntimeKind::Process) {
        auto command = RequireString(runtime_json, "command");
        if (!command.has_value()) {
            return std::unexpected(command.error());
        }
        if (command->find("${plugin_dir}") != std::string::npos) {
            // command 里也允许 ${plugin_dir}(自带给全路径的可执行文件是正路),
            // canonical 校验在替换后统一做。
        }
        auto expanded_command = ExpandPluginDirPlaceholder(*command, manifest.plugin_dir);
        if (!expanded_command.has_value()) {
            return std::unexpected(expanded_command.error());
        }
        if (expanded_command->empty()) {
            return std::unexpected("runtime.command 替换后是空串");
        }
        manifest.argv.push_back(std::move(*expanded_command));

        const auto args_it = runtime_json.find("args");
        if (args_it != runtime_json.end()) {
            if (!args_it->is_array()) {
                return std::unexpected("runtime.args 必须是数组");
            }
            for (const auto& arg : *args_it) {
                if (!arg.is_string()) {
                    return std::unexpected("runtime.args 的元素必须是字符串(不经 shell,原样直传)");
                }
                auto expanded = ExpandPluginDirPlaceholder(arg.get<std::string>(), manifest.plugin_dir);
                if (!expanded.has_value()) {
                    return std::unexpected(expanded.error());
                }
                manifest.argv.push_back(std::move(*expanded));
            }
        }

        // timeout_ms:可选,缺省 30s;0 是合法值(显式不设墙,不推荐)。
        const auto timeout_it = runtime_json.find("timeout_ms");
        if (timeout_it != runtime_json.end() && !timeout_it->is_null()) {
            if (!timeout_it->is_number_integer() || timeout_it->get<std::int64_t>() < 0) {
                return std::unexpected("runtime.timeout_ms 必须是非负整数");
            }
            manifest.timeout_ms = static_cast<int>(timeout_it->get<std::int64_t>());
        }

        // 路径校验:凡带 ${plugin_dir} 的段,canonical 后须仍位于插件目录内。
        // 绝对路径/相对可执行名(如 "python3",走 PATH)不在此列——那是
        // 明确批准的外部 executable 路子。这里复查原始声明,只有用了
        // 占位符的才圈在目录里。
        const auto check_arg = [&](const std::string& raw, const std::string& expanded) -> std::optional<std::string> {
            if (raw.find("${plugin_dir}") == std::string::npos) {
                return std::nullopt;
            }
            std::error_code path_ec;
            const std::filesystem::path expanded_path =
                std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(
                                                      reinterpret_cast<const char8_t*>(expanded.data()),
                                                      expanded.size())),
                                                  path_ec);
            if (path_ec) {
                return "路径解析失败: " + expanded;
            }
            if (!PathIsInside(manifest.plugin_dir, expanded_path)) {
                return "${plugin_dir} 替换后逃出了插件目录: " + expanded;
            }
            return std::nullopt;
        };
        if (auto problem = check_arg(*command, manifest.argv[0]); problem.has_value()) {
            return std::unexpected(*problem);
        }
        if (args_it != runtime_json.end()) {
            for (std::size_t i = 0; i < args_it->size(); ++i) {
                const std::string raw = (*args_it)[i].get<std::string>();
                if (auto problem = check_arg(raw, manifest.argv[i + 1]); problem.has_value()) {
                    return std::unexpected(*problem);
                }
            }
        }
    }

    // permissions 段(v1 只记账)。
    const auto perms_it = root.find("permissions");
    if (perms_it != root.end() && !perms_it->is_null()) {
        if (!perms_it->is_object()) {
            return std::unexpected("permissions 必须是 object");
        }
        const auto network_it = perms_it->find("network");
        if (network_it != perms_it->end() && !network_it->is_null()) {
            if (!network_it->is_boolean()) {
                return std::unexpected("permissions.network 必须是布尔");
            }
            manifest.network_allowed = network_it->get<bool>();
        }
        const auto env_it = perms_it->find("env");
        if (env_it != perms_it->end() && !env_it->is_null()) {
            if (!env_it->is_array()) {
                return std::unexpected("permissions.env 必须是数组");
            }
            for (const auto& name : *env_it) {
                if (!name.is_string() || name.get<std::string>().empty()) {
                    return std::unexpected("permissions.env 的元素必须是非空字符串");
                }
                manifest.env_allowlist.push_back(name.get<std::string>());
            }
        }
    }

    // tools 段:至少一件;逐件强校验;同插件内重名即拒。
    const auto tools_it = root.find("tools");
    if (tools_it == root.end() || !tools_it->is_array() || tools_it->empty()) {
        return std::unexpected("tools 必须是非空数组(插件至少声明一件工具)");
    }
    std::set<std::string> seen_names;
    for (std::size_t i = 0; i < tools_it->size(); ++i) {
        const auto& tool_json = (*tools_it)[i];
        if (!tool_json.is_object()) {
            return std::unexpected("tools[" + std::to_string(i) + "] 必须是 object");
        }
        auto name = RequireString(tool_json, "name");
        if (!name.has_value()) {
            return std::unexpected("tools[" + std::to_string(i) + "]: " + name.error());
        }
        if (!IsValidPluginIdentifier(*name, 64)) {
            return std::unexpected("tools[" + std::to_string(i) + "].name 只能是字母数字_-、字母数字开头、"
                                   "至多 64 字符: " +
                                   *name);
        }
        if (seen_names.count(*name) != 0) {
            return std::unexpected("同一插件里工具重名: " + *name);
        }
        seen_names.insert(*name);

        auto description = OptionalString(tool_json, "description");
        if (!description.has_value()) {
            return std::unexpected("tools[" + std::to_string(i) + "]: " + description.error());
        }
        auto entry = OptionalString(tool_json, "entry");
        if (!entry.has_value()) {
            return std::unexpected("tools[" + std::to_string(i) + "]: " + entry.error());
        }
        // entry 缺省 = 工具短名(单脚本一件工具时省得抄两遍)。
        std::string entry_value = entry->value_or(*name);
        if (!IsValidPluginIdentifier(entry_value, 64)) {
            return std::unexpected("tools[" + std::to_string(i) + "].entry 只能是字母数字_-: " + entry_value);
        }

        // input_schema:必填、必须是 object、必须是认得的子集形状。
        // Schema 坏了拒绝整件插件——这是与 DLL 路径"退化宽 object"相反的
        // 刻意选择(单子「Schema 的方向不能倒」)。
        const auto schema_it = tool_json.find("input_schema");
        if (schema_it == tool_json.end() || !schema_it->is_object()) {
            return std::unexpected("tools[" + std::to_string(i) + "].input_schema 缺失或不是 object"
                                   "(Schema 坏了拒绝加载,不悄悄宽化)");
        }
        if (auto shape_error = ValidateSchemaShape(*schema_it, 0); shape_error.has_value()) {
            return std::unexpected("tools[" + std::to_string(i) + "].input_schema 形状不认得: " + *shape_error);
        }

        PluginDefinition def;
        def.plugin_id = manifest.id;
        def.name = *name;
        def.full_name = BuildPluginToolName(manifest.id, *name);
        def.description = description->value_or("");
        def.entry = std::move(entry_value);
        def.input_schema = *schema_it;
        manifest.tools.push_back(std::move(def));
    }

    return manifest;
}

// ---------------------------------------------------------------------------
// 进程协议帧
// ---------------------------------------------------------------------------

namespace plugin_protocol {

nlohmann::json SerializeRequest(const ProcessRequest& request) {
    nlohmann::json context = nlohmann::json::object();
    if (!request.context_cwd.empty()) {
        context["cwd"] = request.context_cwd;
    }
    return nlohmann::json{
        {"protocol", request.protocol},
        {"call_id", request.call_id},
        {"plugin", request.plugin},
        {"tool", request.tool},
        {"entry", request.entry},
        {"arguments", request.arguments},
        {"context", std::move(context)},
    };
}

ParsedResponse ParseResponse(std::string_view stdout_bytes, std::string_view expected_call_id) {
    ParsedResponse out;
    // 第一关:合法 UTF-8。不做编码猜测(GBK 转一把那种事是 hooks 的明示
    // 解码层的事),协议线就是 UTF-8,坏了即坏。
    const std::string bytes(stdout_bytes);
    if (!platform::IsValidUtf8(bytes)) {
        out.status = PluginErrorCode::BadUtf8;
        out.detail = "stdout 不是合法 UTF-8";
        return out;
    }
    // 第二关:恰好一份合法 JSON,整体 parse。前后混日志在这里就露馅——
    // 不从字堆里猜最后一枚 JSON。
    nlohmann::json root = nlohmann::json::parse(bytes, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "stdout 不是恰好一份合法 JSON object(混日志/多帧/坏文法都算协议错)";
        return out;
    }
    // 第三关:call_id 对得上。
    const auto call_it = root.find("call_id");
    if (call_it == root.end() || !call_it->is_string()) {
        out.status = PluginErrorCode::CallIdMismatch;
        out.detail = "响应缺 call_id 或不是字符串";
        return out;
    }
    const std::string got_call_id = call_it->get<std::string>();
    if (got_call_id != expected_call_id) {
        out.status = PluginErrorCode::CallIdMismatch;
        out.detail = "响应 call_id 对不上(要 " + std::string(expected_call_id) + ",来的是 " + got_call_id + ")";
        return out;
    }
    // protocol:认 1;不认的明说(后续版本协商是以后的批次)。
    const auto protocol_it = root.find("protocol");
    if (protocol_it == root.end() || !protocol_it->is_number_integer() ||
        protocol_it->get<int>() != kProtocolVersion) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "响应 protocol 缺失或不是 1";
        return out;
    }
    // ok:必填布尔。
    const auto ok_it = root.find("ok");
    if (ok_it == root.end() || !ok_it->is_boolean()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "响应缺 ok 或不是布尔";
        return out;
    }
    out.response.protocol = protocol_it->get<int>();
    out.response.call_id = got_call_id;
    out.response.ok = ok_it->get<bool>();

    if (!out.response.ok) {
        const auto error_it = root.find("error");
        if (error_it != root.end() && error_it->is_object()) {
            const auto code_it = error_it->find("code");
            if (code_it != error_it->end() && code_it->is_string()) {
                out.response.error_code = code_it->get<std::string>();
            }
            const auto message_it = error_it->find("message");
            if (message_it != error_it->end() && message_it->is_string()) {
                out.response.error_message = message_it->get<std::string>();
            }
        }
        if (out.response.error_message.empty()) {
            out.response.error_message = "(插件没给错误说明)";
        }
        return out;  // status 保持 Ok:插件自报失败是合法响应,分型在调用方
    }

    // content:必填数组;v1 只认 type=text,别的不静默转字符串。
    const auto content_it = root.find("content");
    if (content_it == root.end() || !content_it->is_array()) {
        out.status = PluginErrorCode::BadJson;
        out.detail = "ok=true 的响应缺 content 或 content 不是数组";
        return out;
    }
    std::string text;
    for (const auto& item : *content_it) {
        if (!item.is_object()) {
            out.status = PluginErrorCode::BadJson;
            out.detail = "content 的元素必须是 object";
            return out;
        }
        const auto type_it = item.find("type");
        if (type_it == item.end() || !type_it->is_string()) {
            out.status = PluginErrorCode::BadJson;
            out.detail = "content 的元素缺 type";
            return out;
        }
        const std::string type_name = type_it->get<std::string>();
        if (type_name != "text") {
            out.status = PluginErrorCode::UnknownContent;
            out.detail = "content type 不认得(v1 只认 text): " + type_name;
            return out;
        }
        const auto text_it = item.find("text");
        if (text_it == item.end() || !text_it->is_string()) {
            out.status = PluginErrorCode::BadJson;
            out.detail = "type=text 的元素缺 text 或不是字符串";
            return out;
        }
        if (!text.empty()) {
            text += "\n";
        }
        text += text_it->get<std::string>();
    }
    out.response.text = std::move(text);
    // structured:可选,原样收。
    const auto structured_it = root.find("structured");
    if (structured_it != root.end() && !structured_it->is_null()) {
        out.response.structured = *structured_it;
    }
    return out;
}

}  // namespace plugin_protocol

// ---------------------------------------------------------------------------
// 入参校验(调用前统一跑的子集)
// ---------------------------------------------------------------------------

namespace {

bool JsonTypeMatches(const nlohmann::json& value, const std::string& expected) {
    if (expected == "string") {
        return value.is_string();
    }
    if (expected == "number") {
        return value.is_number();
    }
    if (expected == "integer") {
        return value.is_number_integer();
    }
    if (expected == "boolean") {
        return value.is_boolean();
    }
    if (expected == "array") {
        return value.is_array();
    }
    if (expected == "object") {
        return value.is_object();
    }
    if (expected == "null") {
        return value.is_null();
    }
    return true;
}

// UTF-8 码点计数(长度界按码点算,与 JSON Schema 语义一致)。入参在协议
// 层已保证合法 UTF-8,这里只数不验。
std::size_t CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::optional<std::string> ValidateValueAgainstSchema(const nlohmann::json& value, const nlohmann::json& schema,
                                                      const std::string& path, int depth) {
    if (depth > 32) {
        return path + ": 嵌套超过 32 层";
    }
    if (!schema.is_object()) {
        return std::nullopt;  // 没给 schema 的位置无从校验
    }
    if (schema.contains("type") && schema["type"].is_string()) {
        const std::string expected = schema["type"].get<std::string>();
        if (!JsonTypeMatches(value, expected)) {
            return path + " 的类型应是 " + expected;
        }
    }
    if (schema.contains("const")) {
        if (value != schema["const"]) {
            return path + " 必须恒等于 " + schema["const"].dump();
        }
    }
    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool in_enum = false;
        for (const auto& allowed : schema["enum"]) {
            if (value == allowed) {
                in_enum = true;
                break;
            }
        }
        if (!in_enum) {
            return path + " 的取值不在枚举表里";
        }
    }
    // 数值界。
    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number() &&
            number < schema["minimum"].get<double>()) {
            return path + " 小于最小值 " + schema["minimum"].dump();
        }
        if (schema.contains("maximum") && schema["maximum"].is_number() &&
            number > schema["maximum"].get<double>()) {
            return path + " 大于最大值 " + schema["maximum"].dump();
        }
    }
    // 字符串长度界(码点)。
    if (value.is_string()) {
        const std::size_t length = CodePointCount(value.get_ref<const std::string&>());
        if (schema.contains("minLength") && schema["minLength"].is_number_integer() &&
            length < static_cast<std::size_t>(schema["minLength"].get<std::int64_t>())) {
            return path + " 短于 minLength " + schema["minLength"].dump();
        }
        if (schema.contains("maxLength") && schema["maxLength"].is_number_integer() &&
            length > static_cast<std::size_t>(schema["maxLength"].get<std::int64_t>())) {
            return path + " 长于 maxLength " + schema["maxLength"].dump();
        }
    }
    // 数组:items 逐项 + minItems/maxItems。
    if (value.is_array()) {
        const std::size_t size = value.size();
        if (schema.contains("minItems") && schema["minItems"].is_number_integer() &&
            size < static_cast<std::size_t>(schema["minItems"].get<std::int64_t>())) {
            return path + " 的元素个数少于 minItems " + schema["minItems"].dump();
        }
        if (schema.contains("maxItems") && schema["maxItems"].is_number_integer() &&
            size > static_cast<std::size_t>(schema["maxItems"].get<std::int64_t>())) {
            return path + " 的元素个数多于 maxItems " + schema["maxItems"].dump();
        }
        if (schema.contains("items") && schema["items"].is_object()) {
            for (std::size_t i = 0; i < size; ++i) {
                if (auto problem = ValidateValueAgainstSchema(value[i], schema["items"],
                                                               path + "[" + std::to_string(i) + "]", depth + 1);
                    problem.has_value()) {
                    return problem;
                }
            }
        }
    }
    // 对象:required + properties 逐键 + additionalProperties。
    if (value.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& key : schema["required"]) {
                if (key.is_string() && !value.contains(key.get<std::string>())) {
                    return path + " 缺少必填字段: " + key.get<std::string>();
                }
            }
        }
        const bool has_properties = schema.contains("properties") && schema["properties"].is_object();
        const bool ap_false = schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() &&
                              !schema["additionalProperties"].get<bool>();
        const bool ap_schema = schema.contains("additionalProperties") && schema["additionalProperties"].is_object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child_path = path.empty() ? it.key() : path + "." + it.key();
            if (has_properties) {
                const auto prop = schema["properties"].find(it.key());
                if (prop != schema["properties"].end()) {
                    if (auto problem =
                            ValidateValueAgainstSchema(it.value(), prop.value(), child_path, depth + 1);
                        problem.has_value()) {
                        return problem;
                    }
                    continue;
                }
            }
            if (ap_false) {
                return child_path + " 不在声明的字段里(additionalProperties=false)";
            }
            if (ap_schema) {
                if (auto problem = ValidateValueAgainstSchema(it.value(), schema["additionalProperties"], child_path,
                                                               depth + 1);
                    problem.has_value()) {
                    return problem;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> ValidateArgumentsAgainstSchema(const nlohmann::json& input, const nlohmann::json& schema) {
    if (!schema.is_object()) {
        return std::nullopt;  // 没有有效 schema,无从校验(实现层仍须防御)
    }
    // 顶层通常声明 type=object;没声明也按对象验 required/properties——
    // 模型入参天然是 object。
    if (!input.is_object()) {
        return "入参必须是 object";
    }
    return ValidateValueAgainstSchema(input, schema, "", 0);
}

}  // namespace lubancode::runtime
