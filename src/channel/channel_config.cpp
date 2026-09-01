#include "channel/channel_config.hpp"

#include <array>

#include "channel/types.hpp"  // ConversationKindFromName(binding match.kind 校验)

namespace lubancode::channel {

const char* DmPolicyName(DmPolicy policy) {
    switch (policy) {
        case DmPolicy::Pairing: return "pairing";
        case DmPolicy::Allowlist: return "allowlist";
        case DmPolicy::Open: return "open";
        case DmPolicy::Disabled: return "disabled";
    }
    return "unknown";
}

std::optional<DmPolicy> DmPolicyFromName(const std::string& name) {
    if (name == "pairing") return DmPolicy::Pairing;
    if (name == "allowlist") return DmPolicy::Allowlist;
    if (name == "open") return DmPolicy::Open;
    if (name == "disabled") return DmPolicy::Disabled;
    return std::nullopt;
}

const char* GroupPolicyName(GroupPolicy policy) {
    switch (policy) {
        case GroupPolicy::Allowlist: return "allowlist";
        case GroupPolicy::Open: return "open";
        case GroupPolicy::Disabled: return "disabled";
    }
    return "unknown";
}

std::optional<GroupPolicy> GroupPolicyFromName(const std::string& name) {
    if (name == "allowlist") return GroupPolicy::Allowlist;
    if (name == "open") return GroupPolicy::Open;
    if (name == "disabled") return GroupPolicy::Disabled;
    return std::nullopt;
}

const char* ReplyModeName(ReplyMode mode) {
    switch (mode) {
        case ReplyMode::Final: return "final";
        case ReplyMode::Block: return "block";
        case ReplyMode::Native: return "native";
    }
    return "unknown";
}

std::optional<ReplyMode> ReplyModeFromName(const std::string& name) {
    if (name == "final") return ReplyMode::Final;
    if (name == "block") return ReplyMode::Block;
    if (name == "native") return ReplyMode::Native;
    return std::nullopt;
}

const char* CredentialSourceName(CredentialSource source) {
    switch (source) {
        case CredentialSource::Missing: return "missing";
        case CredentialSource::FromEnv: return "env";
        case CredentialSource::FromFile: return "file";
        case CredentialSource::InlinePlaintext: return "plaintext";
    }
    return "unknown";
}

CredentialSource DescribeCredentialSource(const ChannelAccountUserConfig& account) {
    if (account.secret.has_value() && !account.secret->empty()) {
        return CredentialSource::InlinePlaintext;
    }
    if (account.secret_env.has_value() && !account.secret_env->empty()) {
        return CredentialSource::FromEnv;
    }
    if (account.secret_file.has_value() && !account.secret_file->empty()) {
        return CredentialSource::FromFile;
    }
    return CredentialSource::Missing;
}

namespace {

// 群聊 session scope 的合法值(§8)。空串 = 未配(用默认 group)。
bool IsValidGroupScope(const std::string& scope) {
    return scope.empty() || scope == "group" || scope == "group_sender" ||
           scope == "group_thread" || scope == "group_thread_sender";
}

bool ParseStringArray(const nlohmann::json& value, const std::string& path,
                      const std::string& file_path_for_error, std::vector<std::string>* out,
                      std::string* error) {
    if (!value.is_array()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 字段必须是字符串数组";
        return false;
    }
    for (const auto& item : value) {
        if (!item.is_string()) {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 数组元素必须是字符串";
            return false;
        }
        out->push_back(item.get<std::string>());
    }
    return true;
}

bool ParseReplyConfig(const nlohmann::json& value, const std::string& path,
                      const std::string& file_path_for_error, ChannelReplyUserConfig* out,
                      std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 字段必须是 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "mode") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".mode 必须是字符串";
                return false;
            }
            const auto mode = ReplyModeFromName(it.value().get<std::string>());
            if (!mode.has_value()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".mode 只认 final/block/native";
                return false;
            }
            out->mode = *mode;
        } else if (it.key() == "tool_progress") {
            if (!it.value().is_boolean()) {
                *error =
                    "配置文件 " + file_path_for_error + " 里的 " + path + ".tool_progress 必须是布尔";
                return false;
            }
            out->tool_progress = it.value().get<bool>();
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + it.key() +
                     " 是认不得的字段(reply 只收 mode/tool_progress)";
            return false;
        }
    }
    return true;
}

