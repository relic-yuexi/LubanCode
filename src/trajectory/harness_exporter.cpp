// Harness Exporter 实现(One-shot 轨迹指定输出单)。合同见
// harness_exporter.hpp。
//
// 引擎两层,全部纯读:
//   1. FoldStreamReplay——验链 + 折叠(turns/tools/requests/integrity),
//      结构权威;验链不过的流出一行 outcome=unknown 存根,不折正文;
//   2. projection 共享层的 raw 扫——purpose/usage/工具细账/run relations/
//      环境快照引用逐行取,与 training exporter 同一本底账。
//
// 隐私(单子 §四/§八):secret 扫描吃 privacy/secret_scan 中立件(FD-06
// 下沉,原 insights 冻结的模式表),命中片段整段替换(RedactSecrets),
// 记录侧留 privacy_findings 稳定码——与 training 的"整包扣下"不同,
// harness 要如实产出,处置面是脱敏不是丢行。
// 路径不脱敏:评测 harness 要认容器内路径,secret 才是红线。

#include "trajectory/harness_exporter.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "privacy/secret_scan.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/directory.hpp"       // ReadSessionJson:v3 场 workspace_key 兜底(v3 信封不带)
#include "trajectory/event.hpp"           // RunKindName
#include "trajectory/export_projection.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/v3/reader.hpp"        // V3Ledger/FoldToolActions/ExpandResultPreview/FoldPressureFacts
#include "trajectory/v3/session_switch.hpp"  // ProbeV3SessionStream(经 export_projection 的 DiscoverSessionStreams)

namespace lubancode::trajectory {
namespace {

using namespace projection;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// UTC ISO-8601(毫秒):source.exported_at 的收据事实。墙钟进导出件只此
// 一处——training-v1 的字节重放承诺不适用于 harness 导出(§四点名要
// 导出时间),完整性由整文件 SHA-256 背书。
std::string NowUtcIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                        .count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms));
    return buffer;
}

