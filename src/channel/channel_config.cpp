#include "channel/channel_config.hpp"

#include <array>

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
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(账号段收 enabled/transport/app_id/secret_env/"
                     "secret_file/secret/dm_policy/allow_from/group_policy/group_allow_from/"
                     "require_mention/allow_bots/agent/reply)";
            return false;
        }
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
            } else {
                if (error != nullptr) {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path + "." +
                             key + " 是认不得的字段(渠道段只收 enabled/default_account/accounts)";
                }
                return std::nullopt;
            }
        }
        out.emplace(channel_id, std::move(channel));
    }
    return out;
}

}  // namespace lubancode::channel
