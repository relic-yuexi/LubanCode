// Harness Exporter 实现(One-shot 轨迹指定输出单)。合同见
// harness_exporter.hpp。
//
// 引擎两层,全部纯读:
//   1. FoldStreamReplay——验链 + 折叠(turns/tools/requests/integrity),
//      结构权威;验链不过的流出一行 outcome=unknown 存根,不折正文;
//   2. projection 共享层的 raw 扫——purpose/usage/工具细账/run relations/
//      环境快照引用逐行取,与 training exporter 同一本底账。
//
// 隐私(单子 §四/§八):secret 扫描复用 insights 冻结的模式表,命中片段
// 整段替换(RedactSecrets),记录侧留 privacy_findings 稳定码——与
// training 的"整包扣下"不同,harness 要如实产出,处置面是脱敏不是丢行。
// 路径不脱敏:评测 harness 要认容器内路径,secret 才是红线。

#include "trajectory/harness_exporter.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "insights/redaction.hpp"
#include "platform/paths.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/export_projection.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/replay.hpp"

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
    char buffer[32];
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
    for (const auto& hit : insights::ScanSecrets(text)) {
        findings->push_back(PrivacyFinding{
            std::string("privacy.secret.") + insights::SecretKindName(hit.kind), event_id});
    }
    return insights::RedactSecrets(text);
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
                bool truncated = false;
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
    const std::string config_hash = ComputeHarnessConfigHash(options);
    const BlobStore blobs(session_dir / "artifacts");
    for (const auto& stream_path : CollectSessionStreams(session_dir)) {
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
        report.message = "session 目录不存在(会话没开 trajectory 便没有账,不造假)";
        return report;
    }
    if (CollectSessionStreams(session_dir).empty()) {
        report.error_code = "export.no_streams";
        report.message = "session 目录里没有 JSONL(没开 trajectory 的会话没有账)";
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