// 按字节上限截正文,末尾不劈开 UTF-8 序列(劈开的坏字节会让 canonical
// dump 抛 invalid UTF-8)。
std::string CutUtf8(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    std::size_t cut = limit;
    // 最多回退 3 字节(UTF-8 序列最长 4 字节,劈口后随的连续字节数 ≤3)。
    for (int back = 0; back < 3 && cut > 0; ++back) {
        if ((static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        } else {
            break;
        }
    }
    return std::string(text.substr(0, cut));
}

// 正文脱敏 + 缘由账:harness 的处置面是替换命中片段,不是丢整块(§四
// "即便失败或 partial 也要如实产出")。stable codes 进 findings。
std::string RedactSecretsAndReport(std::string_view text, const std::string& event_id,
                                   std::vector<PrivacyFinding>* findings) {
    for (const auto& hit : privacy::ScanSecrets(text)) {
        findings->push_back(PrivacyFinding{
            std::string("privacy.secret.") + privacy::SecretKindName(hit.kind), event_id});
    }
    return privacy::RedactSecrets(text);
}

// JSON 值的脱敏:canonical dump -> RedactSecrets -> 回读。结构保住,命中
// 的字符串值整段替换(工具入参/配置快照走这条路)。
nlohmann::json RedactJsonValue(const nlohmann::json& value, const std::string& event_id,
                               std::vector<PrivacyFinding>* findings) {
    const auto dumped = CanonicalJsonDump(value);
    if (!dumped.has_value()) {
        return value;
    }
    const std::string redacted = RedactSecretsAndReport(*dumped, event_id, findings);
    if (redacted == *dumped) {
        return value;
    }
    const auto parsed = nlohmann::json::parse(redacted, nullptr, false);
    return parsed.is_discarded() ? nlohmann::json{{"redacted", true}} : parsed;
}

// ---------------------------------------------------------------------------
// 正文块投影
// ---------------------------------------------------------------------------

// 一段带正文的块(text/thinking):解 blob -> 脱敏 ->(超工具上限时)head
// 摘要 + 引用。块类型原样保留(text 的引用叫 text_ref,thinking 的叫
// thinking_ref)。
nlohmann::json ProjectTextBlock(const std::string& block_type, const nlohmann::json& block,
                                const BlobStore& blobs, const HarnessExportOptions& options,
                                const std::string& event_id, std::vector<PrivacyFinding>* findings,
                                std::uint64_t inline_cap) {
    const ResolvedText resolved = ResolveTextValue(
        block.contains("text") ? block["text"] : nlohmann::json(), blobs,
        options.max_resolved_blob_bytes);
    if (!resolved.ok) {
        return nlohmann::json{{"type", block_type + "_ref"},
                              {"omitted_reason", resolved.structure_code}};
    }
    const std::string redacted = RedactSecretsAndReport(resolved.text, event_id, findings);
    if (redacted.size() > inline_cap) {
        return nlohmann::json{{"type", block_type + "_ref"},
                              {"truncated", true},
                              {"bytes", redacted.size()},
                              {"head", CutUtf8(redacted, 512)}};
    }
    return nlohmann::json{{"type", block_type}, {"text", redacted}};
}

// ---------------------------------------------------------------------------
// V3 引擎(单子 §二.3:V3 → harness-v1 真投影)。
//
// v3 无 turn/run 生命周期事件(docs/architecture/trajectory-v3-schema.md
// §381 行:"不沿用 v2 run 生命周期事件……开场=首行 system+session.started,
// 封口=session.ended"),不借道 FoldStreamReplay/ScanStreamRaw——两代事件
// 模型字段不兼容,硬塞只会产出假成功(单子明令"不硬塞 V2 replay")。这里
// 直接吃 V3Ledger(ReadV3Ledger 已验链)+ FoldToolActions(actionId 折叠),
// messages/tools/requests/usage/environment/outcome 各自现拼;字段不兼容
// 处(turn 终态、outcome 分型)用 v3.* 前缀显式版本化,不冒充 v2 同名值。
// ---------------------------------------------------------------------------

// 折进会话正文的判据(v2 PurposeFoldsIntoConversation 的 v3 对应):只有
// conversation purpose 才是真对话——compact/起名/抽取/验收/摘要一类宿主
// 内部回合不进 messages[](与 v2 同一条界线,词表换成 v3 枚举)。
bool V3PurposeFoldsIntoConversation(v3::MessagePurpose purpose) {
    return purpose == v3::MessagePurpose::Conversation;
}

// 环境快照(T11-C 事实 session.environment.captured):取最后一枚(捕获
// 幂等,一场一次;仍取最后以防迁移/重放场例外)。快照 blob 结构与 v2
// run.environment.captured 同一台机器(BuildEnvironmentCapturePayload 共
// 用),字段名一致——provider/wire/model/model_parameters/
// config_snapshot_redacted。没采集的场如实报 snapshot_available=false,
// 不拿今天环境补昨天事实。
nlohmann::json BuildV3Environment(const v3::V3Ledger& ledger, const BlobStore& blobs,
                                  std::vector<PrivacyFinding>* findings) {
    nlohmann::json environment;
    const v3::EventLine* captured = nullptr;
    for (const auto& event : ledger.events) {
        if (event.kind == v3::EventKindV3::SessionEnvironmentCaptured) {
            captured = &event;
        }
    }
    if (captured == nullptr) {
        environment["snapshot_available"] = false;
        return environment;
    }
    environment["replay_level"] = GetString(captured->payload, "replayLevel");
    environment["gaps"] = captured->payload.contains("gaps") && captured->payload["gaps"].is_array()
                              ? captured->payload["gaps"]
                              : nlohmann::json::array();
    const auto ref = captured->payload.contains("snapshotRef")
                         ? BlobRef::FromJson(captured->payload["snapshotRef"])
                         : std::nullopt;
    if (!ref.has_value()) {
        environment["snapshot_available"] = false;
        return environment;
    }
    const auto read = blobs.ReadVerified(*ref);
    if (!read.has_value()) {
        environment["snapshot_available"] = false;
        return environment;
    }
    const auto snapshot = nlohmann::json::parse(*read, nullptr, false);
    if (snapshot.is_discarded() || !snapshot.is_object()) {
        environment["snapshot_available"] = false;
        return environment;
    }
    environment["lubancode_version"] = GetString(snapshot, "lubancode_version");
    environment["provider"] = GetString(snapshot, "provider");
    environment["wire"] = GetString(snapshot, "wire");
    environment["model"] = GetString(snapshot, "model");
    environment["model_parameters"] =
        snapshot.contains("model_parameters") && snapshot["model_parameters"].is_object()
            ? snapshot["model_parameters"]
            : nlohmann::json::object();
    if (snapshot.contains("config_snapshot_redacted")) {
        environment["config_snapshot_redacted"] =
            RedactJsonValue(snapshot["config_snapshot_redacted"], "environment", findings);
    }
    return environment;
}

// v3 message.content 两种合法形状(生产写法注记,V3RecordInput/
// AppendToolMessage 同款:"单块纯文本落 string,读取投影两读法都认";
// 多块才用数组)——本函数把 string/BlobRef 单块与数组多块统一折成
// harness 的块数组,不因形状漏读一种(字段不兼容处明确版本化,不是漏)。
nlohmann::json V3ProjectMessageContent(const nlohmann::json& content, const BlobStore& blobs,
                                       const HarnessExportOptions& options, const std::string& event_id,
                                       std::vector<PrivacyFinding>* findings, std::uint64_t inline_cap) {
    nlohmann::json out = nlohmann::json::array();
    if (content.is_string() || (content.is_object() && BlobRef::FromJson(content).has_value())) {
        const nlohmann::json synthetic = nlohmann::json{{"text", content}};
        out.push_back(ProjectTextBlock("text", synthetic, blobs, options, event_id, findings, inline_cap));
        return out;
    }
    if (!content.is_array()) {
        return out;  // 异形内容(既非 string/BlobRef 也非数组):不猜,空块
    }
    for (const auto& block : content) {
        if (!block.is_object()) {
            continue;
        }
        const std::string type = GetString(block, "type");
        if (type == "text") {
            out.push_back(ProjectTextBlock("text", block, blobs, options, event_id, findings, inline_cap));
        } else if (type == "thinking") {
            if (options.include_thinking) {
                out.push_back(
                    ProjectTextBlock("thinking", block, blobs, options, event_id, findings, inline_cap));
            } else {
                out.push_back(nlohmann::json{{"type", "thinking_ref"},
                                             {"omitted_reason", "thinking_not_authorized"},
                                             {"source_event_id", event_id}});
            }
        } else {
            out.push_back(block);
        }
    }
    return out;
}

// v3 assistant 消息的 tool_calls 块是 OpenAI 形状(§4.15 声明块:
// {id,type:"function",function:{name,arguments(json 字符串)}}),与 v2
// harness 输出的 {call_id,name,arguments(对象)} 不同——这里是"字段不兼容
// 处明确版本化"的一个实例,解析后照样落 call_id/name/arguments 三键
//(下游读者看到的形状一致,只是来源解析方式不同,arguments 一律已还原
// 成对象并脱敏)。
nlohmann::json V3ProjectToolCallBlock(const nlohmann::json& call, const std::string& event_id,
                                      std::vector<PrivacyFinding>* findings) {
    nlohmann::json out;
    out["call_id"] = GetString(call, "id");
    const auto function = call.find("function");
    if (function != call.end() && function->is_object()) {
        out["name"] = GetString(*function, "name");
        const auto args_it = function->find("arguments");
        nlohmann::json arguments = nlohmann::json::object();
        if (args_it != function->end() && args_it->is_string()) {
            const auto parsed = nlohmann::json::parse(args_it->get<std::string>(), nullptr, false);
            arguments = parsed.is_discarded() ? nlohmann::json(args_it->get<std::string>())
                                              : std::move(parsed);
        } else if (args_it != function->end()) {
            arguments = *args_it;
        }
        out["arguments"] = RedactJsonValue(arguments, event_id, findings);
    } else {
        out["name"] = std::string();
        out["arguments"] = nlohmann::json::object();
    }
    return out;
}

// 该 turn_id 组内最后一枚对话面消息的 completion_status(缺省视为
// complete,§4.43"completion_status 缺省 complete")。找不到已完成的
// assistant 回合(整轮还在进行/宿主崩在半路)给 nullopt——turn 判"open"。
std::optional<v3::CompletionStatus> V3LastConversationCompletion(
    const v3::V3Ledger& ledger, const std::string& turn_id, bool* found_assistant) {
    *found_assistant = false;
    std::optional<v3::CompletionStatus> status;
    for (const auto& entry : ledger.timeline) {
        if (!entry.is_message) {
            continue;
        }
        const v3::MessageLine& message = ledger.messages[entry.index];
        if (!message.turn_id.has_value() || *message.turn_id != turn_id) {
            continue;
        }
        if (message.purpose != v3::MessagePurpose::Conversation) {
            continue;
        }
        if (GetString(message.message, "role") != "assistant") {
            continue;
        }
        *found_assistant = true;
        status = message.completion_status.value_or(v3::CompletionStatus::Complete);
    }
    return status;
}

// turn 终态 token(§二.3"字段不兼容处明确版本化"):v3 没有 turn.completed/
// failed/cancelled 事件,不冒充 v2 同名值,一律 v3.turn_* 前缀。
const char* V3TurnTerminalToken(const std::optional<v3::CompletionStatus>& status, bool found_assistant) {
    if (!found_assistant) {
        return "v3.turn_open";  // 尚无模型回合收口(进行中/宿主崩在半路)
    }
    switch (status.value_or(v3::CompletionStatus::Complete)) {
        case v3::CompletionStatus::Complete:
            return "v3.turn_complete";
        case v3::CompletionStatus::Interrupted:
            return "v3.turn_interrupted";
        case v3::CompletionStatus::Truncated:
            return "v3.turn_truncated";
    }
    return "v3.turn_open";
}

// outcome 分型(v3 版,§二.3/§四:未知或残缺标 partial/unknown,不伪装
// 成功;不借 v2 ClassifyHarnessOutcome 的 run.completed/failed/cancelled
// 词表——v3 没有这些事件,硬套即造假)。判据全部取自账上真实存在的行:
//   1. 没有 session.ended:整场未封口(崩溃/被杀在半路),partial;
//   2. 有工具处于 failed/result_missing/selected_no_message/
//      message_not_admitted(FoldToolActions 折叠出的缺口态):failure;
//   3. 最后一枚对话面 assistant 回合 completion_status==interrupted:
//      cancelled;
//   4. ……==trutruncated 且账上有 context.pressure.recorded verdict=
//      exceeded_denied(FoldPressureFacts 现成投影):budget_exhausted
//     (无该证据的截断只降级 partial,不空口咬定预算);
//   5. 其余(封了口、无工具缺口、末回合 complete):success。
std::string ClassifyHarnessOutcomeV3(const v3::V3Ledger& ledger,
                                     const std::vector<v3::ToolActionSnapshot>& tools) {
    bool session_ended = false;
    for (const auto& event : ledger.events) {
        if (event.kind == v3::EventKindV3::SessionEnded) {
            session_ended = true;
            break;
        }
    }
    if (!session_ended) {
        return "partial";
    }
    bool tool_trouble = false;
    for (const auto& tool : tools) {
        if (tool.folded_status == "failed" || tool.folded_status == "result_missing" ||
            tool.folded_status == "selected_no_message" ||
            tool.folded_status == "message_not_admitted") {
            tool_trouble = true;
            break;
        }
    }
    // 末回合终态:整份账最后一个带 turn_id 的对话面 assistant 消息。
    std::string last_turn_id;
    for (const auto& entry : ledger.timeline) {
        if (!entry.is_message) {
            continue;
        }
        const v3::MessageLine& message = ledger.messages[entry.index];
        if (message.turn_id.has_value() && message.purpose == v3::MessagePurpose::Conversation) {
            last_turn_id = *message.turn_id;
        }
    }
    bool found_assistant = false;
    std::optional<v3::CompletionStatus> last_status =
        last_turn_id.empty() ? std::nullopt
                             : V3LastConversationCompletion(ledger, last_turn_id, &found_assistant);
    if (found_assistant && last_status == v3::CompletionStatus::Interrupted) {
        return "cancelled";
    }
    if (found_assistant && last_status == v3::CompletionStatus::Truncated) {
        for (const auto& fact : v3::FoldPressureFacts(ledger)) {
            if (fact.verdict == "exceeded_denied") {
                return "budget_exhausted";
            }
        }
        return "partial";  // 截断但没有预算证据:如实降级,不空口咬定原因
    }
    if (tool_trouble) {
        return "failure";
    }
    return "success";
}

// v3 主/子账一行(单子 §二.3 全项:messages/requests/tools/usage+cache/
// 环境/终态/artifact)。parent_run_id/parent_call_id 由调用方(树遍历)按
// 真实父子关系递进,workspace_key 走 manifest 兜底(v3 信封不带,纯 v3
// 场读不到 manifest 时留空——不伪造,与 accounting::ReadSessionUsageV3
// 同一口径)。
nlohmann::json BuildV3NodeHarnessRecord(const std::filesystem::path& session_dir,
                                        const v3::V3Ledger& ledger, bool is_root,
                                        const std::optional<std::string>& parent_run_id,
                                        const std::optional<std::string>& parent_call_id,
                                        const std::map<std::string, std::string>& child_run_id_by_action,
                                        const BlobStore& blobs, const HarnessExportOptions& options,
                                        std::optional<int> process_exit_code,
                                        const std::string& config_hash,
                                        const std::string& workspace_key) {
    const std::string exported_at = NowUtcIso8601();
    std::vector<PrivacyFinding> findings;

    nlohmann::json record;
    record["schema"] = kHarnessTrajectorySchema;
    record["schema_version"] = kHarnessTrajectorySchemaVersion;
    record["exporter_version"] = kHarnessExporterVersion;
    record["session_id"] = ledger.session_id;
    record["workspace_key"] = workspace_key;
    record["run_id"] = ledger.run_id;
    record["parent_run_id"] =
        parent_run_id.has_value() ? nlohmann::json(*parent_run_id) : nlohmann::json(nullptr);
    if (parent_call_id.has_value()) {
        record["parent_call_id"] = *parent_call_id;
    }
    std::string run_kind_name = RunKindName(RunKind::Subagent);
    if (is_root) {
        run_kind_name = RunKindName(RunKind::MainSession);
        for (const auto& event : ledger.events) {
            if (event.kind == v3::EventKindV3::SessionStarted) {
                const std::string declared = GetString(event.payload, "runKind");
                if (!declared.empty()) {
                    run_kind_name = declared;
                }
                break;  // session.started 是第二行,只此一枚
            }
        }
    }
    record["run_kind"] = run_kind_name;
    record["environment"] = BuildV3Environment(ledger, blobs, &findings);

    const std::vector<v3::ToolActionSnapshot> tool_snapshots = v3::FoldToolActions(ledger);
    std::map<std::string, const v3::ToolActionSnapshot*> tools_by_action;
    for (const auto& tool : tool_snapshots) {
        tools_by_action[tool.tool_call_id] = &tool;
    }

    // ---- turns(§二.3:字段不兼容处明确版本化,terminal 用 v3.turn_* 词表)
    {
        std::vector<std::string> turn_order;
        std::set<std::string> seen_turns;
        for (const auto& entry : ledger.timeline) {
            if (!entry.is_message) {
                continue;
            }
            const v3::MessageLine& message = ledger.messages[entry.index];
            if (!message.turn_id.has_value() || message.purpose != v3::MessagePurpose::Conversation) {
                continue;
            }
            if (seen_turns.insert(*message.turn_id).second) {
                turn_order.push_back(*message.turn_id);
            }
        }
        nlohmann::json turns = nlohmann::json::array();
        for (const std::string& turn_id : turn_order) {
            std::string trigger;
            for (const auto& entry : ledger.timeline) {
                if (!entry.is_message) {
                    continue;
                }
                const v3::MessageLine& message = ledger.messages[entry.index];
                if (message.turn_id.has_value() && *message.turn_id == turn_id &&
                    message.purpose == v3::MessagePurpose::Conversation &&
                    GetString(message.message, "role") == "user") {
                    trigger = v3::MessageOriginName(message.origin);
                    break;
                }
            }
            bool found_assistant = false;
            const auto status = V3LastConversationCompletion(ledger, turn_id, &found_assistant);
            turns.push_back(nlohmann::json{{"turn_id", turn_id},
                                           {"trigger", trigger},
                                           {"terminal", V3TurnTerminalToken(status, found_assistant)},
                                           {"claimed_outcome", nullptr},
                                           {"reason", nullptr}});
        }
        record["turns"] = std::move(turns);
    }

    // ---- messages(seq 序;只留 conversation purpose、role != system) ----
    // ---- 顺路建 finish_reason 索引(model.response.completed.finishReason,
    //      按 requestId 配对——v3 把它记在事件 payload,不在 message 上) ----
    std::map<std::string, std::string> finish_reason_by_request;
    for (const auto& event : ledger.events) {
        if (event.kind == v3::EventKindV3::ModelResponseCompleted && event.request_id.has_value()) {
            const std::string reason = GetString(event.payload, "finishReason");
            if (!reason.empty()) {
                finish_reason_by_request[*event.request_id] = reason;
            }
        }
    }

    nlohmann::json messages = nlohmann::json::array();
    // requests[]:按 requestId 首见序累计(assistant 消息或失败事件皆可
    // 触发首见)。
    std::vector<std::string> request_order;
    std::map<std::string, nlohmann::json> request_rows;
    const auto ensure_request_row = [&](const std::string& request_id) -> nlohmann::json& {
        auto it = request_rows.find(request_id);
        if (it == request_rows.end()) {
            request_order.push_back(request_id);
            nlohmann::json row;
            row["request_id"] = request_id;
            row["purpose"] = nullptr;
            row["model"] = std::string();
            row["provider"] = std::string();
            row["wire"] = std::string();
            row["output_state"] = "unknown";
            row["stop_reason"] = std::string();
            row["usage"] = nullptr;
            it = request_rows.emplace(request_id, std::move(row)).first;
        }
        return it->second;
    };

    for (const auto& entry : ledger.timeline) {
        if (!entry.is_message) {
            continue;
        }
        const v3::MessageLine& message = ledger.messages[entry.index];
        const std::string role = GetString(message.message, "role");
        if (message.request_id.has_value()) {
            nlohmann::json& row = ensure_request_row(*message.request_id);
            row["output_state"] = "committed";
            if (message.provider.has_value()) {
                row["provider"] = *message.provider;
            }
            if (message.wire.has_value()) {
                row["wire"] = *message.wire;
            }
            if (message.model.has_value()) {
                row["model"] = *message.model;
            }
            row["purpose"] = v3::MessagePurposeName(message.purpose);
            if (message.usage.has_value()) {
                row["usage"] = *message.usage;
            }
            const auto fr = finish_reason_by_request.find(*message.request_id);
            if (fr != finish_reason_by_request.end()) {
                row["stop_reason"] = fr->second;
            }
        }
        if (!V3PurposeFoldsIntoConversation(message.purpose) || role == "system") {
            continue;
        }
        nlohmann::json content = nlohmann::json::array();
        const auto content_it = message.message.find("content");
        if (content_it != message.message.end()) {
            content = V3ProjectMessageContent(*content_it, blobs, options, message.message_id, &findings,
                                              std::numeric_limits<std::uint64_t>::max());
        }
        nlohmann::json out;
        out["role"] = role;
        if (role == "user") {
            out["origin"] = v3::MessageOriginName(message.origin);
            if (message.origin != v3::MessageOrigin::Human) {
                out["injected"] = true;
            }
        } else if (role == "assistant") {
            out["origin"] = "provider_model";
            if (message.request_id.has_value()) {
                out["request_id"] = *message.request_id;
                const auto fr = finish_reason_by_request.find(*message.request_id);
                out["stop_reason"] = fr != finish_reason_by_request.end() ? fr->second : std::string();
            }
            const auto calls_it = message.message.find("tool_calls");
            if (calls_it != message.message.end() && calls_it->is_array() && !calls_it->empty()) {
                nlohmann::json tool_calls = nlohmann::json::array();
                for (const auto& call : *calls_it) {
                    if (call.is_object()) {
                        tool_calls.push_back(
                            V3ProjectToolCallBlock(call, message.message_id, &findings));
                    }
                }
                out["tool_calls"] = std::move(tool_calls);
            }
        } else if (role == "tool") {
            out["call_id"] = message.action_id.value_or(std::string());
            const auto tool_it = tools_by_action.find(out["call_id"].get<std::string>());
            if (tool_it != tools_by_action.end() && tool_it->second->tool_name.has_value()) {
                out["tool_name"] = *tool_it->second->tool_name;
            }
            bool is_error = message.message.value("is_error", false);
            if (!message.message.contains("is_error") && tool_it != tools_by_action.end()) {
                is_error = tool_it->second->folded_status == "failed";
            }
            out["is_error"] = is_error;
        }
        out["content"] = std::move(content);
        messages.push_back(std::move(out));
    }
    record["messages"] = std::move(messages);

    // requests[] 兜底:纯失败请求(无 assistant 消息)也要现身,不静默丢。
    for (const auto& event : ledger.events) {
        if (!event.request_id.has_value()) {
            continue;
        }
        if (event.kind == v3::EventKindV3::ModelRequestPrepared ||
            event.kind == v3::EventKindV3::ModelRequestFailed ||
            event.kind == v3::EventKindV3::ModelResponseFailed) {
            nlohmann::json& row = ensure_request_row(*event.request_id);
            if (row["output_state"] != "committed" &&
                (event.kind == v3::EventKindV3::ModelRequestFailed ||
                 event.kind == v3::EventKindV3::ModelResponseFailed)) {
                row["output_state"] = "failed";
            }
        }
    }
    {
        nlohmann::json requests = nlohmann::json::array();
        std::uint64_t usage_input = 0, usage_output = 0, usage_cache_read = 0, usage_cache_creation = 0,
                      usage_reasoning = 0, usage_reported = 0, failed_outputs = 0;
        for (const std::string& request_id : request_order) {
            nlohmann::json row = request_rows[request_id];
            if (row["output_state"] == "failed") {
                ++failed_outputs;
            }
            if (row["usage"].is_object()) {
                ++usage_reported;
                usage_input += GetUint(row["usage"], "input_tokens");
                usage_output += GetUint(row["usage"], "output_tokens");
                usage_cache_read += GetUint(row["usage"], "cache_read_tokens");
                usage_cache_creation += GetUint(row["usage"], "cache_creation_tokens");
                usage_reasoning += GetUint(row["usage"], "reasoning_tokens");
            }
            requests.push_back(std::move(row));
        }
        record["requests"] = std::move(requests);
        nlohmann::json usage_totals;
        usage_totals["requests_with_reported_usage"] = usage_reported;
        usage_totals["input_tokens"] = usage_input;
        usage_totals["output_tokens"] = usage_output;
        usage_totals["cache_read_tokens"] = usage_cache_read;
        usage_totals["cache_creation_tokens"] = usage_cache_creation;
        usage_totals["reasoning_tokens"] = usage_reasoning;
        record["usage_totals"] = std::move(usage_totals);
        record["request_retry_summary"] =
            nlohmann::json{{"requests", request_order.size()}, {"failed_outputs", failed_outputs}};
    }

    // ---- tools(FoldToolActions 折叠;§二.3 artifact 用 ExpandResultPreview
    //      现成投影,不另造第二套引用展开) ----
    {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& tool : tool_snapshots) {
            nlohmann::json out;
            out["call_id"] = tool.tool_call_id;
            out["tool_name"] = tool.tool_name.value_or(std::string());
            out["arguments"] = tool.declared_args.has_value()
                                   ? RedactJsonValue(*tool.declared_args, tool.tool_call_id, &findings)
                                   : nlohmann::json::object();
            out["outcome"] = tool.folded_status;
            if (!tool.attempts.empty()) {
                const auto& last = tool.attempts.back();
                out["started"] = last.started;
                if (last.exit_code.has_value()) {
                    out["exit_code"] = *last.exit_code;
                }
                if (last.execution_duration_ms.has_value()) {
                    out["duration_ms"] = *last.execution_duration_ms;
                }
                if (last.effective_args_ref.has_value()) {
                    out["effective_args_ref"] = *last.effective_args_ref;
                }
            } else {
                out["started"] = false;
            }
            const auto child_it = child_run_id_by_action.find(tool.tool_call_id);
            if (child_it != child_run_id_by_action.end()) {
                out["child_run_id"] = child_it->second;
            }
            // tool 消息正文:message_versions 里挑当前链上那版,没有就取
            // 最后一版(降档/未接纳也要如实带正文,不能因不在链上就装没有)。
            const v3::MessageLine* tool_message = nullptr;
            bool tool_message_on_chain = false;
            for (const auto& version : tool.message_versions) {
                if (tool_message_on_chain) {
                    break;  // 已锁定当前链上那版,不许被后续非链版本顶替
                }
                const v3::MessageLine* candidate = ledger.FindMessage(version.message_id);
                if (candidate == nullptr) {
                    continue;
                }
                tool_message = candidate;  // seq 序推进:没锁定 current 就一路取最后一版
                tool_message_on_chain = version.on_current_chain;
            }
            if (tool_message != nullptr) {
                nlohmann::json result;
                nlohmann::json content = nlohmann::json::array();
                const auto content_it = tool_message->message.find("content");
                if (content_it != tool_message->message.end()) {
                    // 生产写法(AppendToolMessage 注记):单块结果落 string,
                    // 不额外包数组——两种形状都要读出来,不漏读。
                    content = V3ProjectMessageContent(*content_it, blobs, options, tool_message->message_id,
                                                      &findings, options.max_inline_tool_result_bytes);
                }
                result["is_error"] = tool_message->message.value(
                    "is_error", tool.folded_status == "failed");
                result["content"] = std::move(content);
                const auto preview = v3::ExpandResultPreview(ledger, session_dir, tool_message->message_id);
                if (!preview.artifacts.empty()) {
                    nlohmann::json artifacts = nlohmann::json::array();
                    for (const auto& artifact : preview.artifacts) {
                        artifacts.push_back(nlohmann::json{{"artifact_id", artifact.artifact_id},
                                                           {"path", artifact.path},
                                                           {"exists", artifact.exists},
                                                           {"hash_ok", artifact.hash_ok},
                                                           {"bytes", artifact.bytes},
                                                           {"gap_reason", artifact.gap_reason}});
                    }
                    result["artifacts"] = std::move(artifacts);
                }
                if (!preview.complete) {
                    result["preview_incomplete"] = true;
                }
                out["result"] = std::move(result);
            }
            tools.push_back(std::move(out));
        }
        record["tools"] = std::move(tools);
    }

    // ---- outcome(v3 版分型;§二.3) ----
    {
        bool session_ended = false;
        for (const auto& event : ledger.events) {
            if (event.kind == v3::EventKindV3::SessionEnded) {
                session_ended = true;
                break;
            }
        }
        nlohmann::json outcome;
        outcome["status"] = ClassifyHarnessOutcomeV3(ledger, tool_snapshots);
        outcome["session_ended"] = session_ended;
        outcome["process_exit_code"] =
            process_exit_code.has_value() ? nlohmann::json(*process_exit_code) : nlohmann::json(nullptr);
        record["outcome"] = std::move(outcome);
    }

    // ---- 隐私缘由账 ----
    {
        const std::vector<PrivacyFinding> deduped = DedupeFindings(findings);
        if (!deduped.empty()) {
            nlohmann::json privacy_findings = nlohmann::json::array();
            for (const auto& finding : deduped) {
                privacy_findings.push_back(nlohmann::json{{"code", finding.code},
                                                          {"source_event_id", finding.source_event_id}});
            }
            record["privacy_findings"] = std::move(privacy_findings);
        }
    }

    // ---- source:来源锚(v3 版,§二.3) ----
    {
        nlohmann::json source;
        std::error_code ec;
        const auto relative = std::filesystem::relative(ledger.path, session_dir, ec);
        std::string stream_id = platform::PathToUtf8(ec ? ledger.path.filename() : relative);
        std::replace(stream_id.begin(), stream_id.end(), '\\', '/');
        source["stream"] = stream_id;
        const auto last = ledger.LastEntry();
        source["journal_last_hash"] =
            last.has_value() ? (last->is_message ? ledger.messages[last->index].line_hash
                                                 : ledger.events[last->index].line_hash)
                             : std::string();
        source["folded_seq"] = last.has_value() ? last->seq : ledger.lines;
        source["format"] = "v3";
        source["exported_at"] = exported_at;
        source["exporter_config_hash"] = config_hash;
        nlohmann::json integrity;
        integrity["events_folded"] = ledger.lines;
        integrity["truncated_tail"] = false;  // ReadV3Ledger 对截断尾 fail-closed,能读到即非截断
        std::uint64_t dangling = 0;
        for (const auto& tool : tool_snapshots) {
            if (tool.folded_status == "unknown" || tool.folded_status == "result_missing" ||
                tool.folded_status == "selected_no_message" ||
                tool.folded_status == "message_not_admitted") {
                ++dangling;
            }
        }
        integrity["dangling_tools"] = dangling;
        source["integrity"] = std::move(integrity);
        record["source"] = std::move(source);
    }
    return record;
}

