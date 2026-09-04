// Training Exporter 实现(P0 新轨迹记录单 §十一/§十二/§14)。合同见
// training_exporter.hpp。
//
// 引擎分三层,全部纯读:
//   1. FoldStreamReplay(replay.hpp)——验链 + 折叠(turns/tools/evidence/
//      integrity/state hash),结构权威;
//   2. projection 共享层的 raw 扫(export_projection.hpp)——fold 不留的
//      字段按 envelope 逐行取(turn 归属、training_policy、is_error、
//      purpose、outcome.assessed payload、context.attached、环境快照档位、
//      证据新鲜度对照线);harness exporter 吃同一份,不另造第二本历史;
//   3. 逐 turn 编 episode,正文回读时顺路过隐私扫描,再裁四路、canonical
//      dump 写盘。
//
// 确定性(§16.5"同一 Journal 连 export 两次,规范输出字节一致"):
//   - stream 按相对路径字典序,turn 按折叠序,findings 去重保序;
//   - 一切输出走 CanonicalJsonDump;
//   - 墙钟不进产物(manifest 的"生成时间"锚在源账末事件 wall_time_ms 上,
//     那是 Journal 事实,不是导出时刻)。

#include "trajectory/training_exporter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "insights/redaction.hpp"
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/export_projection.hpp"  // 中立投影层(harness exporter 共用)
#include "trajectory/journal.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/replay.hpp"

