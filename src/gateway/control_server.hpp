// Gateway 控制面(总装单 G1):local-only 文件控制通道。
//
// contracts.md §10 实现裁决:首版 control endpoint 落成本地文件通道——
//   control.json   运行中 Gateway 的状态快照(health/readiness),原子写
//                  (temp + rename),状态一变就换新;
//   control/       命令文件目录。stop 命令落 stop.json(带目标 boot_id),
//                  Gateway 主循环轮询消费,boot_id 对不上视为陈旧命令删除
//                  不理会(防旧命令误杀新实例)。
// 零 socket、零端口;local-only 与"本机身份"由 profile 目录的 user-only
// 权限承担(单子 §8.3)。G2 起 status --deep 需要活探针时再升真 endpoint。
//
// 读写两侧的规矩:写侧(GatewayProcess)建目录、原子换代;读侧(status
// 探测)纯读、容错——坏 JSON 不崩,报 gateway.control_unreachable 类诊断。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::gateway {

// 控制快照(control.json 的正文)。
struct GatewayControlSnapshot {
    int schema_version = 1;
    std::string profile;
    std::string boot_id;
    unsigned long pid = 0;
    std::string start_token;
    std::int64_t started_at_ms = 0;
    std::string state;    // starting | running | draining | stopped
    std::string health;   // ok | degraded
    bool safe_mode = false;
    std::string last_shutdown;  // clean | timeout | crashed | ""(未知)
    std::string version;        // lubancode 版本
    std::int64_t updated_at_ms = 0;

    nlohmann::json ToJson() const;
    // 读侧容错解析:字段缺失/类型不对给 nullopt + error(不抛)。未知字段
    // 忽略——快照是 Gateway 自己写的,前向兼容(老 CLI 读新快照)靠这条。
    static std::optional<GatewayControlSnapshot> FromJson(const nlohmann::json& json,
                                                          std::string* error);
};

// 写侧:原子写 control.json(含建父目录;只在 GatewayProcess 的写侧调用)。
// 返回空 = 成功;否则人话错误。
std::string WriteControlSnapshot(const std::filesystem::path& control_file,
                                 const GatewayControlSnapshot& snapshot);

// 读侧:读 control.json。文件不存在 = nullopt 且 error 空;在但读不懂 =
// nullopt 且 error 带说明(坏 control endpoint 的诊断素材)。
std::optional<GatewayControlSnapshot> ReadControlSnapshot(const std::filesystem::path& control_file,
                                                          std::string* error);

// stop 命令文件(control/stop.json)的正文。
struct GatewayStopCommand {
    int schema_version = 1;
    std::string boot_id;         // 目标实例;空 = 不指名(最老语义,慎用)
    std::int64_t requested_at_ms = 0;

    nlohmann::json ToJson() const;
};

// 写一枚 stop 命令(外部 CLI 的 `gateway stop` 用)。返回空 = 成功。
std::string WriteStopCommand(const std::filesystem::path& control_dir,
                             const GatewayStopCommand& command);

// Gateway 主循环轮询:有命令文件时读出、删文件。boot_id 对不上当前实例
// (或读不懂)也删——陈旧命令不追杀新实例。返回 true = 命令指到本实例,
// 该停了。
bool PollStopCommand(const std::filesystem::path& control_dir, const std::string& current_boot_id);

}  // namespace lubancode::gateway