bool ParseAccountConfig(const std::string& account_id, const nlohmann::json& value,
                        const std::string& channel_path, const std::string& file_path_for_error,
                        ChannelAccountUserConfig* out, std::string* error) {
    const std::string path = channel_path + ".accounts." + account_id;
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        const nlohmann::json& field = it.value();
        if (key == "enabled") {
            if (!field.is_boolean()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".enabled 必须是布尔";
                return false;
            }
            out->enabled = field.get<bool>();
        } else if (key == "transport") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".transport 必须是字符串";
                return false;
            }
            out->transport = field.get<std::string>();
        } else if (key == "app_id") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".app_id 必须是字符串";
                return false;
            }
            out->app_id = field.get<std::string>();
        } else if (key == "secret_env") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".secret_env 必须是字符串";
                return false;
            }
            out->secret_env = field.get<std::string>();
        } else if (key == "secret_file") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".secret_file 必须是字符串";
                return false;
            }
            out->secret_file = field.get<std::string>();
        } else if (key == "secret") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".secret 必须是字符串";
                return false;
            }
            out->secret = field.get<std::string>();
        } else if (key == "dm_policy") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".dm_policy 必须是字符串";
                return false;
            }
            const auto policy = DmPolicyFromName(field.get<std::string>());
            if (!policy.has_value()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".dm_policy 只认 pairing/allowlist/open/disabled";
                return false;
            }
            out->dm_policy = *policy;
        } else if (key == "allow_from") {
            if (!ParseStringArray(field, path + ".allow_from", file_path_for_error, &out->allow_from,
                                  error)) {
                return false;
            }
        } else if (key == "group_policy") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".group_policy 必须是字符串";
                return false;
            }
            const auto policy = GroupPolicyFromName(field.get<std::string>());
            if (!policy.has_value()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".group_policy 只认 allowlist/open/disabled";
                return false;
            }
            out->group_policy = *policy;
        } else if (key == "group_allow_from") {
            if (!ParseStringArray(field, path + ".group_allow_from", file_path_for_error,
                                  &out->group_allow_from, error)) {
                return false;
            }
        } else if (key == "require_mention") {
            if (!field.is_boolean()) {
                *error =
                    "配置文件 " + file_path_for_error + " 里的 " + path + ".require_mention 必须是布尔";
                return false;
            }
            out->require_mention = field.get<bool>();
        } else if (key == "allow_bots") {
            if (!field.is_boolean()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".allow_bots 必须是布尔";
                return false;
            }
            out->allow_bots = field.get<bool>();
        } else if (key == "agent") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".agent 必须是字符串";
                return false;
            }
            out->agent = field.get<std::string>();
        } else if (key == "reply") {
            if (!ParseReplyConfig(field, path + ".reply", file_path_for_error, &out->reply,
                                  error)) {
                return false;
            }
        } else if (key == "group_scope") {
            if (!field.is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".group_scope 必须是字符串";
                return false;
            }
            out->group_scope = field.get<std::string>();
            if (!IsValidGroupScope(out->group_scope)) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".group_scope 只认 group/group_sender/group_thread/group_thread_sender";
                return false;
            }
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(账号段收 enabled/transport/app_id/secret_env/"
                     "secret_file/secret/dm_policy/allow_from/group_policy/group_allow_from/"
                     "require_mention/allow_bots/agent/reply/group_scope)";
            return false;
        }
    }
    return true;
}

// ---- binding 解析(§8 冻结形状) -------------------------------------------

