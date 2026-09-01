// 渠道用户配置段解析(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/configuration.md §1(结构)、§7
// (策略字段与默认值)、§4(密钥来源三种)。config.json 顶层 "channels" 段
// 的严格解析:未知字段报错、坏枚举报错、类型错报错——渠道是全局网络
// 能力,配置错要明报,不走"救命阀静默跳过"。
//
// 层级规矩(§2):只有全局 config 可启用账号;项目 config 出现 channels
// 段一律明拒(合并层执行,见 config.cpp);密钥明文 secret 兼容收,但
// doctor 必须给 warning。
//
// 纯函数库:吃 nlohmann::json,不读文件、不读环境变量。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// 策略枚举(configuration.md §7)
// ---------------------------------------------------------------------------

enum class DmPolicy { Pairing, Allowlist, Open, Disabled };
enum class GroupPolicy { Allowlist, Open, Disabled };
enum class ReplyMode { Final, Block, Native };

const char* DmPolicyName(DmPolicy policy);
std::optional<DmPolicy> DmPolicyFromName(const std::string& name);
const char* GroupPolicyName(GroupPolicy policy);
std::optional<GroupPolicy> GroupPolicyFromName(const std::string& name);
const char* ReplyModeName(ReplyMode mode);
std::optional<ReplyMode> ReplyModeFromName(const std::string& name);

// ---------------------------------------------------------------------------
// 账号与渠道配置
// ---------------------------------------------------------------------------

struct ChannelReplyUserConfig {
    ReplyMode mode = ReplyMode::Block;  // 默认 block(平台无 native 能力时)
    bool tool_progress = false;         // 工具进度默认不出站
};

// ---------------------------------------------------------------------------
// binding(阶段 3 路由批;configuration.md §8 的冻结形状)
//
//   {"agent":"ops-agent","match":{"channel":"qqbot","account":"main",
//     "conversation":{"kind":"group","id":"group_openid"}},
//    "policy":{"tools":{"allow":[...],"deny":[...]},
//              "memory":{"user":true,"project":true}}}
//
// 匹配从具体到宽(conversation+thread -> conversation -> account -> channel
// -> default Agent);同档命中两条报冲突,不按文件次序碰运气——这条规矩
// 在 ChannelRouter(channel_router.hpp)执法,这里只管形状与解析。
// ---------------------------------------------------------------------------

struct ChannelBindingConversationMatch {
    std::string kind;  // direct|group|guild|channel|thread;空 = 不限
    std::string id;    // conversation id;空 = 不限
};

struct ChannelBindingMatch {
    std::string channel;  // 渠道层 bindings 里可空(= 本渠道);写了须与所在渠道 id 一致
    std::string account;  // 空 = 该渠道任意账号
    // 无 conversation = account/channel 档;有 conversation 无 thread = conversation 档。
    std::optional<ChannelBindingConversationMatch> conversation;
    std::string thread_id;  // 空 = 不限 thread
};

struct ChannelBindingToolsPolicy {
    std::vector<std::string> allow;  // 空 = 不另设上限(仍受 deny 与上层交集管)
    std::vector<std::string> deny;
};

struct ChannelBindingMemoryPolicy {
    std::optional<bool> user;     // 仅 owner DM 生效(§8 首版默认表)
    std::optional<bool> project;
};

struct ChannelBindingPolicy {
    ChannelBindingToolsPolicy tools;
    std::optional<ChannelBindingMemoryPolicy> memory;
};

struct ChannelBindingConfig {
    std::string agent;  // 空 = 不改 Agent(只收紧策略的 binding 也合法)
    ChannelBindingMatch match;
    ChannelBindingPolicy policy;
};

struct ChannelAccountUserConfig {
    bool enabled = false;
    std::string transport;                // websocket | webhook | long_polling(按 manifest 能力收)
    std::string app_id;
    std::optional<std::string> secret_env;   // 环境变量名(不落值)
    std::optional<std::string> secret_file;  // 用户明指的文件路径
    std::optional<std::string> secret;       // 明文兼容:doctor 报 warning
    DmPolicy dm_policy = DmPolicy::Pairing;
    std::vector<std::string> allow_from;
    GroupPolicy group_policy = GroupPolicy::Allowlist;
    std::vector<std::string> group_allow_from;
    bool require_mention = true;
    bool allow_bots = false;               // 默认拒绝其它 bot(configuration.md §7)
    std::string agent;                     // 绑定的 Agent 名(可空 = default)
    ChannelReplyUserConfig reply;
    // 群聊 session scope(§8):group|group_sender|group_thread|group_thread_sender。默认 group。
    std::string group_scope;
};

struct ChannelUserConfig {
    bool enabled = false;
    std::string default_account;
    std::map<std::string, ChannelAccountUserConfig> accounts;  // key = account id
    // 渠道层 bindings(§8):match.channel 可空 = 本渠道;非空须等于本渠道 id。
    std::vector<ChannelBindingConfig> bindings;
};

// ---------------------------------------------------------------------------
// 凭据来源诊断(不打印值)
// ---------------------------------------------------------------------------

enum class CredentialSource { Missing, FromEnv, FromFile, InlinePlaintext };

const char* CredentialSourceName(CredentialSource source);
// 账号的凭据来源判定(纯配置侧:secret 明文 > secret_env > secret_file >
// 缺失;真实值解析是 manager/doctor 运行时的事)。
CredentialSource DescribeCredentialSource(const ChannelAccountUserConfig& account);

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

// 解析 config.json 顶层 "channels" 段(须传 object)。错误信息带字段路径
// 与文件名,待遇同 config.cpp 的 ParseMcpServersConfig。
std::optional<std::map<std::string, ChannelUserConfig>> ParseChannelsUserConfig(
    const nlohmann::json& channels_json, const std::string& file_path_for_error,
    std::string* error);

}  // namespace lubancode::channel