// 子账缺失/环/不可读的存根行(fail-closed,不静默丢失——单子 §二.2:
// "扫描错误不得吞成没有流"同理适用到"子账连不上")。
nlohmann::json BuildV3UnavailableChildStub(const v3::SubagentSessionNode& node,
                                           const std::string& config_hash,
                                           std::optional<int> process_exit_code) {
    nlohmann::json record;
    record["schema"] = kHarnessTrajectorySchema;
    record["schema_version"] = kHarnessTrajectorySchemaVersion;
    record["exporter_version"] = kHarnessExporterVersion;
    record["session_id"] = node.session_id;
    record["run_id"] = node.run_id;
    record["parent_run_id"] = node.parent_session_id.empty() ? nlohmann::json(nullptr)
                                                              : nlohmann::json(node.parent_session_id);
    if (!node.parent_action_id.empty()) {
        record["parent_call_id"] = node.parent_action_id;
    }
    record["run_kind"] = RunKindName(RunKind::Subagent);
    record["messages"] = nlohmann::json::array();
    record["requests"] = nlohmann::json::array();
    record["tools"] = nlohmann::json::array();
    record["turns"] = nlohmann::json::array();
    nlohmann::json outcome;
    outcome["status"] = "unknown";
    outcome["process_exit_code"] =
        process_exit_code.has_value() ? nlohmann::json(*process_exit_code) : nlohmann::json(nullptr);
    record["outcome"] = std::move(outcome);
    nlohmann::json source;
    source["stream"] = platform::PathToUtf8(node.jsonl_path.filename());
    source["format"] = "v3";
    source["fold_error"] = "v3." + (node.link_status.empty() ? std::string("unreadable") : node.link_status);
    source["exported_at"] = NowUtcIso8601();
    source["exporter_config_hash"] = config_hash;
    record["source"] = std::move(source);
    return record;
}

