// channel.yaml 严格 parser 的实现。字段规矩逐条对照
// docs/architecture/channels/channel-manifest.md;顶层与各子节都是"未知
// 字段报错"的封闭集合,取值集(capabilities 六项数组)冻结,不接受表外
// 字符串。
#include "channel/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include <yaml-cpp/yaml.h>

#include "platform/paths.hpp"

namespace lubancode::channel {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

int LineOf(const YAML::Node& node) {
    const YAML::Mark mark = node.Mark();
    return mark.line >= 0 ? mark.line + 1 : 0;
}

std::string Trimmed(std::string s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) ++begin;
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) --end;
    return s.substr(begin, end - begin);
}

// channel.yaml:id 的规矩(§3.1):全局渠道 id,小写 kebab-case。与
// package/component.cpp 的 IsKebabCaseLocalId 同一口径,各自成一份小
// 函数——channel 是纯合同库,不 include package/。
bool IsKebabCaseId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    if (id.front() == '-' || id.back() == '-') return false;
    if (id.find("--") != std::string_view::npos) return false;
    for (const unsigned char ch : id) {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-') return false;
    }
    return true;
}

// ${channel_dir} 展开后是否仍在 package_root 内(词法规范化比较,不碰盘;
// symlink 越界另由 Package 盘点层记账,§3.2 同款口径抄自
// component.cpp::EscapesPackageRoot)。
bool EscapesPackageRoot(const std::string& expanded_path, const std::filesystem::path& package_root) {
    const std::filesystem::path root = package_root.lexically_normal();
    const std::filesystem::path normalized = Utf8ToPath(expanded_path).lexically_normal();
    const std::string rel = PathToUtf8(normalized.lexically_relative(root));
    return rel == ".." || rel.rfind("../", 0) == 0 || rel.rfind("..\\", 0) == 0;
}

// args/command 里的占位符规矩(§3.2):只许 ${channel_dir} 一个占位符,
// 不走 shell 字符串。命中即按 channel_dir 展开检查越界;其余 ${...} 一律
// 报"认不得的占位符"。裸串(不含占位符)原样放行——要么是 Package 内
// 冻结 executable 的相对/绝对路径片段,要么是 requires.executables
// 明列的裸命令名,两者都不含 "${"。
bool CheckArgPlaceholders(const std::string& value, const std::filesystem::path& package_root,
                          const std::filesystem::path& channel_dir, const std::string& field, int line,
                          std::vector<ChannelManifestError>& errors) {
    std::size_t pos = 0;
    bool ok = true;
    while (pos < value.size()) {
        const std::size_t hit = value.find("${", pos);
        if (hit == std::string::npos) break;
        const std::size_t close = value.find('}', hit);
        if (close == std::string::npos) {
            errors.push_back({field, line, "占位符没有闭合的 }: " + value.substr(hit)});
            return false;
        }
        const std::string token = value.substr(hit, close - hit + 1);
        if (token == "${channel_dir}") {
            const std::string expanded = PathToUtf8(channel_dir.lexically_normal()) + value.substr(close + 1);
            if (EscapesPackageRoot(expanded, package_root)) {
                errors.push_back({field, line, "path_escape: " + token + " 展开后逃出包根"});
                ok = false;
            }
        } else {
            errors.push_back({field, line, "认不得的占位符(只许 ${channel_dir}): " + token});
            ok = false;
        }
        pos = close + 1;
    }
    return ok;
}

const std::set<std::string> kTransportValues = {"websocket", "webhook", "long_polling", "sdk_events"};
const std::set<std::string> kConversationValues = {"direct", "group", "guild", "channel", "thread"};
const std::set<std::string> kInboundValues = {"text",    "image",   "audio",  "video",       "file",
                                              "link",    "mention", "reply",  "location",    "unsupported"};
const std::set<std::string> kOutboundValues = {"text", "image", "audio", "video", "file", "reply"};
const std::set<std::string> kDeliveryValues = {"send", "edit", "native_stream", "typing", "react"};
const std::set<std::string> kLoginValues = {"credentials", "qr", "oauth", "device_code"};

// capabilities 的一档数组字段:取值须在 allowed 集合内,重复值不特别拦
//(声明重复不改变有效能力集,不算错)。
bool ParseCapabilityArray(const YAML::Node& node, const std::string& field,
                          const std::set<std::string>& allowed, std::vector<std::string>* out,
                          std::vector<ChannelManifestError>& errors) {
    if (!node) return true;
    if (!node.IsSequence()) {
        errors.push_back({field, LineOf(node), "必须是字符串数组"});
        return false;
    }
    bool ok = true;
    for (const auto& item : node) {
        if (!item.IsScalar()) {
            errors.push_back({field, LineOf(item), "元素必须是字符串"});
            ok = false;
            continue;
        }
        const std::string value = item.Scalar();
        if (allowed.count(value) == 0) {
            errors.push_back({field, LineOf(item), "取值不在冻结集合内: " + value});
            ok = false;
            continue;
        }
        out->push_back(value);
    }
    return ok;
}

}  // namespace

