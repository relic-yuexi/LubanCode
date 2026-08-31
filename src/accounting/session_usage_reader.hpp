// 一场 session 的 Journal -> UsageSample 装配(Token 账本单 A2)。
//
// /usage 的读侧:认领 session 目录,把 main/subagents/workflows 各条
// stream 逐条读回、逐条 ProjectUsage,汇成一包 samples。纯读——writer
// 持句柄照读(journal 以共享读开),不 flush、不回写、不补造事实。
//
// active session 直接读已提交高水位,成色由调用方标 provisional;这里只
// 如实汇报 session.json 的 status 与读到的东西。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "accounting/usage_sample.hpp"

namespace lubancode::accounting {

struct SessionUsageRead {
    bool ok = false;
    std::string error_code;   // usage.* 稳定码
    std::string message;      // 人话(报告原样打)
    std::string session_id;   // 目录名(session.json 读不到时兜底)
    std::string workspace_key;
    std::string status;       // session.json 的 status;读不到写 unknown
    std::vector<UsageSample> samples;   // 各 stream 按 run 字典序、stream 内出现序
    std::vector<std::string> warnings;  // 投影 warnings 透传(usage.purpose_missing…)
    // session.json 存在且 status=closed:封口账;其余(active/读不到)调
    // 用方一律按 provisional 口径标,不在这里猜"应该封了"。
    bool sealed() const { return status == "closed"; }
};

// 列一场 session 的全部 stream 文件(main/subagents/workflows;字典序)。
// 目录不存在给 nullopt(调用方按"没有这场 session"报,不算账错)。
std::optional<std::vector<std::filesystem::path>> ListSessionStreams(
    const std::filesystem::path& session_dir);

// 读一场 session 并投影。session 目录不存在 → ok=false、
// error_code=usage.session_not_found,不产残账。某条 stream 坏/版本混写
// → 该条 stream 的 samples 不算数,warning 点名,其余照读——一场 session
// 一条坏 stream 不至于整场没账,但坏处必须看得见。
SessionUsageRead ReadSessionUsage(const std::filesystem::path& session_dir);

}  // namespace lubancode::accounting