// v3 场引擎:主账 + 递归子账树(v3::WalkSessionTree,与
// accounting::ReadSessionUsageV3/replay.cpp::VerifyV3SessionDir 同一套
// 遍历件,不另造第二套父子关系判定)。
std::vector<nlohmann::json> BuildSessionHarnessRecordsV3(const std::filesystem::path& session_dir,
                                                          const std::filesystem::path& v3_main_stream,
                                                          const HarnessExportOptions& options,
                                                          std::optional<int> process_exit_code) {
    std::vector<nlohmann::json> records;
    const std::string config_hash = ComputeHarnessConfigHash(options);
    const BlobStore blobs(session_dir / "artifacts");
    std::string workspace_key;
    if (const auto manifest = ReadSessionJson(session_dir)) {
        workspace_key = manifest->workspace_key;
    }
    const v3::SubagentSessionNode tree = v3::WalkSessionTree(v3_main_stream);
    if (!tree.ledger.has_value()) {
        // 根账验不过:单一存根行,fail-closed,绝不出看似成功的兄弟行。
        nlohmann::json record;
        record["schema"] = kHarnessTrajectorySchema;
        record["schema_version"] = kHarnessTrajectorySchemaVersion;
        record["exporter_version"] = kHarnessExporterVersion;
        record["session_id"] = tree.session_id.empty() ? platform::PathToUtf8(session_dir.filename())
                                                       : tree.session_id;
        record["run_id"] = tree.run_id;
        record["parent_run_id"] = nullptr;
        record["run_kind"] = RunKindName(RunKind::MainSession);
        record["messages"] = nlohmann::json::array();
        record["requests"] = nlohmann::json::array();
        record["tools"] = nlohmann::json::array();
        record["turns"] = nlohmann::json::array();
        nlohmann::json outcome;
        outcome["status"] = "unknown";
        outcome["process_exit_code"] =
            process_exit_code.has_value() ? nlohmann::json(*process_exit_code) : nlohmann::json(nullptr);
        record["outcome"] = std::move(outcome);
        nlohmann::json source;
        source["stream"] = platform::PathToUtf8(v3_main_stream.filename());
        source["format"] = "v3";
        source["fold_error"] = "v3.ledger_unreadable";
        source["exported_at"] = NowUtcIso8601();
        source["exporter_config_hash"] = config_hash;
        record["source"] = std::move(source);
        records.push_back(std::move(record));
        return records;
    }

    std::function<void(const v3::SubagentSessionNode&, bool, std::optional<std::string>,
                       std::optional<std::string>)>
        walk = [&](const v3::SubagentSessionNode& node, bool is_root,
                  std::optional<std::string> parent_run_id, std::optional<std::string> parent_call_id) {
            std::map<std::string, std::string> child_run_id_by_action;
            for (const auto& child : node.children) {
                if (child.link_status == "cycle") {
                    continue;
                }
                if (!child.parent_action_id.empty()) {
                    child_run_id_by_action[child.parent_action_id] = child.run_id;
                }
            }
            if (node.ledger.has_value()) {
                records.push_back(BuildV3NodeHarnessRecord(session_dir, *node.ledger, is_root,
                                                           parent_run_id, parent_call_id,
                                                           child_run_id_by_action, blobs, options,
                                                           process_exit_code, config_hash,
                                                           workspace_key));
            } else if (!is_root) {
                records.push_back(BuildV3UnavailableChildStub(node, config_hash, process_exit_code));
            }
            const std::optional<std::string> this_run_id =
                node.ledger.has_value() ? std::optional<std::string>(node.ledger->run_id)
                                        : (node.run_id.empty() ? std::nullopt
                                                               : std::optional<std::string>(node.run_id));
            for (const auto& child : node.children) {
                if (child.link_status == "cycle") {
                    continue;  // 环:树内已现身,不重复出行
                }
                const std::optional<std::string> call_id =
                    child.parent_action_id.empty() ? std::nullopt
                                                    : std::optional<std::string>(child.parent_action_id);
                walk(child, false, this_run_id, call_id);
            }
        };
    walk(tree, true, std::nullopt, std::nullopt);
    return records;
}

