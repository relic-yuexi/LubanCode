// IntegrityGate(Token 账本单 §9.3 步骤 1/A4)。
//
// 一间 session 进分析器前的验账闸:目录/封口/链/父子边全过才放行。
// active/corrupt/incomplete 各自单列,排除理由可见(§14.1/§14.2)——
// 不悄悄混进任何分母。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "trajectory/event.hpp"

namespace lubancode::insights {

enum class SessionGateStatus {
    Analyzed,    // 验过且封口:整间可分析
    Active,      // session 未封口(读得到 committed 高水位,标 provisional)
    Incomplete,  // 有 stream 尾行截断(崩溃中断;§16.3)
    Corrupt,     // hash 链断/坏行/父子边对不上
    Missing,     // 目录不存在
};
const char* SessionGateStatusName(SessionGateStatus status);

struct SessionGateReport {
    SessionGateStatus status = SessionGateStatus::Missing;
    std::string error_code;   // gate.* / verify.* 稳定码;空 = 过
    std::string message;      // 人话(报告原样打)
    std::string session_id;   // session.json 读不到时用目录名
    std::string workspace_key;
    std::string session_status;  // session.json 的 status;读不到 unknown
    // run_id -> 终枚事件 hash(stale 判定的源账,§6.5)。
    std::map<std::string, std::string> stream_terminal_hashes;
    // 放行时才有:各 stream 已验事件(run_id,按 seq 升序),stream 按
    // run_id 字典序。Active 也会装(高水位),Corrupt/Incomplete 不装。
    std::vector<std::pair<std::string, std::vector<trajectory::EventEnvelope>>> streams;
    // 排除理由明细(每条 stream 的坏处点名)。
    std::vector<std::string> notes;

    bool sealed() const { return session_status == "closed"; }
};

// 验一间 session。目录不存在 → Missing。链/边验证委托 trajectory 的
// VerifySessionDir(同一只引擎,不另写一套);截断与坏链靠逐文件
// VerifyJournalFile 分开点名。纯读,不回写。
SessionGateReport GateSession(const std::filesystem::path& session_dir);

}  // namespace lubancode::insights
