#include "channel/channel_config.hpp"

#include <array>

#include "channel/types.hpp"  // ConversationKindFromName(binding match.kind 校验)

namespace lubancode::channel {

std::size_t CountPlatformChars(std::string_view text) {
    // 官方口径:一个中文汉字算 2 字符。按 UTF-8 码点起始字节走——ASCII
    // 码点 1,其余码点 2(与官方"最多 10 个字符,一个中文汉字算 2 个字符"
    // 同尺)。坏字节按字节计,不静默吞。
    std::size_t platform = 0;
    for (const char byte : text) {
        const auto raw = static_cast<unsigned char>(byte);
        if ((raw & 0xC0) != 0x80) {  // 码点起始字节(ASCII 或多字节首字节)
            platform += (raw & 0x80) == 0 ? 1 : 2;
        }
    }
    return platform;
}

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
    // 与运行时 credential resolver 同一口径(configuration.md §4:secret_file
    // > secret_env > secret 明文)。多来源并配时报高优先级者;resolver 遇
    // 高优先级来源无效时明报,不静默降级。
    if (account.secret_file.has_value() && !account.secret_file->empty()) {
        return CredentialSource::FromFile;
    }
    if (account.secret_env.has_value() && !account.secret_env->empty()) {
        return CredentialSource::FromEnv;
    }
    if (account.secret.has_value() && !account.secret->empty()) {
        return CredentialSource::InlinePlaintext;
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

// tools 上限段(渠道层/账号层共用;QQ 接入单 Q0)。allow 的 presence 显式
// 保留:键在(哪怕空数组)= 设上限;键不在 = nullopt 不添上限。Q6 的
// approve 同款(键在 = 本层参与审批带)。
bool ParseToolsPolicy(const nlohmann::json& value, const std::string& path,
                      const std::string& file_path_for_error, ChannelToolsUserPolicy* out,
                      std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "preset") {
            if (!it.value().is_string() || value.contains("allow") || value.contains("approve")) {
                *error = path + ".preset 须为 readonly/ask/auto，不能同时写 allow/approve";
                return false;
            }
            const auto preset = ChannelToolsPreset(it.value().get<std::string>());
            if (!preset) {
                *error = path + ".preset 只认 readonly/ask/auto";
                return false;
            }
            out->preset = preset->preset;
            out->allow = preset->allow;
            out->approve = preset->approve;
        } else if (it.key() == "allow") {
            std::vector<std::string> allow;
            if (!ParseStringArray(it.value(), path + ".allow", file_path_for_error, &allow, error)) {
                return false;
            }
            out->allow = std::move(allow);  // 空数组也是显式上限:禁全部工具
        } else if (it.key() == "deny") {
            if (!ParseStringArray(it.value(), path + ".deny", file_path_for_error, &out->deny,
                                  error)) {
                return false;
            }
        } else if (it.key() == "approve") {
            // Q6:可申请审批带(执行前经远端按钮问用户,不预先授权)。
            std::vector<std::string> approve;
            if (!ParseStringArray(it.value(), path + ".approve", file_path_for_error, &approve,
                                  error)) {
                return false;
            }
            out->approve = std::move(approve);
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + it.key() +
                     " 是认不得的字段(tools 只收 preset/allow/deny/approve)";
            return false;
        }
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

// ---- Q7 菜单/面板/命令绑定解析(configuration.md §7;官方形状见 hpp 注) ----

// 平台字符长度上限校验(汉字算 2,见 CountPlatformChars)。
bool CheckPlatformChars(const std::string& text, std::size_t limit, const std::string& path,
                        const std::string& file_path_for_error, std::string* error) {
    if (CountPlatformChars(text) > limit) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 超长(平台字符上限 " +
                 std::to_string(limit) + ",中文汉字算 2): \"" + text + "\"";
        return false;
    }
    return true;
}

// link 型字段:非空且必须 https:// 开头(官方 40030008 预拦)。
bool CheckLinkUrl(const std::string& link, const std::string& path,
                  const std::string& file_path_for_error, std::string* error) {
    if (link.rfind("https://", 0) != 0) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须以 https:// 开头";
        return false;
    }
    return true;
}

bool ParseMenuSubItem(const nlohmann::json& value, const std::string& path,
                      const std::string& file_path_for_error, ChannelMenuSubItemUserConfig* out,
                      std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "name" || key == "type" || key == "send_message" || key == "link") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                         " 必须是字符串";
                return false;
            }
            if (key == "name") {
                out->name = it.value().get<std::string>();
            } else if (key == "type") {
                out->type = it.value().get<std::string>();
            } else if (key == "send_message") {
                out->send_message = it.value().get<std::string>();
            } else {
                out->link = it.value().get<std::string>();
            }
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(菜单子项只收 name/type/send_message/link)";
            return false;
        }
    }
    if (out->name.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".name 不能为空";
        return false;
    }
    if (!CheckPlatformChars(out->name, 14, path + ".name", file_path_for_error, error)) {
        return false;
    }
    if (out->type != "send_message" && out->type != "link") {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".type 只认 send_message/link(二级菜单不嵌套)";
        return false;
    }
    if (out->type == "send_message" && out->send_message.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".send_message 缺失(type=send_message 点击填入输入框的文本)";
        return false;
    }
    if (out->type == "link" && !CheckLinkUrl(out->link, path + ".link", file_path_for_error,
                                             error)) {
        return false;
    }
    return true;
}

