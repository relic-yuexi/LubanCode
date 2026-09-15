// 渠道配对控制命令(QQ 接入单 Q1b):本地批准/拒绝的文件控制面。
//
// 与 stop.json 同款通道(control/ 命令文件目录,local-only,零 socket 零
// 端口——本机身份由 profile 目录的 user-only 权限承担),但配对命令要
// 回执:CLI 投命令后要等"批没批上"的结果,不是单向停机。故三件:
//   pairing-<command_id>.json         命令(外部 CLI 写;持锁实例消费即删)
//   pairing-<command_id>.result.json  结果(持锁实例写;CLI 读走即删)
//   command_id 由 CLI 生成(单段随机串),两边凭它对上号。
//
// 身份核与陈旧防护同 stop 命令:命令带目标 boot_id,对不上当前实例 = 陈
// 旧命令,删掉不理(防旧命令误杀/误批新实例)。普通交互进程不许拿空
// manager 冒充批准成功——命令只投给锁里活着的 Gateway,投递前的门在
// CLI 侧(锁探测)与 Gateway 侧(boot_id 复核)各一道。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::gateway {

// 一枚配对控制命令。
struct GatewayPairingCommand {
    int schema_version = 1;
    std::string boot_id;     // 目标实例(锁里的;空 = 不指名,慎用)
    std::string command_id;  // 对号串(单段 [A-Za-z0-9-];也作文件名)
    std::string action;      // "approve" | "reject"
    std::string channel_id;  // 如 "qqbot"
    std::string account_id;  // 如 "main"
    std::string token;       // 配对码或 sender 身份(单参数口)
    std::int64_t requested_at_ms = 0;

    nlohmann::json ToJson() const;
    // 严格解析:必填字段齐、类型对、action 在两值内;未知字段忽略。
    static std::optional<GatewayPairingCommand> FromJson(const nlohmann::json& json,
                                                         std::string* error);
};

// 命令 id 合法吗(单段 [A-Za-z0-9-],长度 ≤ 64——它直接拼命令文件名)。
bool IsValidPairingCommandId(const std::string& command_id);

// 写一枚命令(外部 CLI 用)。返回空 = 成功。
std::string WritePairingCommand(const std::filesystem::path& control_dir,
                                const GatewayPairingCommand& command);

// 持锁实例轮询:收走目录里全部 pairing-*.json(消费即删;boot_id 对不上
// 当前实例或读不懂也删——陈旧命令不追新实例)。返回指到本实例的命令。
std::vector<GatewayPairingCommand> PollPairingCommands(const std::filesystem::path& control_dir,
                                                       const std::string& current_boot_id);

// 一枚命令的回执。
struct GatewayPairingCommandResult {
    int schema_version = 1;
    std::string command_id;
    std::string action;
    bool ok = false;
    std::string sender_id;  // 成功时被批准/拒绝的 sender
    std::string error;      // 失败时的 stable reason(not_found/expired/
                            // already_finalized/account_not_found/...)
    std::string detail;     // 人话补充(不带密钥)

    nlohmann::json ToJson() const;
    static std::optional<GatewayPairingCommandResult> FromJson(const nlohmann::json& json,
                                                               std::string* error);
};

// 持锁实例写回执。返回空 = 成功。
std::string WritePairingCommandResult(const std::filesystem::path& control_dir,
                                      const GatewayPairingCommandResult& result);

// CLI 读回执(读到即删,不留陈旧结果)。文件不在 = nullopt 且 error 空。
std::optional<GatewayPairingCommandResult> TakePairingCommandResult(
    const std::filesystem::path& control_dir, const std::string& command_id, std::string* error);

}  // namespace lubancode::gateway
