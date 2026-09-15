// IntegrityGate(Token 账本单 §9.3 步骤 1/A4;v3 半场 T14)。
//
// 一间 session 进分析器前的验账闸:目录/封口/链/父子边全过才放行。
// active/corrupt/incomplete 各自单列,排除理由可见(§14.1/§14.2)——
// 不悄悄混进任何分母。
//
// v3(T14/细化单 §3.4):ProbeV3SessionStream 分派;v3 走 ReadV3Ledger/
// WalkSessionTree 同一只验卷引擎,投出领域读模型(v3_facts),prompt/
// friction/usage 下游只吃那份;v2 老路保留(旧档消费,T02-B 退役)。
// v3 场无 session.json——封口唯一事实是账面 session.ended;子账坏/缺/
// 环在 notes 点名(partial),不跳整场;不认的格式标 Unsupported,不把
// 旧档伪装成空会话,也不迁用户旧档。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "insights/v3_facts.hpp"
#include "trajectory/event.hpp"

namespace lubancode::insights {

enum class SessionGateStatus {
    Analyzed,     // 验过且封口:整间可分析
    Active,       // session 未封口(读得到 committed 高水位,标 provisional)
    Incomplete,   // 有 stream 尾行截断(崩溃中断;§16.3)
    Corrupt,      // hash 链断/坏行/父子边对不上
    Missing,      // 目录不存在
    Unsupported,  // 格式认不出/两种主账打架(v3 探针;不迁旧档,§一)
};
const char* SessionGateStatusName(SessionGateStatus status);

struct SessionGateReport {
    SessionGateStatus status = SessionGateStatus::Missing;
    std::string error_code;   // gate.* / verify.* 稳定码;空 = 过
    std::string message;      // 人话(报告原样打)
    std::string session_id;   // session.json 读不到时用目录名(v3 用账首行)
    std::string workspace_key;
    std::string session_status;  // session.json 的 status;读不到 unknown;
                                 // v3 无 manifest,按账面事实另记
    // 账格式:"v2"|"v3"|""(错误路径)。聚合层按它分开完整率分母。
    std::string format;
    // run_id -> 终枚事件 hash(stale 判定的源账,§6.5);v3 按
    // session_id:run_id 记末行 lineHash(树内去重,每账一条)。
    std::map<std::string, std::string> stream_terminal_hashes;
    // 放行时才有:v2 的各 stream 已验事件(run_id,按 seq 升序),stream
    // 按 run_id 字典序。Active 也会装(高水位),Corrupt/Incomplete 不装。
    std::vector<std::pair<std::string, std::vector<trajectory::EventEnvelope>>> streams;
    // 放行时才有(format=v3):领域读模型(主账+树内子账,缺口见 notes)。
    std::vector<V3SessionFacts> v3_facts;
    // 排除理由明细(每条 stream 的坏处点名;v3 子账 partial 也在这里)。
    std::vector<std::string> notes;

    // v2:session.json status=closed;v3:账面 session.ended(v3 场的封口
    // 唯一事实)。其余(active/读不到)调用方一律按 provisional 口径标。
    bool sealed() const;
};

// 验一间 session。目录不存在 → Missing。链/边验证委托 trajectory 的
// VerifySessionDir(v2 老路;不另写一套);v3 走 ReadV3Ledger/WalkSessionTree
// (T00 读面)。截断与坏链逐文件分开点名。纯读,不回写。
SessionGateReport GateSession(const std::filesystem::path& session_dir);

}  // namespace lubancode::insights
