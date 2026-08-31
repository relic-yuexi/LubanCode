// 派生仓的 session-summary 位(Token 账本单 §9.2/A4)。
//
// <session>/derived/<analyzer>/session-summary.json —— 临时文件 + 原子
// 替换。派生物全可删可重算,不碰 canonical Journal;清理只删
// derived/ 下的东西。stale 判定:analyzer 版本或任一 stream terminal
// hash 变了就算 stale(§6.5),删掉重算,不叠加。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "insights/session_summary.hpp"

namespace lubancode::insights {

// 摘要落位的目录名(与 analyzer 版本同名;升版本另起目录,旧摘要留给
// 历史报告页眉标旧版本用,§14.6)。
inline constexpr const char* kDerivedSummaryDir = "derived/insights-v1";

std::filesystem::path SessionSummaryPath(const std::filesystem::path& session_dir);

struct DerivedWriteResult {
    bool ok = false;
    std::string error_code;  // derived.* 稳定码
    std::string message;
    std::filesystem::path path;
};
// 原子写:derived/<analyzer>/session-summary.json.tmp -> rename。父目录
// 不在就建;rename 失败(跨设备/被占)清掉 tmp 再报错。
DerivedWriteResult WriteSessionSummaryAtomic(const std::filesystem::path& session_dir,
                                             const SessionInsightSummary& summary);

struct DerivedReadResult {
    bool exists = false;
    bool parse_ok = false;
    std::string error;               // 读/解析失败的人话
    SessionInsightSummary summary;   // parse_ok 时才有意义
};
DerivedReadResult ReadExistingSessionSummary(const std::filesystem::path& session_dir);

// stale 判定(§6.5):analyzer 版本或任一 terminal hash 对不上就 stale。
// current_hashes 是刚验过的终 hash 账。existing.parse_ok=false 一律 stale。
bool IsSummaryStale(const DerivedReadResult& existing,
                    const std::map<std::string, std::string>& current_hashes);

}  // namespace lubancode::insights