// ---------------------------------------------------------------------------
// 一份 stream -> 一行 record
// ---------------------------------------------------------------------------

nlohmann::json BuildStreamHarnessRecord(const std::filesystem::path& session_dir,
                                        const std::filesystem::path& stream_path,
                                        const BlobStore& blobs,
                                        const HarnessExportOptions& options,
                                        std::optional<int> process_exit_code,
                                        const std::string& config_hash) {
    const std::string exported_at = NowUtcIso8601();
    const RawStreamScan raw = ScanStreamRaw(stream_path);
    std::error_code ec;
    const auto relative = std::filesystem::relative(stream_path, session_dir, ec);
    std::string stream_id = platform::PathToUtf8(ec ? stream_path.filename() : relative);
    std::replace(stream_id.begin(), stream_id.end(), '\\', '/');  // 协议统一正斜杠

    // ---- 公共骨架(验链失败也保得住身份) ----
    const auto make_shell = [&](const std::string& run_id) {
        nlohmann::json record;
        record["schema"] = kHarnessTrajectorySchema;
        record["schema_version"] = kHarnessTrajectorySchemaVersion;
        record["exporter_version"] = kHarnessExporterVersion;
        record["session_id"] = raw.session_id.empty()
                                   ? platform::PathToUtf8(session_dir.filename())
                                   : raw.session_id;
        record["workspace_key"] = raw.workspace_key;
        record["run_id"] = run_id;
        record["parent_run_id"] = raw.parent_run_id.empty() ? nlohmann::json(nullptr)
                                                            : nlohmann::json(raw.parent_run_id);
        if (!raw.parent_call_id.empty()) {
            record["parent_call_id"] = raw.parent_call_id;
        }
        record["run_kind"] = raw.run_kind_name;
        return record;
    };
    const std::string fallback_run_id =
        raw.run_id.empty() ? platform::PathToUtf8(stream_path.stem()) : raw.run_id;

    const auto fold = FoldStreamReplay(stream_path);
    if (!fold.ok()) {
        // 验链不过:存根行,fail-closed。绝不出 clean success(§五)。
        nlohmann::json record = make_shell(fallback_run_id);
        record["messages"] = nlohmann::json::array();
        record["requests"] = nlohmann::json::array();
        record["tools"] = nlohmann::json::array();
        record["turns"] = nlohmann::json::array();
        nlohmann::json outcome;
        outcome["status"] = "unknown";
        outcome["run_terminal"] = nullptr;
        outcome["process_exit_code"] =
            process_exit_code.has_value() ? nlohmann::json(*process_exit_code) : nlohmann::json(nullptr);
        record["outcome"] = std::move(outcome);
        nlohmann::json source;
        source["stream"] = stream_id;
        source["fold_error"] = fold.error_code;
        source["exported_at"] = exported_at;
        source["exporter_config_hash"] = config_hash;
        record["source"] = std::move(source);
        return record;
    }
    const ReplayState& state = fold.state;

    nlohmann::json record = make_shell(state.run_id);
    std::vector<PrivacyFinding> findings;

    // ---- 环境快照(只有 main 流落过):provider/wire/model/版本/参数 ----
    {
        nlohmann::json environment;
        bool have_snapshot = false;
        if (!raw.env_snapshot_ref.is_null()) {
            const auto ref = BlobRef::FromJson(raw.env_snapshot_ref);
            if (ref.has_value()) {
                const auto read = blobs.ReadVerified(*ref);
                if (read.has_value()) {
                    const auto snapshot = nlohmann::json::parse(*read, nullptr, false);
                    if (!snapshot.is_discarded() && snapshot.is_object()) {
                        have_snapshot = true;
                        environment["lubancode_version"] = GetString(snapshot, "lubancode_version");
                        environment["provider"] = GetString(snapshot, "provider");
                        environment["wire"] = GetString(snapshot, "wire");
                        environment["model"] = GetString(snapshot, "model");
                        environment["model_parameters"] =
                            snapshot.contains("model_parameters") &&
                                    snapshot["model_parameters"].is_object()
                                ? snapshot["model_parameters"]
                                : nlohmann::json::object();
                        // 生效参数快照(§四):config 侧已 redacted,这里再过
                        // 一道 secret 扫描兜底;命中即整包换 {redacted}。
                        if (snapshot.contains("config_snapshot_redacted")) {
                            environment["config_snapshot_redacted"] = RedactJsonValue(
                                snapshot["config_snapshot_redacted"], "environment", &findings);
                        }
                    }
                }
            }
        }
        if (!have_snapshot) {
            environment["snapshot_available"] = false;  // 子账/旧账:如实报缺,不猜
        }
        record["environment"] = std::move(environment);
    }

    // ---- turns(折叠序) ----
    nlohmann::json turns = nlohmann::json::array();
    for (const auto& turn : state.turns) {
        const auto raw_it = raw.turns.find(turn.turn_id);
        const TurnRaw& turn_raw = raw_it != raw.turns.end() ? raw_it->second : TurnRaw{};
        turns.push_back(nlohmann::json{{"turn_id", turn.turn_id},
                                       {"trigger", turn.trigger},
                                       {"terminal", turn.terminal_state},
                                       {"claimed_outcome", turn_raw.claimed_outcome},
                                       {"reason", turn_raw.terminal_reason}});
    }
    record["turns"] = std::move(turns);

    // ---- messages:对话面事件按 turn 折叠序、turn 内按 seq 交错 ----
    std::map<std::string, const ReplayToolEntry*> folded_calls;
    for (const auto& entry : state.tools) {
        folded_calls[entry.call_id] = &entry;
    }
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& turn : state.turns) {
        const auto raw_it = raw.turns.find(turn.turn_id);
        if (raw_it == raw.turns.end()) {
            continue;
        }
        for (const auto& event : raw_it->second.conversation) {
            const std::string tag = GetString(event, "tag");
            const std::string event_id = GetString(event, "event_id");
            if (tag == "input") {
                const std::string origin = GetString(event, "origin");
                const bool genuine_user = origin == "external_user" || origin == "queued_user";
                nlohmann::json content = nlohmann::json::array();
                for (const auto& block : event["content"]) {
                    if (!block.is_object()) {
                        continue;
                    }
                    if (GetString(block, "type") == "text") {
                        content.push_back(ProjectTextBlock("text", block, blobs, options, event_id,
                                                           &findings, std::numeric_limits<std::uint64_t>::max()));
                    } else {
                        content.push_back(block);  // image 一类引用块,无字节
                    }
                }
                nlohmann::json message;
                message["role"] = "user";
                message["origin"] = origin;
                message["actor"] = GetString(event, "actor");
                // 宿主注入(记忆召回/续跑/peer):照实留在 messages(模型确实
                // 看见它),origin/actor/injected 三处标明,不冒充真人问句。
                if (!genuine_user) {
                    message["injected"] = true;
                }
                message["content"] = std::move(content);
                messages.push_back(std::move(message));
                continue;
            }
            if (tag == "output") {
                const auto purpose_it = raw.request_purpose.find(GetString(event, "request_id"));
                const std::string purpose =
                    purpose_it != raw.request_purpose.end() ? purpose_it->second : std::string();
                if (!PurposeFoldsIntoConversation(purpose)) {
                    continue;  // compact/起名一类宿主工作产物,不进会话历史
                }
                nlohmann::json content = nlohmann::json::array();
                nlohmann::json tool_calls = nlohmann::json::array();
                for (const auto& block : event["blocks"]) {
                    if (!block.is_object()) {
                        continue;
                    }
                    const std::string type = GetString(block, "type");
                    if (type == "text") {
                        content.push_back(ProjectTextBlock("text", block, blobs, options, event_id,
                                                           &findings,
                                                           std::numeric_limits<std::uint64_t>::max()));
                    } else if (type == "thinking") {
                        // thinking 投影按现有隐私策略(§四):默认只留 ref +
                        // 省略缘由;显式授权(include_thinking)才带正文,带
                        // 了也过 secret 脱敏。
                        if (options.include_thinking) {
                            content.push_back(ProjectTextBlock("thinking", block, blobs, options,
                                                               event_id, &findings,
                                                               std::numeric_limits<std::uint64_t>::max()));
                        } else {
                            content.push_back(nlohmann::json{
                                {"type", "thinking_ref"},
                                {"omitted_reason", "thinking_not_authorized"},
                                {"source_event_id", event_id}});
                        }
                    } else if (type == "image_ref") {
                        content.push_back(block);
                    } else if (type == "tool_call") {
                        nlohmann::json call;
                        call["call_id"] = GetString(block, "call_id");
                        if (block.contains("provider_call_id") &&
                            block["provider_call_id"].is_string()) {
                            call["provider_call_id"] = block["provider_call_id"];
                        }
                        call["name"] = GetString(block, "name");
                        call["arguments"] =
                            block.contains("arguments")
                                ? RedactJsonValue(block["arguments"], event_id, &findings)
                                : nlohmann::json::object();
                        tool_calls.push_back(std::move(call));
                    }
                }
                nlohmann::json message;
                message["role"] = "assistant";
                message["origin"] = "provider_model";
                message["request_id"] = GetString(event, "request_id");
                message["stop_reason"] = GetString(event, "stop_reason");
                message["content"] = std::move(content);
                if (!tool_calls.empty()) {
                    message["tool_calls"] = std::move(tool_calls);
                }
                messages.push_back(std::move(message));
                continue;
            }
            if (tag == "result") {
                const std::string call_id = GetString(event, "call_id");
                nlohmann::json content = nlohmann::json::array();
                for (const auto& block : event["content"]) {
                    if (!block.is_object()) {
                        continue;
                    }
                    if (GetString(block, "type") == "text") {
                        // 大工具结果:head 摘要 + 引用,单文件不无上限膨胀(§四)。
                        content.push_back(ProjectTextBlock("text", block, blobs, options, event_id,
                                                           &findings,
                                                           options.max_inline_tool_result_bytes));
                    } else {
                        content.push_back(block);
                    }
                }
                nlohmann::json message;
                message["role"] = "tool";
                message["call_id"] = call_id;
                const auto folded = folded_calls.find(call_id);
                if (folded != folded_calls.end() && !folded->second->tool_name.empty()) {
                    message["tool_name"] = folded->second->tool_name;
                }
                message["is_error"] = GetBool(event, "is_error");
                message["content"] = std::move(content);
                messages.push_back(std::move(message));
            }
        }
    }
    record["messages"] = std::move(messages);

    // ---- requests:逐次模型请求的 usage/stop reason/attempt 摘要 ----
    {
        nlohmann::json requests = nlohmann::json::array();
        std::uint64_t usage_input = 0;
        std::uint64_t usage_output = 0;
        std::uint64_t usage_cache_read = 0;
        std::uint64_t usage_cache_creation = 0;
        std::uint64_t usage_reasoning = 0;
        std::uint64_t usage_reported = 0;
        std::uint64_t failed_outputs = 0;
        for (const auto& step : state.requests) {
            nlohmann::json request;
            request["request_id"] = step.request_id;
            const auto purpose_it = raw.request_purpose.find(step.request_id);
            request["purpose"] =
                purpose_it != raw.request_purpose.end() ? purpose_it->second : std::string();
            request["model"] = step.model;
            request["provider"] = step.provider;
            request["wire"] = step.wire;
            request["parameters"] = step.parameters;
            request["sent"] = step.sent;
            request["output_state"] = step.output_state;
            request["stop_reason"] = step.stop_reason;
            if (step.output_state == "failed") {
                ++failed_outputs;
            }
            // usage(§四:v2 canonical owner 是 model.usage.recorded;缺事件
            // 给 null,不拿 0 冒充)。
            const auto usage_it = raw.usage.find(step.request_id);
            if (usage_it != raw.usage.end()) {
                const nlohmann::json& usage = usage_it->second;
                request["usage"] = usage;
                if (GetBool(usage, "reported_by_provider")) {
                    ++usage_reported;
                    usage_input += GetUint(usage, "input_tokens");
                    usage_output += GetUint(usage, "output_tokens");
                    usage_cache_read += GetUint(usage, "cache_read_tokens");
                    usage_cache_creation += GetUint(usage, "cache_creation_tokens");
                    usage_reasoning += GetUint(usage, "reasoning_tokens");
                }
            } else {
                request["usage"] = nullptr;
            }
            requests.push_back(std::move(request));
        }
        record["requests"] = std::move(requests);
        // attempt/retry 摘要:一次失败的输出后紧跟重新 prepared,是本仓的
        // 重试形状;失败计数即摘要面。
        nlohmann::json usage_totals;
        usage_totals["requests_with_reported_usage"] = usage_reported;
        usage_totals["input_tokens"] = usage_input;
        usage_totals["output_tokens"] = usage_output;
        usage_totals["cache_read_tokens"] = usage_cache_read;
        usage_totals["cache_creation_tokens"] = usage_cache_creation;
        usage_totals["reasoning_tokens"] = usage_reasoning;
        record["usage_totals"] = std::move(usage_totals);
        record["request_retry_summary"] = nlohmann::json{
            {"requests", state.requests.size()}, {"failed_outputs", failed_outputs}};
    }

    // ---- tools:工具名/有效入参/退出码/timeout-cancel-error/结果 ----
    {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& [call_id, call_raw] : raw.calls) {
            nlohmann::json tool;
            tool["call_id"] = call_id;
            tool["tool_name"] = call_raw.tool_name;
            if (call_raw.effective_payload.is_object()) {
                tool["effect_class"] = GetString(call_raw.effective_payload, "effect_class");
                tool["effective_arguments_sha256"] =
                    GetString(call_raw.effective_payload, "effective_arguments_sha256");
                tool["effective_arguments"] =
                    call_raw.effective_payload.contains("effective_arguments")
                        ? RedactJsonValue(call_raw.effective_payload["effective_arguments"],
                                          call_raw.terminal_event_id.empty()
                                              ? call_id
                                              : call_raw.terminal_event_id,
                                          &findings)
                        : nlohmann::json::object();
            }
            const auto folded = folded_calls.find(call_id);
            if (folded != folded_calls.end()) {
                const ReplayToolEntry& entry = *folded->second;
                tool["started"] = entry.started;
                if (entry.child_run_id.has_value()) {
                    // 子代理边界:引用不内联(子流自己是单独一行,§四)。
                    tool["child_run_id"] = *entry.child_run_id;
                    if (!entry.child_terminal_event_hash.empty()) {
                        tool["child_terminal_event_hash"] = entry.child_terminal_event_hash;
                    }
                }
            }
            if (!call_raw.terminal_kind.empty()) {
                tool["terminal_kind"] = call_raw.terminal_kind;
                const nlohmann::json& payload = call_raw.terminal_payload;
                tool["outcome"] = GetString(payload, "outcome");
                // timeout/cancel/error 的稳定码:reason 必有,error_code 有
                // 才带(cancelled/unknown 只落 reason)。
                tool["reason"] = GetString(payload, "reason");
                if (payload.contains("error_code") && payload["error_code"].is_string()) {
                    tool["error_code"] = payload["error_code"];
                }
                if (payload.contains("exit_code") && payload["exit_code"].is_number_integer()) {
                    tool["exit_code"] = payload["exit_code"];
                }
                if (payload.contains("duration_ms") && payload["duration_ms"].is_number()) {
                    tool["duration_ms"] = payload["duration_ms"];
                }
            }
            const auto result_it = raw.tool_results.find(call_id);
            if (result_it != raw.tool_results.end()) {
                nlohmann::json content = nlohmann::json::array();
                if (result_it->second.contains("content") &&
                    result_it->second["content"].is_array()) {
                    for (const auto& block : result_it->second["content"]) {
                        if (!block.is_object() || GetString(block, "type") != "text") {
                            content.push_back(block);
                            continue;
                        }
                        content.push_back(ProjectTextBlock(
                            "text", block, blobs, options,
                            call_raw.terminal_event_id.empty() ? call_id
                                                               : call_raw.terminal_event_id,
                            &findings, options.max_inline_tool_result_bytes));
                    }
                }
                nlohmann::json result;
                result["is_error"] = GetBool(result_it->second, "is_error");
                result["content"] = std::move(content);
                if (result_it->second.contains("structured_content")) {
                    result["structured_content"] =
                        RedactJsonValue(result_it->second["structured_content"], call_id, &findings);
                }
                tool["result"] = std::move(result);
            }
            tools.push_back(std::move(tool));
        }
        record["tools"] = std::move(tools);
    }

    // ---- outcome:分型 + 终态 + 进程退出码 ----
    {
        HarnessOutcomeInputs inputs;
        inputs.fold_ok = true;
        inputs.truncated_tail = state.integrity.truncated_tail;
        inputs.run_terminal = state.run_terminal_state;
        for (const auto& turn : state.turns) {
            inputs.turn_terminals.push_back(turn.terminal_state);
            const auto raw_it = raw.turns.find(turn.turn_id);
            if (raw_it != raw.turns.end() && turn.terminal_state == "turn.failed" &&
                !raw_it->second.terminal_reason.empty()) {
                inputs.failure_reasons.push_back(raw_it->second.terminal_reason);
            }
        }
        nlohmann::json outcome;
        outcome["status"] = ClassifyHarnessOutcome(inputs);
        outcome["run_terminal"] =
            state.run_terminal_state.empty() ? nlohmann::json(nullptr)
                                             : nlohmann::json(state.run_terminal_state);
        outcome["session_ended"] = state.session_end_state == "ended";
        if (!inputs.failure_reasons.empty()) {
            outcome["failure_reasons"] = inputs.failure_reasons;
        }
        outcome["process_exit_code"] =
            process_exit_code.has_value() ? nlohmann::json(*process_exit_code)
                                          : nlohmann::json(nullptr);
        record["outcome"] = std::move(outcome);
    }

    // ---- 隐私缘由账(脱敏是处置面,findings 是账) ----
    {
        const std::vector<PrivacyFinding> deduped = DedupeFindings(findings);
        if (!deduped.empty()) {
            nlohmann::json privacy_findings = nlohmann::json::array();
            for (const auto& finding : deduped) {
                privacy_findings.push_back(nlohmann::json{{"code", finding.code},
                                                          {"source_event_id", finding.source_event_id}});
            }
            record["privacy_findings"] = std::move(privacy_findings);
        }
    }

    // ---- source:来源锚(§四) ----
    {
        nlohmann::json source;
        source["stream"] = stream_id;
        source["journal_last_hash"] = state.integrity.last_event_hash;
        source["folded_seq"] = state.folded_seq;
        source["replay_level"] = raw.replay_level;
        source["gaps"] = raw.gaps;
        nlohmann::json integrity;
        integrity["events_folded"] = state.integrity.events_folded;
        integrity["truncated_tail"] = state.integrity.truncated_tail;
        integrity["dangling_tools"] = state.integrity.dangling_tools;
        source["integrity"] = std::move(integrity);
        source["exported_at"] = exported_at;
        source["exporter_config_hash"] = config_hash;
        record["source"] = std::move(source);
    }
    return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// 指纹与分型