bool ParseMenuItem(const nlohmann::json& value, const std::string& path,
                   const std::string& file_path_for_error, ChannelMenuItemUserConfig* out,
                   std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "name" || key == "type" || key == "send_message" || key == "link" ||
            key == "switch_id") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                         " 必须是字符串";
                return false;
            }
            if (key == "name") {
                out->name = it.value().get<std::string>();
            } else if (key == "type") {
                out->type = it.value().get<std::string>();
            } else if (key == "send_message") {
                out->send_message = it.value().get<std::string>();
            } else if (key == "link") {
                out->link = it.value().get<std::string>();
            } else {
                out->switch_id = it.value().get<std::string>();
            }
        } else if (key == "switch_default") {
            if (!it.value().is_boolean()) {
                *error =
                    "配置文件 " + file_path_for_error + " 里的 " + path + ".switch_default 必须是布尔";
                return false;
            }
            out->switch_default = it.value().get<bool>();
        } else if (key == "sub_menu_items") {
            if (!it.value().is_array()) {
                *error =
                    "配置文件 " + file_path_for_error + " 里的 " + path + ".sub_menu_items 必须是数组";
                return false;
            }
            std::size_t index = 0;
            for (const auto& sub : it.value()) {
                ChannelMenuSubItemUserConfig sub_item;
                if (!ParseMenuSubItem(sub, path + ".sub_menu_items[" + std::to_string(index) + "]",
                                      file_path_for_error, &sub_item, error)) {
                    return false;
                }
                out->sub_menu_items.push_back(std::move(sub_item));
                ++index;
            }
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(菜单项只收 name/type/send_message/link/switch_id/"
                     "switch_default/sub_menu_items)";
            return false;
        }
    }
    if (out->name.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".name 不能为空";
        return false;
    }
    if (!CheckPlatformChars(out->name, 10, path + ".name", file_path_for_error, error)) {
        return false;
    }
    if (out->type != "switch" && out->type != "send_message" && out->type != "link" &&
        out->type != "menu") {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".type 只认 switch/send_message/link/menu";
        return false;
    }
    if (out->sub_menu_items.size() > 5) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".sub_menu_items 最多 5 项(官方限制)";
        return false;
    }
    if (out->type == "menu") {
        if (out->sub_menu_items.empty()) {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                     " 是 type=menu 折叠项但没写 sub_menu_items";
            return false;
        }
    } else if (!out->sub_menu_items.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".sub_menu_items 仅 type=menu 时有效";
        return false;
    }
    if (out->type == "send_message" && out->send_message.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".send_message 缺失(type=send_message 点击填入输入框的文本)";
        return false;
    }
    if (out->type == "link" && !CheckLinkUrl(out->link, path + ".link", file_path_for_error,
                                             error)) {
        return false;
    }
    if (out->type == "switch" && out->switch_id.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".switch_id 缺失(开关标识只承载用户偏好,不改授权)";
        return false;
    }
    return true;
}

