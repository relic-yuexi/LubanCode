// Session Analyzer(Token 账本单 §9.3/A4):一间 session 的本地确定性
// 分析管线。
//
//   IntegrityGate(验账/封口/坏账排除)
//     -> UsageProjector(每 stream 投 UsageSample)
//     -> PromptAuditor(runtime 层变化)
//     -> FrictionClassifier(摩擦归类)
//     -> FeatureSignalEngine(现成功能信号)
//     -> SessionInsightSummary(派生摘要,原子写,可删可重算)
//
// 口径:坏账整间排除且理由可见;active 默认跳过(include_active 才读
// 高水位,摘要标 provisional、不写长期 session-summary——§14.2 不与
// writer 争);同一封口 session 重算字节相同。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "insights/feature_signal.hpp"
#include "insights/friction_classifier.hpp"
#include "insights/integrity_gate.hpp"
#include "insights/prompt_auditor.hpp"
#include "insights/session_summary.hpp"

namespace lubancode::insights {

struct SessionAnalyzeOptions {
    // 落 derived 摘要(封口 session 才写;active 不写,§14.2)。
    bool write_summary = true;
    // active session 也分析(读已提交高水位,摘要 provisional)。
    bool include_active = false;
};

struct SessionAnalyzeResult {
    SessionGateReport gate;
    bool analyzed = false;       // 进了分析(封口或 include_active 的高水位)
    bool provisional = false;    // active 高水位成色
    SessionInsightSummary summary;         // analyzed 时有
    std::vector<Finding> prompt_findings;  // runtime 层变化 finding(副本)
    std::vector<FrictionOccurrence> frictions;
    std::vector<FeatureSignal> signals;
    std::vector<std::string> warnings;     // 读侧/投影缺口透传
    // 摘要落盘账。
    bool summary_written = false;  // 本轮写了(或重算换新)
    bool summary_reused = false;   // fresh 摘要直接复用(增量,§9.2)
    std::string derived_error;     // 落盘失败的人话(Journal 不受影响)
};

// 分析一间 session(目录)。Missing/Corrupt/Incomplete/不该进的 Active
// → analyzed=false,gate 里的理由原样给调用方(报告单列,不混分母)。
SessionAnalyzeResult AnalyzeSession(const std::filesystem::path& session_dir,
                                    const SessionAnalyzeOptions& options);

// ---- workspace 扫描(A4 的 coverage 面;/prompt audit outcome 与 A5 共用) ----
struct WorkspaceScanEntry {
    std::string session_id;
    SessionGateStatus status = SessionGateStatus::Missing;
    std::string reason;  // error_code + message 摘要
};

struct WorkspaceScanReport {
    std::vector<WorkspaceScanEntry> entries;  // 按目录名字典序
    std::map<std::string, std::int64_t> status_counts;
    std::int64_t sessions_found = 0;
};

// 扫 sessions 根下的 session 目录。now_yyyymmdd 由调用方注入(时钟不进
// 领域层);只收目录名头 8 位落在 [now-since_days, now] 窗内的场。目录名
// 形状不合(不YYYYMMDD 起)的不猜日期——收进来,日期标 unknown(不冒充)。
// 纯 gate 级扫描,不做深分析。
WorkspaceScanReport ScanWorkspaceSessions(const std::filesystem::path& sessions_root,
                                          const std::string& now_yyyymmdd, int since_days);

// ---- /prompt audit outcome 的抬升面(A3 第三层,吃 A4 分析结果) ----
// 把多场 session 的摩擦折叠成 prompt 相关的 finding(§8.3 措辞戒律:
// 只说信号,不越界断言 prompt 错)。finding_id 按类钉(P-AUD-O01…),
// 同输入同输出。input 为空给空表(没分析过就别编)。
std::vector<Finding> AuditPromptOutcome(const std::vector<SessionAnalyzeResult>& sessions);

}  // namespace lubancode::insights