// ---------------------------------------------------------------------------

std::string ComputeHarnessConfigHash(const HarnessExportOptions& options) {
    nlohmann::json config;
    config["include_thinking"] = options.include_thinking;
    config["max_resolved_blob_bytes"] = options.max_resolved_blob_bytes;
    config["max_inline_tool_result_bytes"] = options.max_inline_tool_result_bytes;
    config["exporter_version"] = kHarnessExporterVersion;
    config["schema_version"] = kHarnessTrajectorySchemaVersion;
    const auto dumped = CanonicalJsonDump(config);
    return hooks::Sha256Hex(dumped.has_value() ? *dumped : config.dump());
}

const char* ClassifyHarnessOutcome(const HarnessOutcomeInputs& inputs) {
    if (!inputs.fold_ok) {
        return "unknown";
    }
    if (inputs.truncated_tail) {
        return "partial";  // §16.3:已验证前缀,如实报不伪造终态
    }
    const bool any_failed = inputs.run_terminal == "run.failed" ||
                            std::find(inputs.turn_terminals.begin(), inputs.turn_terminals.end(),
                                      "turn.failed") != inputs.turn_terminals.end();
    if (any_failed) {
        // 预算耗尽单列(§四的六型之一):turn/run 的失败 reason 点名 budget。
        for (const std::string& reason : inputs.failure_reasons) {
            if (reason.find("budget") != std::string::npos) {
                return "budget_exhausted";
            }
        }
        return "failure";
    }
    const bool any_cancelled =
        inputs.run_terminal == "run.cancelled" ||
        std::find(inputs.turn_terminals.begin(), inputs.turn_terminals.end(), "turn.cancelled") !=
            inputs.turn_terminals.end();
    if (any_cancelled) {
        return "cancelled";
    }
    // 崩溃前缀/close 没写进 run terminal:partial,不是 unknown——账仍可验。
    if (inputs.run_terminal.empty() ||
        std::find(inputs.turn_terminals.begin(), inputs.turn_terminals.end(), "") !=
            inputs.turn_terminals.end()) {
        return "partial";
    }
    return "success";
}

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------