namespace lubancode::trajectory {
namespace {

// §五"复用现有 Exporter":raw 扫/正文回读/隐私扫描/stream 清单/原子写
// 全部住在 projection 共享层,本件与 harness exporter 共用一份,不另造
// 第二本历史。
using namespace projection;

// ---------------------------------------------------------------------------
// episode 组装
// ---------------------------------------------------------------------------

struct EpisodeBuild {
    nlohmann::json episode;
    EpisodeRouting routing;
    EpisodeRoute route = EpisodeRoute::Excluded;
    std::vector<std::string> reasons;
    std::vector<PrivacyFinding> findings;
};

EpisodeBuild BuildTurnEpisode(const ReplayState& state, const RawStreamScan& raw,
                              const ReplayTurnEntry& turn, const BlobStore& blobs,
                              const TrainingExportOptions& options,
                              const std::string& structure_base, const std::string& config_hash) {
    EpisodeBuild build;
    const auto raw_it = raw.turns.find(turn.turn_id);
    const TurnRaw empty_turn;
    const TurnRaw& turn_raw = raw_it != raw.turns.end() ? raw_it->second : empty_turn;

    EpisodeRouting routing;
    routing.structure = structure_base;
    routing.replayability = MapReplayLevelToAbility(raw.replay_level);
    routing.turn_terminal = turn.terminal_state;
    routing.run_terminal = state.run_terminal_state;
    routing.completeness =
        !turn.terminal_state.empty() && !state.run_terminal_state.empty() ? "complete"
                                                                          : "incomplete";

    std::string structure_code = structure_base;
    nlohmann::json messages = nlohmann::json::array();
    nlohmann::json context_entries = nlohmann::json::array();
    std::map<std::string, const ReplayToolEntry*> folded_calls;
    for (const auto& entry : state.tools) {
        folded_calls[entry.call_id] = &entry;
    }

    // ---- 对话面事件按 seq 交错投影(§11.2 origin 映射) ----
    for (const auto& event : turn_raw.conversation) {
        const std::string tag = GetString(event, "tag");
        if (tag == "input") {
            const std::string origin = GetString(event, "origin");
            if (origin != "external_user" && origin != "queued_user") {
                // 宿主注入/peer/goal 续跑(§2.7 不变量 5):不冒充 user,只记
                // 指纹与来源,正文不进训练件。
                const std::string content_text = event["content"].is_array()
                                                     ? event["content"].dump()
                                                     : std::string();
                context_entries.push_back(nlohmann::json{
                    {"kind", "input"},
                    {"origin", origin},
                    {"event_id", GetString(event, "event_id")},
                    {"content_sha256", hooks::Sha256Hex(content_text)},
                });
                continue;
            }
            nlohmann::json content = nlohmann::json::array();
            for (const auto& block : event["content"]) {
                if (!block.is_object()) {
                    continue;
                }
                if (GetString(block, "type") == "text") {
                    const ResolvedText resolved = ResolveTextValue(
                        block.contains("text") ? block["text"] : nlohmann::json(), blobs,
                                             options.max_resolved_blob_bytes);
                    if (!resolved.ok) {
                        if (structure_code == "valid") {
                            structure_code = resolved.structure_code;
                        }
                        continue;
                    }
                    ScanTextForPrivacy(resolved.text, GetString(event, "event_id"),
                                       &build.findings);
                    content.push_back(nlohmann::json{{"type", "text"}, {"text", resolved.text}});
                } else {
                    // image 一类附件块:只是引用/文件名,无字节,原样带。
                    content.push_back(block);
                }
            }
            nlohmann::json message;
            message["role"] = "user";
            message["origin"] = origin;
            message["channel"] = GetString(event, "channel");
            message["policy"] = GetString(event, "policy");
            message["content"] = std::move(content);
            messages.push_back(std::move(message));
            continue;
        }
        if (tag == "output") {
            const auto purpose_it = raw.request_purpose.find(GetString(event, "request_id"));
            const std::string purpose =
                purpose_it != raw.request_purpose.end() ? purpose_it->second : std::string();
            if (!PurposeFoldsIntoConversation(purpose)) {
                continue;  // compact/起名/抽取一类宿主工作产物(§14.4)
            }
            nlohmann::json content = nlohmann::json::array();
            nlohmann::json tool_calls = nlohmann::json::array();
            for (const auto& block : event["blocks"]) {
                if (!block.is_object()) {
                    continue;
                }
                const std::string type = GetString(block, "type");
                if (type == "text" || (type == "thinking" && options.include_reasoning)) {
                    const ResolvedText resolved =
                        ResolveTextValue(block.contains("text") ? block["text"] : nlohmann::json(),
                                         blobs, options.max_resolved_blob_bytes);
                    if (!resolved.ok) {
                        if (structure_code == "valid") {
                            structure_code = resolved.structure_code;
                        }
                        continue;
                    }
                    ScanTextForPrivacy(resolved.text, GetString(event, "event_id"),
                                       &build.findings);
                    content.push_back(nlohmann::json{{"type", type}, {"text", resolved.text}});
                } else if (type == "image_ref") {
                    content.push_back(block);  // 引用块,无字节
                } else if (type == "tool_call") {
                    nlohmann::json call;
                    call["call_id"] = GetString(block, "call_id");
                    if (block.contains("provider_call_id") &&
                        block["provider_call_id"].is_string()) {
                        call["provider_call_id"] = block["provider_call_id"];
                    }
                    call["name"] = GetString(block, "name");
                    call["arguments"] = block.contains("arguments")
                                            ? block["arguments"]
                                            : nlohmann::json::object();
                    const auto dumped = CanonicalJsonDump(call["arguments"]);
                    if (dumped.has_value()) {
                        ScanTextForPrivacy(*dumped, GetString(event, "event_id"), &build.findings);
                    }
                    tool_calls.push_back(std::move(call));
                }
                // thinking 且未开 include_reasoning:默认剔除(§11.6),不补不猜。
            }
            nlohmann::json message;
            message["role"] = "assistant";
            message["origin"] = "provider_model";
            message["policy"] = GetString(event, "policy");
            message["stop_reason"] = GetString(event, "stop_reason");
            message["content"] = std::move(content);
            message["tool_calls"] = std::move(tool_calls);
            messages.push_back(std::move(message));
            continue;
        }
        if (tag == "result") {
            const std::string call_id = GetString(event, "call_id");
            nlohmann::json content = nlohmann::json::array();
            for (const auto& block : event["content"]) {
                if (!block.is_object() || GetString(block, "type") != "text") {
                    continue;
                }
                const ResolvedText resolved = ResolveTextValue(
                    block.contains("text") ? block["text"] : nlohmann::json(), blobs,
                                             options.max_resolved_blob_bytes);
                if (!resolved.ok) {
                    if (structure_code == "valid") {
                        structure_code = resolved.structure_code;
                    }
                    continue;
                }
                ScanTextForPrivacy(resolved.text, GetString(event, "event_id"), &build.findings);
                content.push_back(nlohmann::json{{"type", "text"}, {"text", resolved.text}});
            }
            nlohmann::json message;
            message["role"] = "tool";
            message["call_id"] = call_id;
            message["origin"] = "tool";
            const auto folded = folded_calls.find(call_id);
            if (folded != folded_calls.end() && !folded->second->tool_name.empty()) {
                message["tool_name"] = folded->second->tool_name;
            }
            message["policy"] = GetString(event, "policy");
            message["is_error"] = GetBool(event, "is_error");
            message["content"] = std::move(content);
            messages.push_back(std::move(message));
        }
    }
    for (const auto& context : turn_raw.contexts) {
        context_entries.push_back(context);  // 引用不带头部正文,原样带
    }

    // ---- steps(§5.4 台账;子代理边界只进 metadata,不展开子账正文) ----
    nlohmann::json steps = nlohmann::json::array();
    for (const auto& [call_id, call_raw] : raw.calls) {
        if (call_raw.turn_id != turn.turn_id) {
            continue;
        }
        nlohmann::json step;
        step["call_id"] = call_id;
        step["tool_name"] = call_raw.tool_name;
        const auto folded = folded_calls.find(call_id);
        if (folded != folded_calls.end()) {
            const ReplayToolEntry& entry = *folded->second;
            step["planned"] = entry.planned;
            step["effective"] = entry.effective;
            step["started"] = entry.started;
            step["terminal_kind"] = entry.terminal_kind;
            step["outcome"] = entry.outcome;
            step["result_committed"] = entry.result_committed;
            if (entry.child_run_id.has_value()) {
                // §11.1:父文件只带边界引用;子轨迹在它自己的文件里导。
                step["child_run_id"] = *entry.child_run_id;
                if (!entry.child_terminal_event_hash.empty()) {
                    step["child_terminal_event_hash"] = entry.child_terminal_event_hash;
                }
            }
            if (entry.UnknownSideEffect()) {
                routing.unknown_side_effect = true;
            }
        }
        step["side_effects_recorded"] = call_raw.has_side_effects;
        steps.push_back(std::move(step));
    }

    // ---- evidence 与 verification 轴 ----
    nlohmann::json evidence = nlohmann::json::array();
    std::map<std::string, const ReplayEvidenceEntry*> evidence_by_id;
    for (const auto& entry : state.evidence) {
        if (turn_raw.verification_events.count(entry.verification_id) > 0) {
            evidence.push_back(entry.ToJson());
        }
        evidence_by_id[entry.verification_id] = &entry;
    }
    nlohmann::json assessed_outcome = nlohmann::json();
    nlohmann::json criteria = nlohmann::json::array();
    nlohmann::json evidence_refs = nlohmann::json::array();
    if (turn_raw.assessed.is_object()) {
        assessed_outcome = GetString(turn_raw.assessed, "outcome");
        if (turn_raw.assessed.contains("criteria") && turn_raw.assessed["criteria"].is_array()) {
            criteria = turn_raw.assessed["criteria"];
        }
        if (turn_raw.assessed.contains("evidence_refs") &&
            turn_raw.assessed["evidence_refs"].is_array()) {
            evidence_refs = turn_raw.assessed["evidence_refs"];
        }
    }
    routing.assessed_outcome =
        assessed_outcome.is_string() ? assessed_outcome.get<std::string>() : std::string();
    routing.verification = "unverified";
    if (turn_raw.assessed.is_object()) {
        bool all_good = !evidence_refs.empty();
        for (const auto& ref : evidence_refs) {
            if (!ref.is_object()) {
                all_good = false;
                continue;
            }
            const auto it = evidence_by_id.find(GetString(ref, "verification_id"));
            if (it == evidence_by_id.end() || !it->second->passed) {
                routing.verification = "insufficient";
                all_good = false;
                continue;
            }
            if (it->second->invalidated) {
                routing.verification = "stale";
                all_good = false;
                continue;
            }
            // §11.5"evidence 晚于相关最后修改":observed_after_seq 不得早于
            // 本 turn 带副作用工具的终态。
            if (it->second->observed_after_seq < turn_raw.last_mutating_seq) {
                routing.verification = "stale";
                all_good = false;
            }
        }
        if (all_good && routing.verification == "unverified") {
            routing.verification = "verified";
        }
    }

    // ---- 隐私轴与结构轴收口 ----
    build.findings = DedupeFindings(build.findings);
    routing.privacy = build.findings.empty() ? "passed" : "excluded";
    routing.structure = structure_code;

    // ---- 四路裁断 ----
    build.route = DecideEpisodeRoute(routing, &build.reasons);
    // 隐私细码并进 reasons:manifest 的 exclusion_reasons 按 §13.1 逐码计数,
    // 不能只留一句笼统的 privacy.excluded。
    for (const auto& finding : build.findings) {
        if (std::find(build.reasons.begin(), build.reasons.end(), finding.code) ==
            build.reasons.end()) {
            build.reasons.push_back(finding.code);
        }
    }

    // ---- 组装 episode(§11.1 全形) ----
    nlohmann::json episode;
    episode["schema"] = kTrainingEpisodeSchema;
    episode["schema_version"] = kTrainingEpisodeSchemaVersion;
    episode["episode_id"] = state.run_id + ":" + turn.turn_id;
    nlohmann::json source;
    source["workspace_key"] = state.workspace_key;
    source["session_id"] = state.session_id;
    source["run_id"] = state.run_id;
    source["run_kind"] = RunKindName(state.run_kind);
    source["turn_id"] = turn.turn_id;
    source["journal_last_hash"] = state.integrity.last_event_hash;
    source["exporter_version"] = kTrainingExporterVersion;
    episode["source"] = std::move(source);
    // 隐私命中:正文整包扣下,excluded.jsonl 不当泄密出口(§十二)。
    episode["messages"] =
        routing.privacy == "passed" ? std::move(messages) : nlohmann::json::array();
    episode["steps"] = std::move(steps);
    nlohmann::json outcome;
    outcome["turn_terminal"] = turn.terminal_state;
    outcome["claimed_outcome"] = turn_raw.claimed_outcome;
    outcome["assessed_outcome"] = assessed_outcome;
    outcome["criteria"] = std::move(criteria);
    outcome["evidence_refs"] = std::move(evidence_refs);
    episode["outcome"] = std::move(outcome);
    episode["evidence"] = std::move(evidence);
    if (!context_entries.empty()) {
        episode["context"] = std::move(context_entries);
    }
    nlohmann::json quality;
    quality["structure"] = routing.structure;
    quality["outcome"] = routing.assessed_outcome.empty() ? turn_raw.claimed_outcome
                                                          : routing.assessed_outcome;
    quality["verification"] = routing.verification;
    quality["privacy"] = routing.privacy;
    quality["replayability"] = routing.replayability;
    quality["completeness"] = routing.completeness;
    quality["training_eligible"] = build.route != EpisodeRoute::Excluded;
    quality["reasons"] = build.reasons;
    episode["quality"] = std::move(quality);
    nlohmann::json replay;
    replay["replay_level"] = raw.replay_level;
    replay["gaps"] = raw.gaps;
    replay["state_hash"] = ComputeReplayStateHash(state);
    replay["folded_seq"] = state.folded_seq;
    episode["replay"] = std::move(replay);
    episode["fingerprint"] = ComputeEpisodeFingerprint(
        state.run_id, turn.turn_id, state.integrity.last_event_hash, config_hash);
    if (!build.findings.empty()) {
        nlohmann::json findings = nlohmann::json::array();
        for (const auto& finding : build.findings) {
            findings.push_back(nlohmann::json{{"code", finding.code},
                                              {"source_event_id", finding.source_event_id}});
        }
        episode["privacy_findings"] = std::move(findings);
    }

    build.episode = std::move(episode);
    build.routing = std::move(routing);
    return build;
}

// ---------------------------------------------------------------------------
// stream 清单与原子写:住在 projection 共享层(CollectSessionStreams /
// WriteTextAtomically),本件与 harness exporter 同一份。
// ---------------------------------------------------------------------------

// 破账存根:链断/超前版本的 stream 出一枚 run 级 excluded,不折正文。
ExportedEpisode BuildBrokenStreamStub(const RawStreamScan& raw,
                                      const std::filesystem::path& stream_path,
                                      const std::string& structure_code,
                                      const std::string& config_hash) {
    ExportedEpisode stub;
    EpisodeRouting routing;
    routing.structure = structure_code;
    routing.privacy = "passed";
    routing.replayability = MapReplayLevelToAbility(raw.replay_level);
    routing.completeness = "incomplete";
    routing.verification = "unverified";
    stub.route = DecideEpisodeRoute(routing, &stub.reasons);

    const std::string run_id = raw.run_id.empty() ? stream_path.stem().string() : raw.run_id;
    nlohmann::json episode;
    episode["schema"] = kTrainingEpisodeSchema;
    episode["schema_version"] = kTrainingEpisodeSchemaVersion;
    episode["episode_id"] = run_id + ":stream";
    nlohmann::json source;
    source["workspace_key"] = raw.workspace_key;
    source["session_id"] = raw.session_id;
    source["run_id"] = run_id;
    source["run_kind"] = raw.run_kind_name;
    source["turn_id"] = nullptr;
    source["stream"] = platform::PathToUtf8(stream_path.filename());
    source["exporter_version"] = kTrainingExporterVersion;
    episode["source"] = std::move(source);
    episode["messages"] = nlohmann::json::array();
    episode["steps"] = nlohmann::json::array();
    episode["outcome"] = nlohmann::json::object();
    episode["evidence"] = nlohmann::json::array();
    nlohmann::json quality;
    quality["structure"] = routing.structure;
    quality["outcome"] = "unknown";
    quality["verification"] = routing.verification;
    quality["privacy"] = routing.privacy;
    quality["replayability"] = routing.replayability;
    quality["completeness"] = routing.completeness;
    quality["training_eligible"] = false;
    quality["reasons"] = stub.reasons;
    episode["quality"] = std::move(quality);
    episode["fingerprint"] = ComputeEpisodeFingerprint(run_id, "stream", "", config_hash);
    stub.episode = std::move(episode);
    stub.fingerprint = ComputeEpisodeFingerprint(run_id, "stream", "", config_hash);
    stub.run_id = run_id;
    stub.turn_key = "stream";
    return stub;
}

}  // namespace

// ---------------------------------------------------------------------------
// 枚举名与路由
// ---------------------------------------------------------------------------

const char* EpisodeRouteName(EpisodeRoute route) {
    switch (route) {
        case EpisodeRoute::Success:
            return "success";
        case EpisodeRoute::Failure:
            return "failure";
        case EpisodeRoute::Partial:
            return "partial";
        case EpisodeRoute::Excluded:
            return "excluded";
    }
    return "excluded";
}

std::optional<EpisodeRoute> EpisodeRouteFromName(std::string_view name) {
    if (name == "success") {
        return EpisodeRoute::Success;
    }
    if (name == "failure") {
        return EpisodeRoute::Failure;
    }
    if (name == "partial") {
        return EpisodeRoute::Partial;
    }
    if (name == "excluded") {
        return EpisodeRoute::Excluded;
    }
    return std::nullopt;
}

const char* EpisodeRouteFileName(EpisodeRoute route) {
    switch (route) {
        case EpisodeRoute::Success:
            return "success.jsonl";
        case EpisodeRoute::Failure:
            return "failure.jsonl";
        case EpisodeRoute::Partial:
            return "partial.jsonl";
        case EpisodeRoute::Excluded:
            return "excluded.jsonl";
    }
    return "excluded.jsonl";
}

EpisodeRoute DecideEpisodeRoute(const EpisodeRouting& routing, std::vector<std::string>* reasons) {
    const auto add = [&](const std::string& code) {
        if (reasons != nullptr) {
            reasons->push_back(code);
        }
    };
    // §11.5 成功门的硬挡次序:结构 -> 隐私 -> replay 档 -> unknown side effect。
    if (routing.structure != "valid") {
        add("structure." + routing.structure);
        return EpisodeRoute::Excluded;
    }
    if (routing.privacy != "passed") {
        add("privacy." + routing.privacy);
        return EpisodeRoute::Excluded;
    }
    if (routing.replayability != "exact_offline" && routing.replayability != "source_exact") {
        // P0-5 训练门:只有 exact / source_exact 级的 run 配进训练集
        //(§9.2/§11.4);其余如实 excluded,不降档凑数。
        add(routing.replayability == "unknown" ? "replay.level_missing"
                                               : "replay.level_below_floor");
        return EpisodeRoute::Excluded;
    }
    if (routing.unknown_side_effect) {
        add("tool.unknown_side_effect");
        return EpisodeRoute::Excluded;
    }
    if (routing.completeness != "complete") {
        // 预算耗尽、人工打断、运行未收口(§11.3 partial 路)。
        add("completeness.incomplete");
        return EpisodeRoute::Partial;
    }
    // 到这里结构完整、收口干净;成败看 turn/run 终态与证据。
    const bool turn_ok = routing.turn_terminal == "turn.completed";
    const bool run_ok = routing.run_terminal == "run.completed";
    if (turn_ok && run_ok) {
        if (routing.verification == "verified" && routing.assessed_outcome == "succeeded") {
            return EpisodeRoute::Success;
        }
        // 任务成了却没证据:只进 excluded/unverified(§11.4);证据失效或
        // 不够同样不许冒充 success。
        add(routing.verification == "unverified" ? "verification.unverified"
                                                 : "verification." + routing.verification);
        return EpisodeRoute::Excluded;
    }
    // 已知失败/取消,结构完整(§11.3 failure 路)。
    add("outcome." +
        (routing.run_terminal.empty() ? routing.turn_terminal : routing.run_terminal));
    return EpisodeRoute::Failure;
}

// ---------------------------------------------------------------------------
// 指纹
// ---------------------------------------------------------------------------

std::string ComputeExporterConfigHash(const TrainingExportOptions& options) {
    nlohmann::json config;
    config["include_reasoning"] = options.include_reasoning;
    config["max_resolved_blob_bytes"] = options.max_resolved_blob_bytes;
    config["exporter_version"] = kTrainingExporterVersion;
    config["episode_schema_version"] = kTrainingEpisodeSchemaVersion;
    const auto dumped = CanonicalJsonDump(config);
    return hooks::Sha256Hex(dumped.has_value() ? *dumped : config.dump());
}

std::string ComputeEpisodeFingerprint(std::string_view run_id, std::string_view turn_key,
                                      std::string_view journal_last_hash,
                                      std::string_view exporter_config_hash) {
    std::string input;
    input.reserve(run_id.size() + turn_key.size() + journal_last_hash.size() +
                  exporter_config_hash.size());
    input.append(run_id);
    input.append(turn_key);
    input.append(journal_last_hash);
    input.append(exporter_config_hash);
    return hooks::Sha256Hex(input);
}

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------

std::vector<ExportedEpisode> BuildSessionTrainingEpisodes(const std::filesystem::path& session_dir,
                                                          const TrainingExportOptions& options) {
    std::vector<ExportedEpisode> episodes;
    const std::string config_hash = ComputeExporterConfigHash(options);
    const BlobStore blobs(session_dir / "artifacts");
    for (const auto& stream_path : CollectSessionStreams(session_dir)) {
        const RawStreamScan raw = ScanStreamRaw(stream_path);
        const auto fold = FoldStreamReplay(stream_path);
        if (!fold.ok()) {
            // 链/schema 坏或版本超前:fail-closed,只出存根(§8.4)。
            episodes.push_back(BuildBrokenStreamStub(
                raw, stream_path,
                fold.error_code == "replay.unsupported" ? "unsupported" : "verify_failed",
                config_hash));
            continue;
        }
        std::string structure_base = "valid";
        if (fold.state.integrity.truncated_tail) {
            structure_base = "truncated_tail";  // §16.3:截断明报,不伪造终态
        }
        for (const auto& turn : fold.state.turns) {
            EpisodeBuild build = BuildTurnEpisode(fold.state, raw, turn, blobs, options,
                                                  structure_base, config_hash);
            // §11.1 的训练单位是真对话轮:宿主旁路小请求(compact/起名/抽取)
            // 折下来没有 messages 也没有 steps,不当 episode 编。隐私命中的
            // 例外——正文虽扣下,审计存根必须留(excluded 是账,不是可丢项)。
            const bool privacy_hit = build.routing.privacy != "passed";
            const bool has_messages =
                build.episode["messages"].is_array() && !build.episode["messages"].empty();
            const bool has_steps =
                build.episode["steps"].is_array() && !build.episode["steps"].empty();
            if (!has_messages && !has_steps && !privacy_hit) {
                continue;
            }
            ExportedEpisode exported;
            exported.episode = std::move(build.episode);
            exported.route = build.route;
            exported.reasons = std::move(build.reasons);
            exported.fingerprint = GetString(exported.episode, "fingerprint");
            exported.run_id = GetString(exported.episode["source"], "run_id");
            exported.turn_key = turn.turn_id;
            episodes.push_back(std::move(exported));
        }
    }
    return episodes;
}

TrainingExportReport ExportSessionTrainingV1(const std::filesystem::path& session_dir,
                                              const TrainingExportOptions& options) {
    TrainingExportReport report;
    std::error_code ec;
    if (!std::filesystem::is_directory(session_dir, ec)) {
        report.error_code = "export.no_session_dir";
        report.message = "session 目录不存在(会话没开 trajectory 便没有账,不造假)";
        return report;
    }
    const auto streams = CollectSessionStreams(session_dir);
    if (streams.empty()) {
        report.error_code = "export.no_streams";
        report.message = "session 目录里没有 JSONL(没开 trajectory 的会话没有账)";
        return report;
    }
    report.streams = streams.size();

    const auto episodes = BuildSessionTrainingEpisodes(session_dir, options);
    report.episodes = episodes.size();
    std::map<EpisodeRoute, std::string> route_contents;
    for (const auto& episode : episodes) {
        const auto dumped = CanonicalJsonDump(episode.episode);
        if (!dumped.has_value()) {
            report.error_code = "export.internal_error";
            report.message = "episode canonical 序列化失败: " + dumped.error();
            return report;
        }
        route_contents[episode.route].append(*dumped);
        route_contents[episode.route].append("\n");
        ++report.counts[EpisodeRouteName(episode.route)];
        for (const std::string& reason : episode.reasons) {
            ++report.exclusion_reasons[reason];
        }
    }

    // 写盘前的磁盘余量门(§12.2 storage_exhausted 同款判据)。
    const auto export_dir = session_dir / "exports" / std::string(kTrainingExportFormat);
    if (!HasDiskReserve(export_dir, options.min_free_bytes)) {
        report.error_code = "export.storage_exhausted";
        report.message = "磁盘余量低于导出门(§12.2),一个字节不写";
        return report;
    }

    // manifest(§12.3):config hash、过滤规则、授权来源;生成时间锚在源账
    // 末事件上(墙钟不进产物,§16.5 字节确定);逐份文件 sha256 可校验。
    nlohmann::json manifest;
    manifest["schema"] = kTrainingDatasetSchema;
    manifest["schema_version"] = kTrainingDatasetSchemaVersion;
    manifest["format"] = kTrainingExportFormat;
    nlohmann::json source;
    source["session_id"] = platform::PathToUtf8(session_dir.filename());
    const auto main_fold = FoldStreamReplay(session_dir / "main.jsonl");
    if (main_fold.ok()) {
        source["journal_last_hash"] = main_fold.state.integrity.last_event_hash;
    }
    std::int64_t last_wall_time = 0;
    for (const auto& stream_path : streams) {
        const RawStreamScan raw = ScanStreamRaw(stream_path);
        last_wall_time = std::max(last_wall_time, raw.last_wall_time_ms);
        if (!source.contains("workspace_key") && !raw.workspace_key.empty()) {
            source["workspace_key"] = raw.workspace_key;
        }
    }
    source["source_last_event_wall_time_ms"] = last_wall_time;
    manifest["source"] = std::move(source);
    manifest["exporter_version"] = kTrainingExporterVersion;
    manifest["exporter_config_hash"] = ComputeExporterConfigHash(options);
    nlohmann::json filter_rules;
    nlohmann::json floor = nlohmann::json::array();
    floor.push_back("exact");
    floor.push_back("source_exact_environment_partial");
    filter_rules["replay_level_floor"] = std::move(floor);
    filter_rules["reasoning_policy"] = options.include_reasoning ? "included" : "excluded";
    nlohmann::json privacy_scan = nlohmann::json::array();
    privacy_scan.push_back("secret");
    privacy_scan.push_back("personal_path");
    privacy_scan.push_back("absolute_path");
    privacy_scan.push_back("binary");
    filter_rules["privacy_scan"] = std::move(privacy_scan);
    filter_rules["max_resolved_blob_bytes"] = options.max_resolved_blob_bytes;
    manifest["filter_rules"] = std::move(filter_rules);
    nlohmann::json authorization;
    authorization["source"] = "explicit_cli_command";
    authorization["auto_export_training"] = false;  // §12.3 默认关
    manifest["authorization"] = std::move(authorization);
    nlohmann::json counts;
    for (const auto& [name, count] : report.counts) {
        counts[name] = count;
    }
    manifest["counts"] = std::move(counts);
    manifest["exclusion_reasons"] = report.exclusion_reasons;

    // 四路文件 + manifest,各自临时件 + 原子 rename。
    nlohmann::json files = nlohmann::json::array();
    for (const auto& route : {EpisodeRoute::Success, EpisodeRoute::Failure, EpisodeRoute::Partial,
                              EpisodeRoute::Excluded}) {
        const std::string& content = route_contents[route];
        const auto target = export_dir / EpisodeRouteFileName(route);
        std::string error;
        if (!WriteTextAtomically(target, content, &error)) {
            report.error_code = "export.write_failed";
            report.message = error;
            return report;
        }
        files.push_back(nlohmann::json{{"name", EpisodeRouteFileName(route)},
                                       {"episodes", report.counts[EpisodeRouteName(route)]},
                                       {"sha256", hooks::Sha256Hex(content)}});
    }
    manifest["files"] = std::move(files);
    const auto manifest_dumped = CanonicalJsonDump(manifest);
    if (!manifest_dumped.has_value()) {
        report.error_code = "export.internal_error";
        report.message = "manifest canonical 序列化失败: " + manifest_dumped.error();
        return report;
    }
    std::string manifest_error;
    if (!WriteTextAtomically(export_dir / "manifest.json", *manifest_dumped + "\n",
                             &manifest_error)) {
        report.error_code = "export.write_failed";
        report.message = manifest_error;
        return report;
    }
    report.export_dir = export_dir;
    return report;
}

TrainingExportReport ExportWorkspaceTrainingV1(const std::filesystem::path& workspace_dir,
                                                const TrainingExportOptions& options) {
    TrainingExportReport report;
    const auto sessions = workspace_dir / "sessions";
    std::error_code ec;
    if (!std::filesystem::is_directory(sessions, ec)) {
        report.error_code = "export.no_session_dir";
        report.message = "workspace 没有 sessions 目录";
        return report;
    }
    std::vector<std::filesystem::path> session_dirs;
    for (const auto& entry : std::filesystem::directory_iterator(sessions, ec)) {
        if (ec) {
            break;
        }
        std::error_code dir_ec;
        if (entry.is_directory(dir_ec) && !dir_ec) {
            session_dirs.push_back(entry.path());
        }
    }
    if (session_dirs.empty()) {
        report.error_code = "export.no_streams";
        report.message = "workspace 里没有 session";
        return report;
    }
    std::sort(session_dirs.begin(), session_dirs.end());
    for (const auto& session_dir : session_dirs) {
        if (CollectSessionStreams(session_dir).empty()) {
            continue;  // 没账的目录不造假
        }
        const auto one = ExportSessionTrainingV1(session_dir, options);
        if (!one.ok()) {
            report.error_code = one.error_code;
            report.message = platform::PathToUtf8(session_dir.filename()) + ": " + one.message;
            return report;
        }
        report.streams += one.streams;
        report.episodes += one.episodes;
        for (const auto& [name, count] : one.counts) {
            report.counts[name] += count;
        }
        for (const auto& [reason, count] : one.exclusion_reasons) {
            report.exclusion_reasons[reason] += count;
        }
        report.export_dir = one.export_dir;  // 逐 session 各自落,末场留索引
    }
    if (report.episodes == 0 && report.streams == 0) {
        report.error_code = "export.no_streams";
        report.message = "workspace 里没有可导的轨迹账";
        return report;
    }
    return report;
}

}  // namespace lubancode::trajectory
