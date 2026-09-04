// Export Projection(中立投影层):training-v1 与 harness-v1 两只 exporter
// 共用的"Journal 只读扫描"件(Harbor Harness 派生 JSONL 单 §五"复用现有
// Exporter,不另造第二本历史")。
//
// 这里只做两件事:
//   1. raw 扫——fold 不留的字段按 envelope 逐行取(对话面事件、purpose、
//      usage、工具细账、run relations、环境快照引用);
//   2. 正文回读——BlobRef 解引用 + 上限,超限/缺失给稳定结构码。
//
// 纪律与 training_exporter 同源:纯读、不联网、不回写 Journal;secret 扫描
// 复用 insights 冻结的模式表,不在 trajectory 重造第二份。本件是内部件
// (exporter 实现的共享腹部),不进公共 API——依赖铁律不变:trajectory
// 纯库,不 include app/cli/runtime。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "insights/redaction.hpp"
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"

namespace lubancode::trajectory::projection {

// ---------------------------------------------------------------------------
// JSON 取值小工具(只认类型对的键,不猜)
// ---------------------------------------------------------------------------

inline std::string GetString(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string();
}

inline bool GetBool(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_boolean() ? it->get<bool>() : false;
}

inline std::uint64_t GetUint(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    if (it == value.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        return static_cast<std::uint64_t>(it->get<std::int64_t>());
    }
    return 0;
}

// §9.2 四档 -> §11.4 replayability 轴值(两 exporter 同一张表)。
inline std::string MapReplayLevelToAbility(const std::string& level) {
    if (level == "exact") {
        return "exact_offline";
    }
    if (level == "source_exact_environment_partial") {
        return "source_exact";
    }
    if (level == "input_only") {
        return "input_only";
    }
    if (level == "blocked") {
        return "blocked";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 隐私扫描(§十二:exporter 再扫一遍;training 命中进 excluded,harness
// 命中做正文脱敏——两个处置面,一套扫描器)
// ---------------------------------------------------------------------------

struct PrivacyFinding {
    std::string code;  // privacy.secret.<kind> | privacy.personal_path |
                       // privacy.absolute_path | privacy.binary
    std::string source_event_id;
};

// 边界判定:路径打头处须是串首/空白/引号/括号一类非词字符——"src/home/x"
// 与 "https://x" 都不算绝对路径。
inline bool AtTokenBoundary(std::string_view text, std::size_t at) {
    if (at == 0) {
        return true;
    }
    const char c = text[at - 1];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        return true;
    }
    return c == '"' || c == '\'' || c == '`' || c == '(' || c == '[' || c == '{' || c == '<' ||
           c == ',' || c == ';' || c == '=';
}

inline bool IsAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool StartsWithCaseInsensitive(std::string_view text, std::size_t at,
                                      std::string_view needle) {
    if (at + needle.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < needle.size(); ++i) {
        if (LowerAscii(text[at + i]) != LowerAscii(needle[i])) {
            return false;
        }
    }
    return true;
}

// 绝对路径/个人路径扫描。模式集冻结如下,单测钉行为:
//   1) Windows 盘符绝对路径 X:\ 或 X:/(前一字符不得是 scheme 词字符,
//      免得 https:// 的 's:' 误伤);后随 users 段判个人路径;
//   2) 边界打头的 /home/<段>、/Users/<段>——个人路径;
//   3) 边界打头的 /usr /var /tmp /etc /opt /root /mnt /srv /data
//      /workspace——系统绝对路径。
inline void ScanTextForPaths(std::string_view text, const std::string& event_id,
                             std::vector<PrivacyFinding>* findings) {
    const auto push = [&](const char* code) {
        findings->push_back(PrivacyFinding{code, event_id});
    };
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if (IsAsciiAlpha(text[i]) && text[i + 1] == ':' &&
            (text[i + 2] == '\\' || text[i + 2] == '/')) {
            const bool scheme_before =
                i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0 ||
                          text[i - 1] == '+' || text[i - 1] == '-' || text[i - 1] == '.');
            if (!scheme_before) {
                const std::size_t rest = i + 3;
                if (StartsWithCaseInsensitive(text, rest, "users/") ||
                    StartsWithCaseInsensitive(text, rest, "users\\")) {
                    push("privacy.personal_path");
                } else {
                    push("privacy.absolute_path");
                }
            }
        }
    }
    static const char* kPosixPersonal[] = {"/home/", "/Users/"};
    for (const char* marker : kPosixPersonal) {
        std::size_t at = text.find(marker);
        while (at != std::string_view::npos) {
            if (AtTokenBoundary(text, at) && at + std::string_view(marker).size() < text.size()) {
                push("privacy.personal_path");
            }
            at = text.find(marker, at + 1);
        }
    }
    static const char* kPosixSystem[] = {"/usr/", "/var/", "/tmp/",  "/etc/", "/opt/",
                                         "/root/", "/mnt/", "/srv/", "/data/", "/workspace/"};
    for (const char* marker : kPosixSystem) {
        std::size_t at = text.find(marker);
        while (at != std::string_view::npos) {
            if (AtTokenBoundary(text, at)) {
                push("privacy.absolute_path");
            }
            at = text.find(marker, at + 1);
        }
    }
}

// 二进制误入:NUL 字节、非法 UTF-8、控制字符占比超三成(§十二)。
inline bool LooksLikeBinary(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return true;
    }
    if (!IsValidUtf8(text)) {
        return true;
    }
    if (text.empty()) {
        return false;
    }
    std::size_t controls = 0;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            ++controls;
        }
    }
    return controls * 10 > text.size() * 3;
}