std::expected<ChannelManifest, std::vector<ChannelManifestError>> ParseChannelManifestYaml(
    std::string_view yaml_text, const std::filesystem::path& package_root,
    const std::filesystem::path& channel_dir) {
    std::vector<ChannelManifestError> errors;
    YAML::Node root;
    try {
        root = YAML::Load(std::string(yaml_text));
    } catch (const YAML::Exception& ex) {
        return std::unexpected(std::vector<ChannelManifestError>{
            {"(yaml)", ex.mark.line >= 0 ? ex.mark.line + 1 : 0, std::string("YAML 语法坏: ") + ex.what()}});
    }
    if (!root.IsMap()) {
        return std::unexpected(std::vector<ChannelManifestError>{{"(yaml)", 1, "根必须是映射"}});
    }

    static const std::set<std::string> kKnownTop = {"schema",       "id",           "name",
                                                     "description", "runtime",      "capabilities",
                                                     "limits",      "state",        "risk"};
    for (const auto& kv : root) {
        const std::string key = kv.first.as<std::string>();
        if (kKnownTop.count(key) == 0) {
            errors.push_back({"(yaml)", LineOf(kv.first), "未知字段: " + key});
        }
    }

    ChannelManifest def;

    if (const auto& n = root["schema"]; n && n.IsScalar()) {
        try {
            def.schema = n.as<int>();
        } catch (const YAML::Exception&) {
            errors.push_back({"schema", LineOf(n), "必须是整数"});
        }
    } else {
        errors.push_back({"schema", LineOf(n), "缺 schema(必填)"});
    }
    if (def.schema != kChannelManifestSchemaVersion && errors.empty()) {
        errors.push_back({"schema", LineOf(root["schema"]),
                          "只认 " + std::to_string(kChannelManifestSchemaVersion) + ",给了 " +
                              std::to_string(def.schema) + "(不静默猜结构)"});
    }

    const auto read_string = [&](const char* key, std::string& into, bool required) {
        const auto& n = root[key];
        if (!n || !n.IsScalar()) {
            if (required) errors.push_back({key, LineOf(n), "缺必填字符串"});
            return;
        }
        into = n.Scalar();
        if (required && Trimmed(into).empty()) {
            errors.push_back({key, LineOf(n), "不许为空"});
        }
    };
    read_string("id", def.id, true);
    read_string("name", def.name, true);
    read_string("description", def.description, true);
    if (!def.id.empty() && !IsKebabCaseId(def.id)) {
        errors.push_back({"id", LineOf(root["id"]),
                          "id_invalid: 须小写 kebab-case(字母数字单横线),给了 " + def.id});
    }

    // ---- runtime ----
    const auto& runtime = root["runtime"];
    if (!runtime || !runtime.IsMap()) {
        errors.push_back({"runtime", LineOf(runtime), "缺 runtime 映射(command 必填)"});
    } else {
        static const std::set<std::string> kKnownRuntime = {
            "kind", "command", "args", "protocol", "startup_timeout_ms", "shutdown_timeout_ms",
            "requires"};
        for (const auto& kv : runtime) {
            const std::string key = kv.first.as<std::string>();
            if (kKnownRuntime.count(key) == 0) {
                errors.push_back({"runtime." + key, LineOf(kv.first), "未知字段"});
            }
        }
        if (const auto& n = runtime["kind"]; n && n.IsScalar()) {
            def.runtime.kind = n.Scalar();
            if (def.runtime.kind != "process") {
                errors.push_back({"runtime.kind", LineOf(n),
                                  "首版只收 process(native-library Channel 首版不做),给了 " +
                                      def.runtime.kind});
            }
        } else {
            errors.push_back({"runtime.kind", LineOf(runtime["kind"]), "缺必填(只认 process)"});
        }
        if (const auto& n = runtime["command"]; n && n.IsScalar()) {
            def.runtime.command = n.Scalar();
            if (Trimmed(def.runtime.command).empty()) {
                errors.push_back({"runtime.command", LineOf(n), "不许为空"});
            } else {
                CheckArgPlaceholders(def.runtime.command, package_root, channel_dir, "runtime.command",
                                     LineOf(n), errors);
            }
        } else {
            errors.push_back({"runtime.command", LineOf(runtime["command"]), "缺必填(可执行文件,exec form)"});
        }
        if (const auto& args = runtime["args"]; args && args.IsSequence()) {
            for (const auto& item : args) {
                if (!item.IsScalar()) {
                    errors.push_back({"runtime.args", LineOf(item), "元素必须是字符串"});
                    continue;
                }
                const std::string value = item.Scalar();
                CheckArgPlaceholders(value, package_root, channel_dir, "runtime.args", LineOf(item),
                                     errors);
                def.runtime.args.push_back(value);
            }
        } else if (args && !args.IsSequence()) {
            errors.push_back({"runtime.args", LineOf(args), "必须是字符串数组"});
        }
        if (const auto& n = runtime["protocol"]; n && n.IsScalar()) {
            def.runtime.protocol = n.Scalar();
            if (def.runtime.protocol != "lubancode-channel/1") {
                errors.push_back({"runtime.protocol", LineOf(n),
                                  "首版只收 lubancode-channel/1,给了 " + def.runtime.protocol});
            }
        } else {
            errors.push_back({"runtime.protocol", LineOf(runtime["protocol"]), "缺必填(只认 lubancode-channel/1)"});
        }
        if (const auto& n = runtime["startup_timeout_ms"]; n && n.IsScalar()) {
            try {
                def.runtime.startup_timeout_ms = n.as<int>();
                if (def.runtime.startup_timeout_ms < 0) {
                    errors.push_back({"runtime.startup_timeout_ms", LineOf(n), "不许为负"});
                }
            } catch (const YAML::Exception&) {
                errors.push_back({"runtime.startup_timeout_ms", LineOf(n), "必须是整数"});
            }
        }
        if (const auto& n = runtime["shutdown_timeout_ms"]; n && n.IsScalar()) {
            try {
                def.runtime.shutdown_timeout_ms = n.as<int>();
                if (def.runtime.shutdown_timeout_ms < 0) {
                    errors.push_back({"runtime.shutdown_timeout_ms", LineOf(n), "不许为负"});
                }
            } catch (const YAML::Exception&) {
                errors.push_back({"runtime.shutdown_timeout_ms", LineOf(n), "必须是整数"});
            }
        }
        if (const auto& requires_node = runtime["requires"]; requires_node && requires_node.IsMap()) {
            static const std::set<std::string> kKnownRequires = {"executables"};
            for (const auto& kv : requires_node) {
                const std::string key = kv.first.as<std::string>();
                if (kKnownRequires.count(key) == 0) {
                    errors.push_back({"runtime.requires." + key, LineOf(kv.first), "未知字段"});
                }
            }
            if (const auto& execs = requires_node["executables"]; execs && execs.IsSequence()) {
                for (const auto& item : execs) {
                    if (!item.IsMap()) {
                        errors.push_back(
                            {"runtime.requires.executables", LineOf(item), "元素必须是映射 {name, version?}"});
                        continue;
                    }
                    static const std::set<std::string> kKnownExec = {"name", "version"};
                    for (const auto& kv : item) {
                        const std::string key = kv.first.as<std::string>();
                        if (kKnownExec.count(key) == 0) {
                            errors.push_back(
                                {"runtime.requires.executables[]." + key, LineOf(kv.first), "未知字段"});
                        }
                    }
                    ChannelManifestExecutableRequirement req;
                    if (const auto& name = item["name"]; name && name.IsScalar()) {
                        req.name = name.Scalar();
                        if (Trimmed(req.name).empty()) {
                            errors.push_back({"runtime.requires.executables[].name", LineOf(name), "不许为空"});
                        }
                    } else {
                        errors.push_back(
                            {"runtime.requires.executables[].name", LineOf(item), "缺必填 name"});
                    }
                    if (const auto& version = item["version"]; version && version.IsScalar()) {
                        req.version = version.Scalar();
                    } else if (version && !version.IsScalar()) {
                        errors.push_back(
                            {"runtime.requires.executables[].version", LineOf(version), "必须是字符串"});
                    }
                    def.runtime.requires_executables.push_back(std::move(req));
                }
            } else if (execs && !execs.IsSequence()) {
                errors.push_back({"runtime.requires.executables", LineOf(execs), "必须是数组"});
            }
        } else if (requires_node && !requires_node.IsMap()) {
            errors.push_back({"runtime.requires", LineOf(requires_node), "必须是映射"});
        }
    }

    // ---- capabilities ----
    if (const auto& caps = root["capabilities"]; caps && caps.IsMap()) {
        static const std::set<std::string> kKnownCaps = {"transports", "conversations", "inbound",
                                                          "outbound",   "delivery",      "login"};
        for (const auto& kv : caps) {
            const std::string key = kv.first.as<std::string>();
            if (kKnownCaps.count(key) == 0) {
                errors.push_back({"capabilities." + key, LineOf(kv.first), "未知字段"});
            }
        }
        ParseCapabilityArray(caps["transports"], "capabilities.transports", kTransportValues,
                             &def.capabilities.transports, errors);
        ParseCapabilityArray(caps["conversations"], "capabilities.conversations", kConversationValues,
                             &def.capabilities.conversations, errors);
        ParseCapabilityArray(caps["inbound"], "capabilities.inbound", kInboundValues,
                             &def.capabilities.inbound, errors);
        ParseCapabilityArray(caps["outbound"], "capabilities.outbound", kOutboundValues,
                             &def.capabilities.outbound, errors);
        ParseCapabilityArray(caps["delivery"], "capabilities.delivery", kDeliveryValues,
                             &def.capabilities.delivery, errors);
        ParseCapabilityArray(caps["login"], "capabilities.login", kLoginValues, &def.capabilities.login,
                             errors);
    } else if (caps && !caps.IsMap()) {
        errors.push_back({"capabilities", LineOf(caps), "必须是映射"});
    }

    // ---- limits ----
    if (const auto& limits = root["limits"]; limits && limits.IsMap()) {
        static const std::set<std::string> kKnownLimits = {"text_chars", "media_bytes",
                                                            "outbound_requests_per_minute"};
        for (const auto& kv : limits) {
            const std::string key = kv.first.as<std::string>();
            if (kKnownLimits.count(key) == 0) {
                errors.push_back({"limits." + key, LineOf(kv.first), "未知字段"});
            }
        }
        const auto read_limit = [&](const char* key, std::optional<std::int64_t>& into) {
            const auto& n = limits[key];
            if (!n) return;
            if (!n.IsScalar()) {
                errors.push_back({std::string("limits.") + key, LineOf(n), "必须是整数"});
                return;
            }
            try {
                const std::int64_t value = n.as<std::int64_t>();
                if (value < 0) {
                    errors.push_back({std::string("limits.") + key, LineOf(n), "不许为负"});
                    return;
                }
                into = value;
            } catch (const YAML::Exception&) {
                errors.push_back({std::string("limits.") + key, LineOf(n), "必须是整数"});
            }
        };
        read_limit("text_chars", def.limits.text_chars);
        read_limit("media_bytes", def.limits.media_bytes);
        read_limit("outbound_requests_per_minute", def.limits.outbound_requests_per_minute);
    } else if (limits && !limits.IsMap()) {
        errors.push_back({"limits", LineOf(limits), "必须是映射"});
    }

    // ---- state ----
    if (const auto& state = root["state"]; state && state.IsMap()) {
        static const std::set<std::string> kKnownState = {"format", "migrator"};
        for (const auto& kv : state) {
            const std::string key = kv.first.as<std::string>();
            if (kKnownState.count(key) == 0) {
                errors.push_back({"state." + key, LineOf(kv.first), "未知字段"});
            }
        }
        if (const auto& n = state["format"]; n && n.IsScalar()) {
            try {
                def.state.format = n.as<int>();
                if (def.state.format < 1) {
                    errors.push_back({"state.format", LineOf(n), "从 1 起"});
                }
            } catch (const YAML::Exception&) {
                errors.push_back({"state.format", LineOf(n), "必须是整数"});
            }
        }
        if (const auto& n = state["migrator"]; n && n.IsScalar()) {
            const std::string value = n.Scalar();
            CheckArgPlaceholders(value, package_root, channel_dir, "state.migrator", LineOf(n), errors);
            def.state.migrator = value;
        } else if (n && !n.IsScalar()) {
            errors.push_back({"state.migrator", LineOf(n), "必须是字符串"});
        }
    } else if (state && !state.IsMap()) {
        errors.push_back({"state", LineOf(state), "必须是映射"});
    }

    // ---- risk(§3.6:manifest 不收账号 token/allowlist/webhook URL;risk 是
    // 唯一的附加风险标注,只认这一个值) ----
    if (const auto& n = root["risk"]; n && n.IsScalar()) {
        const std::string value = n.Scalar();
        if (value != "unofficial_personal_account") {
            errors.push_back({"risk", LineOf(n),
                              "只认 unofficial_personal_account,给了 " + value});
        }
        def.risk = value;
    } else if (n && !n.IsScalar()) {
        errors.push_back({"risk", LineOf(n), "必须是字符串"});
    }

    if (!errors.empty()) return std::unexpected(std::move(errors));
    return def;
}

}  // namespace lubancode::channel