bool ParseBindingConversation(const nlohmann::json& value, const std::string& path,
                              const std::string& file_path_for_error,
                              ChannelBindingConversationMatch* out, std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "kind") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".kind 必须是字符串";
                return false;
            }
            const std::string kind = it.value().get<std::string>();
            if (!ConversationKindFromName(kind).has_value()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".kind 只认 direct/group/guild/channel/thread";
                return false;
            }
            out->kind = kind;
        } else if (it.key() == "id") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".id 必须是字符串";
                return false;
            }
            out->id = it.value().get<std::string>();
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + it.key() +
                     " 是认不得的字段(conversation 只收 kind/id)";
            return false;
        }
    }
    return true;
}

bool ParseBindingMatch(const nlohmann::json& value, const std::string& path,
                       const std::string& file_path_for_error, const std::string& channel_id,
                       ChannelBindingMatch* out, std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "channel") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".channel 必须是字符串";
                return false;
            }
            out->channel = it.value().get<std::string>();
            // 渠道层 bindings:match.channel 只许等于本渠道(空 = 本渠道)。
            // 不许借渠道段的 bindings 指到别家渠道去。
            if (out->channel != channel_id) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".channel(=" +
                         out->channel + ")与所在渠道 " + channel_id + " 不一致";
                return false;
            }
        } else if (it.key() == "account") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".account 必须是字符串";
                return false;
            }
            out->account = it.value().get<std::string>();
        } else if (it.key() == "conversation") {
            ChannelBindingConversationMatch conversation;
            if (!ParseBindingConversation(it.value(), path + ".conversation", file_path_for_error,
                                          &conversation, error)) {
                return false;
            }
            out->conversation = std::move(conversation);
        } else if (it.key() == "thread") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".thread 必须是字符串";
                return false;
            }
            out->thread_id = it.value().get<std::string>();
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + it.key() +
                     " 是认不得的字段(match 只收 channel/account/conversation/thread)";
            return false;
        }
    }
    return true;
}

bool ParseBindingPolicy(const nlohmann::json& value, const std::string& path,
                        const std::string& file_path_for_error, ChannelBindingPolicy* out,
                        std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "tools") {
            const nlohmann::json& tools = it.value();
            if (!tools.is_object()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".tools 必须是一个 JSON object";
                return false;
            }
            for (auto tool_it = tools.begin(); tool_it != tools.end(); ++tool_it) {
                if (tool_it.key() == "allow") {
                    if (!ParseStringArray(tool_it.value(), path + ".tools.allow",
                                          file_path_for_error, &out->tools.allow, error)) {
                        return false;
                    }
                } else if (tool_it.key() == "deny") {
                    if (!ParseStringArray(tool_it.value(), path + ".tools.deny",
                                          file_path_for_error, &out->tools.deny, error)) {
                        return false;
                    }
                } else {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".tools." +
                             tool_it.key() + " 是认不得的字段(tools 只收 allow/deny)";
                    return false;
                }
            }
        } else if (it.key() == "memory") {
            const nlohmann::json& memory = it.value();
            if (!memory.is_object()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".memory 必须是一个 JSON object";
                return false;
            }
            ChannelBindingMemoryPolicy policy;
            for (auto mem_it = memory.begin(); mem_it != memory.end(); ++mem_it) {
                if (mem_it.key() == "user" || mem_it.key() == "project") {
                    if (!mem_it.value().is_boolean()) {
                        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".memory." +
                                 mem_it.key() + " 必须是布尔";
                        return false;
                    }
                    (mem_it.key() == "user" ? policy.user : policy.project) =
                        mem_it.value().get<bool>();
                } else {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".memory." +
                             mem_it.key() + " 是认不得的字段(memory 只收 user/project)";
                    return false;
                }
            }
            out->memory = std::move(policy);
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + it.key() +
                     " 是认不得的字段(policy 只收 tools/memory)";
            return false;
        }
    }
    return true;
}