// 一段要进导出件的正文的全套隐私扫描。secret 扫描复用 insights 冻结的
// 模式表(Token 账本单 A0),不在 trajectory 重造第二份(telemetry
// redactor 同款先例)。
inline void ScanTextForPrivacy(std::string_view text, const std::string& event_id,
                               std::vector<PrivacyFinding>* findings) {
    for (const auto& hit : insights::ScanSecrets(text)) {
        findings->push_back(PrivacyFinding{
            std::string("privacy.secret.") + insights::SecretKindName(hit.kind), event_id});
    }
    ScanTextForPaths(text, event_id, findings);
    if (LooksLikeBinary(text)) {
        findings->push_back(PrivacyFinding{"privacy.binary", event_id});
    }
}

// 去重保序(同码只留首枚,source event id 保留第一现场)。
inline std::vector<PrivacyFinding> DedupeFindings(const std::vector<PrivacyFinding>& findings) {
    std::vector<PrivacyFinding> out;
    std::set<std::string> seen;
    for (const auto& finding : findings) {
        if (seen.insert(finding.code).second) {
            out.push_back(finding);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// raw 扫(fold 不留的字段;harness-v1 需要的细账也在这层取)
// ---------------------------------------------------------------------------

struct CallRaw {
    std::string turn_id;
    std::string tool_name;
    bool has_side_effects = false;   // 终态 payload 带 side_effects 且非空
    std::uint64_t terminal_seq = 0;  // 终态事件 seq(证据新鲜度的对照线)
    // ---- harness-v1 细账(training 不消费,纯增量)----
    std::string terminal_kind;         // "tool.execution.finished" 等完整 kind 名
    std::string terminal_event_id;     // 终态事件 id
    nlohmann::json terminal_payload = nlohmann::json();  // 终态 payload 原样
    nlohmann::json effective_payload = nlohmann::json();  // tool.input.effective payload
};

struct TurnRaw {
    // 对话面事件按 seq 单列交错(tool 结果须紧跟带 tool_call 的 assistant,
    // 训练样本的配对序就是事件序)。每枚带 tag:event_id/seq/policy 与正文。
    std::vector<nlohmann::json> conversation;  // {"tag":"input"|"output"|"result", ...}
    std::vector<nlohmann::json> contexts;      // {context_id, kind, content_ref, origin}
    std::map<std::string, std::string> verification_events;  // verification_id -> recorded 事件 id
    nlohmann::json assessed = nlohmann::json();  // 末枚 outcome.assessed payload
    std::uint64_t last_mutating_seq = 0;         // 带副作用工具的终态 seq 高水位
    std::string claimed_outcome;                 // turn 终态 payload 自称的 outcome/reason
    std::string terminal_reason;                 // turn 终态 payload 的 reason(failed/cancelled)
};

struct RawStreamScan {
    std::string run_id;
    std::string workspace_key;
    std::string session_id;
    std::string run_kind_name;
    std::string replay_level;
    nlohmann::json gaps = nlohmann::json::array();
    std::int64_t last_wall_time_ms = 0;
    std::map<std::string, std::string> request_purpose;  // request_id -> purpose
    std::map<std::string, CallRaw> calls;
    std::map<std::string, TurnRaw> turns;
    // ---- harness-v1 细账(training 不消费,纯增量)----
    std::string parent_run_id;     // run.started relations 报的 owner(子账)
    std::string parent_call_id;    // relations.parent_call_id(前台派工才有)
    nlohmann::json env_snapshot_ref = nlohmann::json();  // run.environment.captured 的 snapshot_ref
    std::map<std::string, nlohmann::json> usage;         // request_id -> model.usage.recorded payload
    std::map<std::string, nlohmann::json> tool_results;  // call_id -> tool.result.committed payload
};

inline RawStreamScan ScanStreamRaw(const std::filesystem::path& stream_path) {
    RawStreamScan scan;
    const auto lines = ReadJournalLines(stream_path);
    if (!lines.has_value()) {
        return scan;
    }
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            continue;  // 链/schema 坏由 fold 报告,raw 扫只取可用字段
        }
        const std::string kind = GetString(parsed, "kind");
        const std::string turn_id = GetString(parsed, "turn_id");
        const std::string event_id = GetString(parsed, "event_id");
        const std::string origin = GetString(parsed, "origin");
        const nlohmann::json& payload =
            parsed.contains("payload") && parsed["payload"].is_object() ? parsed["payload"]
                                                                       : nlohmann::json::object();
        scan.last_wall_time_ms = static_cast<std::int64_t>(GetUint(parsed, "wall_time_ms"));
        if (kind == "run.started") {
            if (scan.run_id.empty()) {
                scan.run_id = GetString(parsed, "run_id");
                scan.workspace_key = GetString(parsed, "workspace_key");
                scan.session_id = GetString(parsed, "session_id");
                scan.run_kind_name = GetString(parsed, "run_kind");
            }
            // 子账身份:run.started 的 relations 带 parent_run_id /(前台
            // 派工)parent_call_id(verifier 同一口径)。
            if (parsed.contains("relations") && parsed["relations"].is_object()) {
                const std::string parent = GetString(parsed["relations"], "parent_run_id");
                if (!parent.empty()) {
                    scan.parent_run_id = parent;
                }
                const std::string parent_call = GetString(parsed["relations"], "parent_call_id");
                if (!parent_call.empty()) {
                    scan.parent_call_id = parent_call;
                }
            }
        } else if (kind == "run.environment.captured") {
            scan.replay_level = GetString(payload, "replay_level");
            if (payload.contains("gaps") && payload["gaps"].is_array()) {
                scan.gaps = payload["gaps"];
            }
            if (payload.contains("snapshot_ref") && payload["snapshot_ref"].is_object()) {
                scan.env_snapshot_ref = payload["snapshot_ref"];
            }
        } else if (kind == "model.request.prepared") {
            const std::string request_id = GetString(parsed, "request_id");
            if (!request_id.empty()) {
                scan.request_purpose[request_id] = GetString(payload, "purpose");
            }
        } else if (kind == "model.usage.recorded") {
            // v2 usage canonical owner:harness 的 requests[] 从这取数
            // (input/cache 分列,attempt/reported 标志一并原样)。
            const std::string request_id = GetString(parsed, "request_id");
            if (!request_id.empty()) {
                scan.usage[request_id] = payload;
            }
        } else if (kind == "input.received") {
            TurnRaw& turn = scan.turns[turn_id];
            turn.conversation.push_back(nlohmann::json{
                {"tag", "input"},
                {"seq", GetUint(parsed, "seq")},
                {"event_id", event_id},
                {"origin", origin},
                {"actor", GetString(parsed, "actor")},
                {"channel", GetString(payload, "channel")},
                {"content", payload.contains("content") && payload["content"].is_array()
                                ? payload["content"]
                                : nlohmann::json::array()},
                {"policy", GetString(parsed, "training_policy")}});
        } else if (kind == "context.attached") {
            TurnRaw& turn = scan.turns[turn_id];
            turn.contexts.push_back(nlohmann::json{
                {"context_id", GetString(payload, "context_id")},
                {"context_kind", GetString(payload, "context_kind")},
                {"content_ref", payload.contains("content_ref") && payload["content_ref"].is_object()
                                    ? payload["content_ref"]
                                    : nlohmann::json()},
                {"origin", origin}});
        } else if (kind == "model.output.completed") {
            TurnRaw& turn = scan.turns[turn_id];
            turn.conversation.push_back(nlohmann::json{
                {"tag", "output"},
                {"seq", GetUint(parsed, "seq")},
                {"event_id", event_id},
                {"request_id", GetString(parsed, "request_id")},
                {"blocks", payload.contains("blocks") && payload["blocks"].is_array()
                               ? payload["blocks"]
                               : nlohmann::json::array()},
                {"stop_reason", GetString(payload, "stop_reason")},
                {"policy", GetString(parsed, "training_policy")}});
        } else if (kind == "tool.result.committed") {
            TurnRaw& turn = scan.turns[turn_id];
            turn.conversation.push_back(nlohmann::json{
                {"tag", "result"},
                {"seq", GetUint(parsed, "seq")},
                {"event_id", event_id},
                {"call_id", GetString(payload, "call_id")},
                {"content", payload.contains("content") && payload["content"].is_array()
                                ? payload["content"]
                                : nlohmann::json::array()},
                {"is_error", GetBool(payload, "is_error")},
                {"policy", GetString(parsed, "training_policy")}});
            const std::string call_id = GetString(payload, "call_id");
            if (!call_id.empty()) {
                scan.tool_results[call_id] = payload;
            }
        } else if (kind == "tool.execution.planned" || kind == "tool.input.effective") {
            CallRaw& call = scan.calls[GetString(payload, "call_id")];
            if (call.turn_id.empty()) {
                call.turn_id = turn_id;
            }
            if (call.tool_name.empty()) {
                call.tool_name = GetString(payload, "tool_name");
            }
            if (kind == "tool.input.effective") {
                call.effective_payload = payload;
            }
        } else if (kind == "tool.execution.finished" || kind == "tool.execution.failed" ||
                   kind == "tool.execution.cancelled" || kind == "tool.execution.unknown") {
            std::string call_id = GetString(payload, "call_id");
            if (call_id.empty()) {
                call_id = GetString(parsed, "call_id");
            }
            CallRaw& call = scan.calls[call_id];
            if (call.turn_id.empty()) {
                call.turn_id = turn_id;
            }
            if (call.tool_name.empty()) {
                call.tool_name = GetString(payload, "tool_name");
            }
            call.terminal_seq = GetUint(parsed, "seq");
            call.terminal_kind = kind;
            call.terminal_event_id = event_id;
            call.terminal_payload = payload;
            if (payload.contains("side_effects") && payload["side_effects"].is_array() &&
                !payload["side_effects"].empty()) {
                call.has_side_effects = true;
            }
        } else if (kind == "turn.completed" || kind == "turn.failed" || kind == "turn.cancelled") {
            TurnRaw& turn = scan.turns[turn_id];
            std::string claimed = GetString(payload, "outcome");
            if (claimed.empty()) {
                claimed = GetString(payload, "reason");
            }
            turn.claimed_outcome = claimed;
            turn.terminal_reason = GetString(payload, "reason");
        } else if (kind == "verification.recorded") {
            TurnRaw& turn = scan.turns[turn_id];
            turn.verification_events[GetString(payload, "verification_id")] = event_id;
        } else if (kind == "outcome.assessed") {
            scan.turns[turn_id].assessed = payload;
        }
        // 带副作用工具的终态 seq 高水位:证据新鲜度的对照线(§11.5
        // "evidence 晚于相关最后修改")。
        if ((kind == "tool.execution.finished" || kind == "tool.execution.failed" ||
             kind == "tool.execution.unknown") &&
            payload.contains("side_effects") && payload["side_effects"].is_array() &&
            !payload["side_effects"].empty()) {
            scan.turns[turn_id].last_mutating_seq =
                std::max(scan.turns[turn_id].last_mutating_seq, GetUint(parsed, "seq"));
        }
    }
    return scan;
}

// ---------------------------------------------------------------------------
// 文本回读(超限正文在 Journal 里是 BlobRef;§11.5 缺 blob 不进 success)
// ---------------------------------------------------------------------------

struct ResolvedText {
    bool ok = false;
    std::string text;
    std::string structure_code;  // blob_missing | blob_oversized(空 = 成)
};

inline ResolvedText ResolveTextValue(const nlohmann::json& value, const BlobStore& blobs,
                                     std::uint64_t max_resolved_blob_bytes) {
    ResolvedText out;
    if (value.is_string()) {
        out.ok = true;
        out.text = value.get<std::string>();
        return out;
    }
    const auto ref = BlobRef::FromJson(value);
    if (!ref.has_value()) {
        // 非字符串也非 BlobRef 的异形:fold 已拦过形状,这里不猜。
        out.structure_code = "blob_missing";
        return out;
    }
    if (ref->size > max_resolved_blob_bytes) {
        out.structure_code = "blob_oversized";
        return out;
    }
    const auto read = blobs.ReadVerified(*ref);
    if (!read.has_value()) {
        out.structure_code = "blob_missing";
        return out;
    }
    out.ok = true;
    out.text = *read;
    return out;
}

// ---------------------------------------------------------------------------
// stream 清单(VerifySessionDir 同一套;相对路径字典序,跨平台确定)
// ---------------------------------------------------------------------------

inline std::vector<std::filesystem::path> CollectSessionStreams(
    const std::filesystem::path& session_dir) {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    const auto scan_dir = [&paths](const std::filesystem::path& dir) {
        std::error_code inner_ec;
        if (!std::filesystem::exists(dir, inner_ec)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, inner_ec)) {
            if (entry.is_regular_file(inner_ec) && entry.path().extension() == ".jsonl") {
                paths.push_back(entry.path());
            }
        }
    };
    if (std::filesystem::exists(session_dir / "main.jsonl", ec)) {
        paths.push_back(session_dir / "main.jsonl");
    }
    scan_dir(session_dir / "subagents");
    scan_dir(session_dir / "goals");
    scan_dir(session_dir / "loops");
    const auto workflows = session_dir / "workflows";
    if (std::filesystem::exists(workflows, ec)) {
        for (const auto& run : std::filesystem::directory_iterator(workflows, ec)) {
            if (!run.is_directory(ec)) {
                continue;
            }
            const auto stream = run.path() / "workflow.jsonl";
            if (std::filesystem::exists(stream, ec)) {
                paths.push_back(stream);
            }
            scan_dir(run.path() / "nodes");
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

// ---------------------------------------------------------------------------
// 原子写(统一走 platform::AtomicWriteFile:同目录唯一临时件 + 平台原子
// 替换;§12.1 换名不越 session 根)。harness 落用户点名路径时升
// ProcessCrashDurability(评测件,flush 落盘再换名)。
// ---------------------------------------------------------------------------

inline bool WriteTextAtomically(const std::filesystem::path& target, const std::string& content,
                                std::string* error,
                                platform::WriteDurability durability =
                                    platform::WriteDurability::AtomicVisibility) {
    const auto written = platform::AtomicWriteFile(target, content, durability);
    if (!written.has_value()) {
        *error = written.error().code + ": " + written.error().message;
        return false;
    }
    return true;
}

}  // namespace lubancode::trajectory::projection