bool ParsePanelItem(const nlohmann::json& value, const std::string& path,
                    const std::string& file_path_for_error, ChannelPanelItemUserConfig* out,
                    std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "name" || key == "desc" || key == "type" || key == "link") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                         " 必须是字符串";
                return false;
            }
            if (key == "name") {
                out->name = it.value().get<std::string>();
            } else if (key == "desc") {
                out->desc = it.value().get<std::string>();
            } else if (key == "type") {
                out->type = it.value().get<std::string>();
            } else {
                out->link = it.value().get<std::string>();
            }
        } else if (key == "only_admin") {
            if (!it.value().is_boolean()) {
                *error =
                    "配置文件 " + file_path_for_error + " 里的 " + path + ".only_admin 必须是布尔";
                return false;
            }
            out->only_admin = it.value().get<bool>();
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(面板元素只收 name/desc/type/only_admin/link)";
            return false;
        }
    }
    if (out->name.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".name 不能为空";
        return false;
    }
    if (!CheckPlatformChars(out->name, 14, path + ".name", file_path_for_error, error)) {
        return false;
    }
    if (!CheckPlatformChars(out->desc, 30, path + ".desc", file_path_for_error, error)) {
        return false;
    }
    if (out->type != "command" && out->type != "link") {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".type 只认 command/link";
        return false;
    }
    if (out->type == "link" && !CheckLinkUrl(out->link, path + ".link", file_path_for_error,
                                             error)) {
        return false;
    }
    return true;
}

bool ParsePanelConfig(const nlohmann::json& value, const std::string& path,
                      const std::string& file_path_for_error, ChannelPanelUserConfig* out,
                      std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "enabled") {
            if (!it.value().is_boolean()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".enabled 必须是布尔";
                return false;
            }
            out->enabled = it.value().get<bool>();
        } else if (key == "scope" || key == "target_type" || key == "remark") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                         " 必须是字符串";
                return false;
            }
            if (key == "scope") {
                out->scope = it.value().get<std::string>();
            } else if (key == "target_type") {
                out->target_type = it.value().get<std::string>();
            } else {
                out->remark = it.value().get<std::string>();
            }
        } else if (key == "items") {
            if (!it.value().is_array()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".items 必须是数组";
                return false;
            }
            std::size_t index = 0;
            for (const auto& item : it.value()) {
                ChannelPanelItemUserConfig panel_item;
                if (!ParsePanelItem(item, path + ".items[" + std::to_string(index) + "]",
                                    file_path_for_error, &panel_item, error)) {
                    return false;
                }
                out->items.push_back(std::move(panel_item));
                ++index;
            }
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(面板段只收 enabled/scope/target_type/remark/items)";
            return false;
        }
    }
    if (out->scope != "c2c") {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".scope 首版只认 c2c(QQ 单聊;group/channel/dm 后续批)";
        return false;
    }
    if (!out->target_type.empty() && out->target_type != "all" && out->target_type != "specific") {
        *error =
            "配置文件 " + file_path_for_error + " 里的 " + path + ".target_type 只认 all/specific";
        return false;
    }
    if (out->items.size() > 20) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".items 最多 20 项(官方限制)";
        return false;
    }
    if (!CheckPlatformChars(out->remark, 200, path + ".remark", file_path_for_error, error)) {
        return false;  // 发布时还要拼所有权前缀,给前缀留余量
    }
    if (out->enabled && out->items.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 " 启用发布但 items 为空(空面板没有发布意义)";
        return false;
    }
    return true;
}

bool ParseMenuConfig(const nlohmann::json& value, const std::string& path,
                     const std::string& file_path_for_error, ChannelMenuUserConfig* out,
                     std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    if (value.contains("preset")) {
        if (!value["preset"].is_string() || value["preset"] != "assistant" || value.contains("items")) {
            *error = path + ".preset 只认 assistant，且不能与 items 同写";
            return false;
        }
        for (const auto& [name, command] : std::vector<std::pair<std::string, std::string>>{
                 {"帮助", "/help"}, {"新会话", "/new"}, {"会话列表", "/session"},
                 {"我的提醒", "/reminders"}, {"文件说明", "/files"}, {"功能状态", "/status"}}) {
            ChannelMenuItemUserConfig item;
            item.name = name;
            item.type = "send_message";
            item.send_message = command;
            out->items.push_back(std::move(item));
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "preset") {
            continue;
        } else if (key == "publish") {
            if (!it.value().is_boolean()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".publish 必须是布尔";
                return false;
            }
            out->publish = it.value().get<bool>();
        } else if (key == "items") {
            if (!it.value().is_array()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".items 必须是数组";
                return false;
            }
            std::size_t index = 0;
            for (const auto& item : it.value()) {
                ChannelMenuItemUserConfig menu_item;
                if (!ParseMenuItem(item, path + ".items[" + std::to_string(index) + "]",
                                   file_path_for_error, &menu_item, error)) {
                    return false;
                }
                out->items.push_back(std::move(menu_item));
                ++index;
            }
        } else if (key == "panel") {
            ChannelPanelUserConfig panel;
            if (!ParsePanelConfig(it.value(), path + ".panel", file_path_for_error, &panel,
                                  error)) {
                return false;
            }
            out->panel = std::move(panel);
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(菜单段只收 publish/preset/items/panel)";
            return false;
        }
    }
    if (out->items.size() > 10) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".items 最多 10 项(官方限制)";
        return false;
    }
    if (out->publish && out->items.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 " 启用发布但 items 为空(想清空菜单在开放平台操作,不走配置)";
        return false;
    }
    return true;
}