bool ParseBindings(const nlohmann::json& value, const std::string& channel_path,
                   const std::string& file_path_for_error, const std::string& channel_id,
                   std::vector<ChannelBindingConfig>* out, std::string* error) {
    if (!value.is_array()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path +
                 ".bindings 必须是一个数组";
        return false;
    }
    int index = 0;
    for (const auto& item : value) {
        const std::string path = channel_path + ".bindings[" + std::to_string(index++) + "]";
        if (!item.is_object()) {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
            return false;
        }
        ChannelBindingConfig binding;
        bool has_match = false;
        for (auto it = item.begin(); it != item.end(); ++it) {
            if (it.key() == "agent") {
                if (!it.value().is_string()) {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".agent 必须是字符串";
                    return false;
                }
                binding.agent = it.value().get<std::string>();
            } else if (it.key() == "match") {
                has_match = true;
                if (!ParseBindingMatch(it.value(), path + ".match", file_path_for_error,
                                       channel_id, &binding.match, error)) {
                    return false;
                }
            } else if (it.key() == "policy") {
                if (!ParseBindingPolicy(it.value(), path + ".policy", file_path_for_error,
                                        &binding.policy, error)) {
                    return false;
                }
            } else {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + it.key() +
                         " 是认不得的字段(binding 只收 agent/match/policy)";
                return false;
            }
        }
        if (!has_match) {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 缺 match 字段";
            return false;
        }
        out->push_back(std::move(binding));
    }
    return true;
}

}  // namespace

std::optional<std::map<std::string, ChannelUserConfig>> ParseChannelsUserConfig(
    const nlohmann::json& channels_json, const std::string& file_path_for_error,
    std::string* error) {
    if (error != nullptr) error->clear();
    if (!channels_json.is_object()) {
        if (error != nullptr) {
            *error = "配置文件 " + file_path_for_error + " 里的 channels 字段必须是一个 JSON object";
        }
        return std::nullopt;
    }

    std::map<std::string, ChannelUserConfig> out;
    for (auto channel_it = channels_json.begin(); channel_it != channels_json.end();
         ++channel_it) {
        const std::string& channel_id = channel_it.key();
        const std::string channel_path = "channels." + channel_id;
        const nlohmann::json& value = channel_it.value();
        if (!value.is_object()) {
            if (error != nullptr) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path +
                         " 必须是一个 JSON object";
            }
            return std::nullopt;
        }
        ChannelUserConfig channel;
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string& key = it.key();
            const nlohmann::json& field = it.value();
            if (key == "enabled") {
                if (!field.is_boolean()) {
                    if (error != nullptr) {
                        *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path +
                                 ".enabled 必须是布尔";
                    }
                    return std::nullopt;
                }
                channel.enabled = field.get<bool>();
            } else if (key == "default_account") {
                if (!field.is_string()) {
                    if (error != nullptr) {
                        *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path +
                                 ".default_account 必须是字符串";
                    }
                    return std::nullopt;
                }
                channel.default_account = field.get<std::string>();
            } else if (key == "accounts") {
                if (!field.is_object()) {
                    if (error != nullptr) {
                        *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path +
                                 ".accounts 必须是一个 JSON object";
                    }
                    return std::nullopt;
                }
                for (auto account_it = field.begin(); account_it != field.end(); ++account_it) {
                    ChannelAccountUserConfig account;
                    if (!ParseAccountConfig(account_it.key(), account_it.value(), channel_path,
                                            file_path_for_error, &account, error)) {
                        return std::nullopt;
                    }
                    channel.accounts.emplace(account_it.key(), std::move(account));
                }
            } else if (key == "bindings") {
                if (!ParseBindings(field, channel_path, file_path_for_error, channel_id,
                                   &channel.bindings, error)) {
                    return std::nullopt;
                }
            } else {
                if (error != nullptr) {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path + "." +
                             key +
                             " 是认不得的字段(渠道段只收 enabled/default_account/accounts/bindings)";
                }
                return std::nullopt;
            }
        }
        out.emplace(channel_id, std::move(channel));
    }
    return out;
}

}  // namespace lubancode::channel