std::vector<nlohmann::json> BuildSessionHarnessRecords(const std::filesystem::path& session_dir,
                                                       const HarnessExportOptions& options,
                                                       std::optional<int> process_exit_code) {
    std::vector<nlohmann::json> records;
    // 格式分派(单子 §二.2):V3-GAP-01(accounting::ReadSessionUsage)、
    // VerifyV3SessionDir(replay.cpp)同一套探测件,认 V3 主账
    // <session-id>.jsonl,不再只认根下 main.jsonl。
    const auto discovery = projection::DiscoverSessionStreams(session_dir);
    if (discovery.error.has_value()) {
        return records;  // 纯引擎不决策错误码;调用方(ExportSessionHarnessV1)按
                         // discovery.error 给稳定诊断。
    }
    const std::string config_hash = ComputeHarnessConfigHash(options);
    const BlobStore blobs(session_dir / "artifacts");
    if (discovery.format == "v3") {
        // v3 场:主账 + 递归子账走真投影引擎(§二.3);workflow 编排账恒
        // v2(§4.31,与 v3 同代并行),走既有单流引擎,schema 不变。
        records = BuildSessionHarnessRecordsV3(session_dir, discovery.v3_main_stream, options,
                                               process_exit_code);
        for (const auto& stream_path : projection::CollectWorkflowStreams(session_dir)) {
            records.push_back(BuildStreamHarnessRecord(session_dir, stream_path, blobs, options,
                                                       process_exit_code, config_hash));
        }
        return records;
    }
    for (const auto& stream_path : discovery.streams) {
        records.push_back(BuildStreamHarnessRecord(session_dir, stream_path, blobs, options,
                                                   process_exit_code, config_hash));
    }
    return records;
}