bool ParseCommandBinding(const nlohmann::json& value, const std::string& path,
                         const std::string& file_path_for_error,
                         ChannelCommandBindingUserConfig* out, std::string* error) {
    if (!value.is_object()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + " 必须是一个 JSON object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "match" || key == "action" || key == "prompt") {
            if (!it.value().is_string()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                         " 必须是字符串";
                return false;
            }
            if (key == "match") {
                out->match = it.value().get<std::string>();
            } else if (key == "action") {
                out->action = it.value().get<std::string>();
            } else {
                out->prompt = it.value().get<std::string>();
            }
        } else if (key == "require_tools") {
            if (!ParseStringArray(it.value(), path + ".require_tools", file_path_for_error,
                                  &out->require_tools, error)) {
                return false;
            }
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(命令绑定只收 match/action/prompt/require_tools)";
            return false;
        }
    }
    if (out->match.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".match 不能为空";
        return false;
    }
    if (CountPlatformChars(out->match) > 64) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".match 超长(64 字符)";
        return false;
    }
    if (out->action != "help" && out->action != "file_help" && out->action != "list_reminders" &&
        out->action != "prompt") {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".action 只认 help/file_help/list_reminders/prompt";
        return false;
    }
    if (out->action == "prompt" && out->prompt.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".prompt 缺失(action=prompt 的预设输入)";
        return false;
    }
    if (out->action != "prompt" && !out->prompt.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".prompt 只在 action=prompt 时有效";
        return false;
    }
    if (out->action != "prompt" && !out->require_tools.empty()) {
        *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                 ".require_tools 只在 action=prompt 时有效(控制命令不经模型,无工具面)";
        return false;
    }
    for (const auto& tool : out->require_tools) {
        if (tool.empty()) {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                     ".require_tools 元素不能为空串";
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
        } else if (key == "tools") {
            if (!ParseToolsPolicy(field, path + ".tools", file_path_for_error, &out->tools, error)) {
                return false;
            }
        } else if (key == "menu") {
            ChannelMenuUserConfig menu;
            if (!ParseMenuConfig(field, path + ".menu", file_path_for_error, &menu, error)) {
                return false;
            }
            out->menu = std::move(menu);
        } else if (key == "commands") {
            if (!field.is_array()) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".commands 必须是数组";
                return false;
            }
            std::size_t index = 0;
            for (const auto& item : field) {
                ChannelCommandBindingUserConfig binding;
                if (!ParseCommandBinding(item,
                                         path + ".commands[" + std::to_string(index) + "]",
                                         file_path_for_error, &binding, error)) {
                    return false;
                }
                out->commands.push_back(std::move(binding));
                ++index;
            }
        } else {
            *error = "配置文件 " + file_path_for_error + " 里的 " + path + "." + key +
                     " 是认不得的字段(账号段收 enabled/transport/app_id/secret_env/"
                     "secret_file/secret/dm_policy/allow_from/group_policy/group_allow_from/"
                     "require_mention/allow_bots/agent/reply/group_scope/tools/menu/commands)";
            return false;
        }
    }
    // 命令绑定的 match 不许重复(重复 = 同一输入两条分派,按文件次序碰运气
    // 的老毛病不犯)。
    for (std::size_t i = 0; i < out->commands.size(); ++i) {
        for (std::size_t j = i + 1; j < out->commands.size(); ++j) {
            if (out->commands[i].match == out->commands[j].match) {
                *error = "配置文件 " + file_path_for_error + " 里的 " + path +
                         ".commands 有重复的 match(\"" + out->commands[i].match + "\")";
                return false;
            }
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
                    std::vector<std::string> allow;
                    if (!ParseStringArray(tool_it.value(), path + ".tools.allow",
                                          file_path_for_error, &allow, error)) {
                        return false;
                    }
                    // presence 显式保留:allow 键在(含空数组)= 设上限,
                    // 空名单即禁全部工具;键不在 = 不添上限(旧语义)。
                    out->tools.allow = std::move(allow);
                } else if (tool_it.key() == "deny") {
                    if (!ParseStringArray(tool_it.value(), path + ".tools.deny",
                                          file_path_for_error, &out->tools.deny, error)) {
                        return false;
                    }
                } else if (tool_it.key() == "approve") {
                    // Q6:binding 层审批带(presence 合同与 allow 同款)。
                    std::vector<std::string> approve;
                    if (!ParseStringArray(tool_it.value(), path + ".tools.approve",
                                          file_path_for_error, &approve, error)) {
                        return false;
                    }
                    out->tools.approve = std::move(approve);
                } else {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + path + ".tools." +
                             tool_it.key() + " 是认不得的字段(tools 只收 allow/deny/approve)";
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

bool IsValidChannelId(const std::string& id) {
    if (id.empty() || id.size() > 64 || id == "." || id == "..") {
        return false;
    }
    for (const char c : id) {
        const unsigned char byte = static_cast<unsigned char>(c);
        // 路径分隔段(两平台)、盘符冒号、控制字符、空白一概不收——id 会
        // 直接拼进 state_root/<ch>/<acct> 与锁文件名,收了就拼得出根外路径。
        if (byte < 0x21 || byte > 0x7E || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

bool IsValidChannelAccountId(const std::string& id) { return IsValidChannelId(id); }

std::optional<ChannelToolsUserPolicy> ChannelToolsPreset(const std::string& name) {
    if (name != "readonly" && name != "ask" && name != "auto") return std::nullopt;
    ChannelToolsUserPolicy policy;
    policy.preset = name;
    policy.allow = std::vector<std::string>{"read_file", "search", "web_fetch", "web_search",
                                          "skill", "get_current_time", "list_reminders"};
    policy.approve = std::vector<std::string>{};
    if (name != "readonly") {
        policy.allow->insert(policy.allow->end(), {"create_reminder", "cancel_reminder"});
        const std::vector<std::string> actions{"write_file", "edit_file", "run_command", "send_file"};
        if (name == "ask") policy.approve = actions;
        else policy.allow->insert(policy.allow->end(), actions.begin(), actions.end());
    }
    return policy;
}

ChannelAccountUserConfig MakeQqTemplateAccount() {
    // QQ 首版模板(configuration.md §7):逐字段显式。group_policy=disabled、
    // reply.mode=final 与全渠道默认(allowlist/block)不同——这是 QQ 模板
    // 自己的取值,不动全局默认;group 首版不开放,dm 走 pairing。
    ChannelAccountUserConfig account;
    account.enabled = false;  // 用户配齐凭据再自己开
    account.transport = "websocket";
    account.dm_policy = DmPolicy::Pairing;
    account.group_policy = GroupPolicy::Disabled;
    account.allow_bots = false;
    account.require_mention = true;
    account.reply.mode = ReplyMode::Final;
    // 新账号由向导默认选询问档；旧账号无 preset 时保留显式名单。
    // 查询和提醒直接用；写文件/命令/传文件经 Q6 审批，不扩权到插件/MCP。
    account.tools = *ChannelToolsPreset("ask");
    return account;
}

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
            } else if (key == "tools") {
                if (!ParseToolsPolicy(field, channel_path + ".tools", file_path_for_error,
                                      &channel.tools, error)) {
                    return std::nullopt;
                }
            } else {
                if (error != nullptr) {
                    *error = "配置文件 " + file_path_for_error + " 里的 " + channel_path + "." +
                             key +
                             " 是认不得的字段(渠道段只收 enabled/default_account/accounts/"
                             "bindings/tools)";
                }
                return std::nullopt;
            }
        }
        out.emplace(channel_id, std::move(channel));
    }
    return out;
}

}  // namespace lubancode::channel
