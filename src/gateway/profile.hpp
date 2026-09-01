// Gateway profile(总装单 G1):profile 名裁决、目录落位与 gateway.json 装载。
//
// 唯一真源 docs/architecture/gateway/README.md §6(目录布局)与 contracts.md
// §10(G1 实现裁决)。三条规矩:
//   - profile 名是单段名(同 trajectory session id 的规矩),不带路径分隔、
//     不带 "..",防 profile 名逃出 profiles/ 目录;
//   - 路径裁决是纯函数:不建任何目录。disabled 零副作用合同(contracts.md
//     §6)要求只读命令(status/stop 探测)在 Gateway 未运行时零建目录零写盘,
//     建目录只发生在 GatewayProcess::Start 的写侧;
//   - gateway.json 严格解析:未知字段/坏类型按 gateway.config_invalid 明报
//     (稳定退出码 3,防 supervisor 无限拉起),缺文件 = 全默认。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lubancode::gateway {

// 默认 profile 名(未指名时)。
inline constexpr std::string_view kDefaultGatewayProfile = "default";

// profile 名合法吗:非空、单段(无 '/' '\\' ".."),字符限 [A-Za-z0-9._-],
// 且不得以 '.' 开头(防 "." ".." ".config" 这类)。
bool IsValidGatewayProfileName(std::string_view name);

// 一个 profile 的全部落位(纯裁决,零 IO、零建目录)。root =
// <home>/.lubancode/gateway;profile_dir = root/profiles/<name>。
struct GatewayProfilePaths {
    std::string name;  // profile 名(非法名时为空)
    std::filesystem::path root;
    std::filesystem::path profile_dir;
    std::filesystem::path config_file;     // profile_dir/gateway.json
    std::filesystem::path lock_file;       // profile_dir/gateway.lock
    std::filesystem::path control_file;    // profile_dir/control.json
    std::filesystem::path control_dir;     // profile_dir/control(命令文件)
    std::filesystem::path boot_history;    // profile_dir/boot-history.jsonl
    std::filesystem::path logs_dir;        // profile_dir/logs
    std::filesystem::path log_file;        // logs_dir/gateway.log
};

// 名字不合法时返回空 root 的paths(调用方先过 IsValidGatewayProfileName;
// 这里的防御只保证不拼出逃逸路径)。
GatewayProfilePaths ResolveGatewayProfilePaths(const std::filesystem::path& root,
                                               std::string_view profile_name);

// profile 配置(gateway.json)。只放"许可与上限"(单子 §14.1 的规矩):
// job spec、occurrence 状态不塞这里。
struct GatewayProfileConfig {
    int schema_version = 1;
    int shutdown_grace_secs = 30;      // graceful shutdown 上限;超限记
                                       // gateway.shutdown_timeout,不假写 clean
    int max_concurrent_sessions = 4;   // G3 headless 并发帽;G1 只装载校验
    int safe_mode_threshold = 3;       // 连续非干净关机几次进 SafeMode

    bool Validate(std::string* error) const;
};

struct GatewayConfigLoad {
    enum class Status { Ok, Missing, Invalid };
    Status status = Status::Missing;
    GatewayProfileConfig config;  // Ok:装载值;Missing:默认值;Invalid:默认值
    std::string error;            // Invalid 时的人话(带稳定码 gateway.config_invalid)
};

// 读 profile 的 gateway.json。缺文件 = Missing(全默认);在但读不懂/认不出
// = Invalid(明报,调用方退稳定码)。
GatewayConfigLoad LoadGatewayConfig(const std::filesystem::path& config_file);

}  // namespace lubancode::gateway