HarnessExportReport ExportSessionHarnessV1(const std::filesystem::path& session_dir,
                                           const std::filesystem::path& target_path,
                                           const HarnessExportOptions& options,
                                           std::optional<int> process_exit_code) {
    HarnessExportReport report;
    report.schema = std::string(kHarnessTrajectorySchema);
    report.schema_version = kHarnessTrajectorySchemaVersion;
    std::error_code ec;
    if (!std::filesystem::is_directory(session_dir, ec)) {
        report.error_code = "export.no_session_dir";
        report.message =
            "session 目录不存在(会话没开 trajectory 便没有账,不造假): " + platform::PathToUtf8(session_dir);
        return report;
    }
    // 格式分派 + 稳定诊断(单子 §二.2/§四:主账缺失/不可读/空首行/坏
    // schema/V2V3 并存/坏链各给稳定诊断,export.no_streams 不再暗示"没开
    // trajectory"——恒开配置下这句话本身就是假的)。
    const auto discovery = projection::DiscoverSessionStreams(session_dir);
    if (discovery.error.has_value()) {
        report.error_code = discovery.error->code;
        report.message = discovery.error->message + "; session_dir=" + platform::PathToUtf8(session_dir) +
                         "; 补导命令: lubancode trajectory export " +
                         platform::PathToUtf8(session_dir.filename()) +
                         " --format harness-v1 --output <path>";
        return report;
    }
    if (discovery.streams.empty()) {
        report.error_code = "export.no_streams";
        report.message = "session 目录(" + platform::PathToUtf8(session_dir) + ",格式=" +
                         (discovery.format.empty() ? std::string("unknown") : discovery.format) +
                         ")里没有可导出的账目——不是没开 trajectory(本版恒开),是没找到可读的主账/子账"
                         "文件; 补导命令: lubancode trajectory export " +
                         platform::PathToUtf8(session_dir.filename()) +
                         " --format harness-v1 --output <path>";
        return report;
    }

    std::string content;
    const auto records = BuildSessionHarnessRecords(session_dir, options, process_exit_code);
    for (const auto& record : records) {
        const auto dumped = CanonicalJsonDump(record);
        if (!dumped.has_value()) {
            report.error_code = "export.internal_error";
            report.message = "harness record canonical 序列化失败: " + dumped.error();
            return report;
        }
        content.append(*dumped);
        content.append("\n");
    }
    report.records = records.size();
    report.session_id = platform::PathToUtf8(session_dir.filename());
    if (!records.empty() && records.front().contains("session_id") &&
        records.front()["session_id"].is_string()) {
        const std::string from_ledger = records.front()["session_id"].get<std::string>();
        if (!from_ledger.empty()) {
            report.session_id = from_ledger;
        }
    }

    // 相对路径按当前工作目录解析(单子 §三;调用方已传绝对路径时原样)。
    std::error_code abs_ec;
    const auto absolute = std::filesystem::absolute(target_path, abs_ec);
    if (abs_ec) {
        report.error_code = "export.write_failed";
        report.message = "输出路径解析不了: " + abs_ec.message();
        return report;
    }
    report.target = absolute;

    // 写盘前的磁盘余量门(§12.2 storage_exhausted 同款判据)。
    if (!HasDiskReserve(absolute.parent_path(), options.min_free_bytes)) {
        report.error_code = "export.storage_exhausted";
        report.message = "磁盘余量低于导出门,一个字节不写";
        return report;
    }

    // 同目录临时件 + flush 落盘 + 原子替换;失败不碰旧成品(§三/§七)。
    std::string write_error;
    if (!WriteTextAtomically(absolute, content, &write_error,
                             platform::WriteDurability::ProcessCrashDurability)) {
        report.error_code = "export.write_failed";
        report.message = write_error;
        return report;
    }
    report.sha256 = hooks::Sha256Hex(content);
    return report;
}

}  // namespace lubancode::trajectory
