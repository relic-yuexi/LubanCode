// memory_extract.hpp 的实现。任务分型、转写压缩与 JSON 解析全是纯函数,
// 好单测;只有 RunMemoryExtraction 碰网络。

#include "app/memory_extract.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "agent/prompt_assembler.hpp"
#include "agent/sample_model.hpp"  // SampleModel 原语:采样的公共路(批一·病四)
#include "api/backend.hpp"
#include "memory/project_memory.hpp"  // LooksLikeMemoryDate:occurred_at 的清洗
#include "platform/text_encoding.hpp"
#include "runtime/trajectory_session.hpp"  // MemoryTurnLedger 的落账口(P0 调度账)
#include "trajectory/recorder.hpp"

namespace lubancode::app {
namespace extraction = agent::memory_extraction;
const char* ExtractionErrorCodeName(ExtractionErrorCode code) {
    return extraction::ExtractionErrorCodeName(code);
}
std::string ClassifyTaskType(const std::string& text, const std::vector<std::string>& tools) {
    return extraction::ClassifyTaskType(text, tools);
}
std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes) {
    return extraction::BuildTurnTranscript(messages, max_bytes);
}
std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type) {
    return extraction::BuildExtractionSystemPrompt(prompts_dir, task_type);
}
std::expected<MemoryExtraction, ExtractionError> ParseExtractionJson(const std::string& text) {
    return extraction::ParseExtractionJson(text);
}
std::expected<MemoryExtraction, ExtractionError> RunMemoryExtraction(
    api::Backend& backend, const std::string& model, const std::string& system_prompt,
    const std::string& transcript, int timeout_secs, const std::string& reasoning_effort,
    agent::BackgroundCallAccounting* accounting, agent::LoopBoundaryRecorder* recorder,
    const std::atomic<bool>* cancel) {
    return extraction::RunMemoryExtraction(backend, model, system_prompt, transcript,
                                         timeout_secs, reasoning_effort, accounting, recorder, cancel);
}
std::expected<MemoryExtraction, ExtractionError> FinishMemoryExtraction(const agent::SampleResult& sampled) {
    return extraction::FinishMemoryExtraction(sampled);
}
const nlohmann::json& MemoryExtractionOutputSchema() {
    return extraction::MemoryExtractionOutputSchema();
}

// ---- 记忆写入调度单 P0(§六/§10)账本 + P1(§7)门控实现 -------------------

bool MemoryGateShadowEnabled() {
    const char* raw = std::getenv("LUBANCODE_MEMORY_GATE_SHADOW");
    if (raw == nullptr) return false;
    const std::string value = raw;
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

std::string StableExtractErrorCode(const ExtractionError& error) {
    // 结构化版(P0-A 起):六类新码 + route_miss。旧账里 syntax_invalid/
    // utf8_invalid/schema_invalid 统称 parse_failed,离线对账按此折算。
    return app::ExtractionErrorCodeName(error.code);
}

std::string StableExtractErrorCode(const std::string& error) {
    // 旧文案版(保留):ExtractTurnMemory 失败路的固定文案(编译期字面量);
    // 认不出落 other。新文案以"抽取输出"开头,这把尺子继续量得准。
    if (error.starts_with("cheap 路由找不到 provider")) return "route_miss";
    if (error.starts_with("抽取输出为空")) return "empty_output";
    if (error.starts_with("抽取输出")) return "parse_failed";  // 不是合法 JSON / 找不到 object
    return "other";
}

std::uint64_t AutoQueuedFromAssessedPayload(const nlohmann::json& payload) {
    // 修复单 §五 D 的读侧统一口:新键优先,旧键是同一计数的历史名(排队
    // 数,不是落盘数)。nlohmann 缺键经 value() 安全取默认,不碰 UB。
    if (!payload.is_object()) return 0;
    for (const char* key : {"autoQueued", "auto_queued"}) {
        const auto found = payload.find(key);
        if (found != payload.end() && found->is_number_unsigned()) {
            return found->get<std::uint64_t>();
        }
    }
    for (const char* key : {"autoWritten", "auto_written"}) {
        const auto found = payload.find(key);
        if (found != payload.end() && found->is_number_unsigned()) {
            return found->get<std::uint64_t>();
        }
    }
    return 0;
}

namespace {

// §10.1 漏斗的 skip 计数器与 reason 的对账(一处收口,漏斗不散架)。
void CountSkip(ExtractionFunnel& funnel, ExtractionSkipReason reason) {
    switch (reason) {
        case ExtractionSkipReason::Disabled: ++funnel.skipped_disabled; break;
        case ExtractionSkipReason::NoNewHistory: ++funnel.skipped_no_new_history; break;
        case ExtractionSkipReason::EmptyTranscript: ++funnel.skipped_empty_transcript; break;
        case ExtractionSkipReason::PromptMissing: ++funnel.skipped_prompt_missing; break;
        // P1/P3 接线后才轮到这五枚。
        case ExtractionSkipReason::ShortText: ++funnel.skipped_short; break;
        case ExtractionSkipReason::AcknowledgementOnly: ++funnel.skipped_ack; break;
        case ExtractionSkipReason::SlashCommandOnly: ++funnel.skipped_command; break;
        case ExtractionSkipReason::AlreadyMutated: ++funnel.skipped_already_mutated; break;
        case ExtractionSkipReason::NoDurableSignal: ++funnel.skipped_no_durable_signal; break;
        case ExtractionSkipReason::ExtractModeOff: break;  // 档位账归配置,P0 不数
    }
}

}  // namespace

MemoryTurnLedger::MemoryTurnLedger(runtime::TrajectorySessionLedger* trajectory)
    : trajectory_(trajectory) {}
MemoryTurnLedger::~MemoryTurnLedger() = default;

void MemoryTurnLedger::BeginTurn(std::string session_id, std::string turn_id,
                                 const std::string& user_text) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // 悬账冲账(回合总结异步化单):上一轮门过起飞、收账还没赶上新轮
    // 开张——悬账以 aborted 口径落袋(decision=Called 而 outcome 缺席,
    // RecordAssessedLocked 的既有兜底),不编数字,也不阻塞新轮。
    if (!suspended_turn_id_.empty()) {
        RecordAssessedLocked(0, suspended_turn_id_);
        suspended_turn_id_.clear();
    }
    state_ = MemoryTurnState{};
    state_.session_id = std::move(session_id);
    state_.turn_id = std::move(turn_id);
    state_.user_text_stats = ComputeMeaningfulTextStats(user_text);
    state_.extraction_gate_decision = ExtractionDecision::Skipped;
    state_.extraction_gate_reason = ExtractionSkipReason::Disabled;
    turn_open_ = true;
    extraction_called_ = false;
    pending_outcome_ = ExtractOutcome{};
    gate_context_noted_ = false;
    turn_has_tool_evidence_ = false;
    shadow_evaluated_ = false;
    ++funnel_.outer_user_turns;
}

void MemoryTurnLedger::NoteExtractionSkipped(ExtractionSkipReason reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.extraction_gate_decision = ExtractionDecision::Skipped;
    state_.extraction_gate_reason = reason;
    CountSkip(funnel_, reason);
}

void MemoryTurnLedger::NoteHistoryGrew() {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++funnel_.history_grew_turns;
}

bool MemoryTurnLedger::turn_mutated() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return turn_open_ && !(state_.successful_save_ids.empty() && state_.successful_forget_ids.empty()
                           && state_.accepted_candidate_ids.empty());
}

void MemoryTurnLedger::NoteGateContext(bool has_tool_evidence) {
    const std::lock_guard<std::mutex> lock(mutex_);
    gate_context_noted_ = true;
    turn_has_tool_evidence_ = has_tool_evidence;
}

void MemoryTurnLedger::NoteDurableSignals(const std::vector<std::string>& reasons) {
    const std::lock_guard<std::mutex> lock(mutex_);
    shadow_evaluated_ = true;
    state_.durable_signal_reasons = reasons;
}

void MemoryTurnLedger::NoteExtractionCalled() {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.extraction_gate_decision = ExtractionDecision::Called;
    extraction_called_ = true;
    ++funnel_.extract_batches;
    ++funnel_.eligible_turns;
}

void MemoryTurnLedger::NoteExtractionOutcome(const ExtractOutcome& outcome) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_outcome_ = outcome;
    if (!outcome.ok) ++funnel_.extract_failures;
}

void MemoryTurnLedger::OnMemoryWriteReceipt(const memory::MemoryWriteReceipt& receipt) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string turn_id = turn_open_ ? state_.turn_id : std::string();
    // 本轮写入账(§6.1):排队成功按路分账;被拒记稳定码。job_id 是排进
    // pending 的文件名——排队≠落盘,P0 不冒充 committed。
    if (receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued) {
        if (receipt.operation == "forget") {
            state_.successful_forget_ids.push_back(receipt.job_id);
        } else {
            state_.successful_save_ids.push_back(receipt.job_id);
            if (receipt.source == memory::MemoryWriteSource::CandidateAccept) {
                // accept 的凭证即 job:候选文件当场删了,job 名是留得住的号。
                state_.accepted_candidate_ids.push_back(receipt.job_id);
            }
        }
    } else {
        state_.rejected_write_codes.push_back(receipt.error_code);
    }
    RecordReceiptLocked(receipt, turn_id);
}

void MemoryTurnLedger::FinishTurn(std::int64_t foreground_tail_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (turn_open_) {
        RecordAssessedLocked(foreground_tail_ms, state_.turn_id);
    }
    turn_open_ = false;
    suspended_turn_id_.clear();  // FinishTurn 落过袋的回合没有悬账(纪律:二选一)
    state_.turn_id.clear();  // 回合间的写路回执(slash 命令)不带回合号
}

void MemoryTurnLedger::SuspendTurn() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!turn_open_) {
        return;
    }
    // 悬账:state_(门决策/漏斗材料)原样留着等迟到收账补 outcome;轮先
    // 关(回合间口径:写路回执不挂轮号)。悬账期间 slash 写路回执还会写
    // state_ 的写入账——那是回合间的事,RecordAssessedLocked 不读它,串
    // 不进这轮的 assessed。
    suspended_turn_id_ = state_.turn_id;
    turn_open_ = false;
    state_.turn_id.clear();
}

bool MemoryTurnLedger::SettleSuspendedTurn(const std::string& turn_id, std::int64_t settle_wall_ms,
                                           const ExtractOutcome* outcome) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (suspended_turn_id_.empty() || suspended_turn_id_ != turn_id) {
        // 对不上档:悬账已被冲(BeginTurn 记了 aborted)或弃过(换代)。
        // 真失败仍数进漏斗——漏斗是"一场会话"的聚合,不因回合翻篇丢数。
        if (outcome != nullptr && !outcome->ok) {
            ++funnel_.extract_failures;
        }
        return false;
    }
    if (outcome != nullptr) {
        pending_outcome_ = *outcome;
        if (!outcome->ok) {
            ++funnel_.extract_failures;
        }
    }
    RecordAssessedLocked(settle_wall_ms, suspended_turn_id_);
    suspended_turn_id_.clear();
    state_ = MemoryTurnState{};  // 悬账期间攒下的回合间回执不串进下一笔
    pending_outcome_ = ExtractOutcome{};
    return true;
}

void MemoryTurnLedger::AbandonSuspendedTurn() {
    const std::lock_guard<std::mutex> lock(mutex_);
    // 换代弃账:不落盘、不记漏斗(这一轮连"评估完成"都到不了新场的账
    // 上)。state_ 留给下一轮 BeginTurn 重置。
    suspended_turn_id_.clear();
}

void MemoryTurnLedger::RecordAssessedLocked(std::int64_t foreground_tail_ms,
                                            const std::string& trigger_turn_id) {
    if (trajectory_ == nullptr) return;
    // v3 场走 v3 写口(取消误报 ESC 单 Bug 2):typed 事件 + camelCase 载荷,
    // 不往 v3 卷塞 v2 行;v2 老路一字不动。
    if (auto* v3_writer = trajectory_->v3_main_writer()) {
        RecordAssessedV3Locked(*v3_writer, foreground_tail_ms, trigger_turn_id);
        return;
    }
    auto* recorder = trajectory_->main();
    if (recorder == nullptr) return;

    nlohmann::json payload{
        {"trigger", ExtractionTriggerName(ExtractionTrigger::EveryTurn)},
        {"turn_id", trigger_turn_id},
        {"decision", ExtractionDecisionName(state_.extraction_gate_decision)},
        {"user_text_stats",
         nlohmann::json{{"unicode_scalar_count", state_.user_text_stats.unicode_scalar_count},
                        {"cjk_char_count", state_.user_text_stats.cjk_char_count},
                        {"latin_word_count", state_.user_text_stats.latin_word_count},
                        // P1(§3.2)补全的三项:六项一并算齐、一并落账,shadow
                        // 报告与门槛判定离线可复算。
                        {"code_token_count", state_.user_text_stats.code_token_count},
                        {"only_acknowledgement", state_.user_text_stats.only_acknowledgement},
                        {"only_slash_command", state_.user_text_stats.only_slash_command}}},
        {"foreground_tail_ms", foreground_tail_ms},
    };
    // P1(§7.1):工具证据在场否——ack 门与耐久信号的"须有工具证据"靠它
    // 离线复算。走不到转写扫描的回合(disabled/no_new_history/同轮去重)
    // 不落键。
    if (gate_context_noted_) {
        payload["has_tool_evidence"] = turn_has_tool_evidence_;
    }
    // P1(§7.2 shadow):耐久信号逐回合落账,离线重放可复算漏判。开着才
    // 落(空表 = 评过、一条没中);关着事件与 P0 同形。
    if (shadow_evaluated_) {
        payload["shadow_gate"] = nlohmann::json{
            {"durable_signal", state_.durable_signal_reasons.empty() ? "none" : "hit"},
            {"signals", state_.durable_signal_reasons}};
    }
    if (state_.extraction_gate_decision == ExtractionDecision::Skipped) {
        payload["skip_reason"] = ExtractionSkipReasonName(state_.extraction_gate_reason);
    } else {
        // called:收口材料齐上报;outcome 没送到(异常路)按 aborted 报,
        // 不编数字。provider 没报 usage 时 token 三项整组缺席(§10.3)。
        ExtractOutcome outcome = pending_outcome_;
        if (!outcome.ok && outcome.error_code.empty()) {
            // ok=false 且无稳定码:收口没走到,记 aborted。
            outcome.error_code = "aborted";
        }
        payload["extract_outcome"] = outcome.ok ? "completed" : "failed";
        if (!outcome.ok) payload["error_code"] = outcome.error_code;
        payload["extract_wall_ms"] = outcome.extract_wall_ms;
        payload["review_candidates"] = outcome.review_candidates;
        payload["auto_queued"] = outcome.auto_queued;
        if (outcome.usage_reported) {
            payload["usage_reported"] = true;
            payload["input_tokens"] = outcome.input_tokens;
            payload["output_tokens"] = outcome.output_tokens;
            payload["cached_tokens"] = outcome.cached_tokens;
        }
    }

    trajectory::EventScope scope = recorder->base_scope();
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.actor = trajectory::Actor::Host;
    scope.origin = trajectory::Origin::ScheduledHost;
    scope.visibility = {trajectory::Visibility::HostOnly};
    scope.training_policy = trajectory::TrainingPolicy::Exclude;

    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::MemoryExtractionAssessed;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    // 落不稳只吞(诊断口径同 MemoryLedgerBridge):调度账不许反过来
    // 拖垮回合收尾。
    (void)recorder->Record(request, trajectory::Durability::ProcessCrash);
}

void MemoryTurnLedger::RecordAssessedV3Locked(trajectory::v3::V3Writer& writer,
                                              std::int64_t foreground_tail_ms,
                                              const std::string& trigger_turn_id) {
    // v3 的 assessed 事实行(取消误报 ESC 单 Bug 2):字段与 v2 同一套账
    //(跳过原因/决策/收口材料/墙钟/失败码),键名随 v3 合同走 camelCase;
    // turnId 挂触发它的主回合,重开会话单凭事件答得出"哪次抽取、预算
    // 多久、实际多久、谁叫停"。
    nlohmann::json payload{
        {"trigger", ExtractionTriggerName(ExtractionTrigger::EveryTurn)},
        {"turnId", trigger_turn_id},
        {"decision", ExtractionDecisionName(state_.extraction_gate_decision)},
        {"userTextStats",
         nlohmann::json{{"unicodeScalarCount", state_.user_text_stats.unicode_scalar_count},
                        {"cjkCharCount", state_.user_text_stats.cjk_char_count},
                        {"latinWordCount", state_.user_text_stats.latin_word_count},
                        {"codeTokenCount", state_.user_text_stats.code_token_count},
                        {"onlyAcknowledgement", state_.user_text_stats.only_acknowledgement},
                        {"onlySlashCommand", state_.user_text_stats.only_slash_command}}},
        {"foregroundTailMs", foreground_tail_ms},
    };
    if (gate_context_noted_) {
        payload["hasToolEvidence"] = turn_has_tool_evidence_;
    }
    if (shadow_evaluated_) {
        payload["shadowGate"] = nlohmann::json{
            {"durableSignal", state_.durable_signal_reasons.empty() ? "none" : "hit"},
            {"signals", state_.durable_signal_reasons}};
    }
    if (state_.extraction_gate_decision == ExtractionDecision::Skipped) {
        payload["skipReason"] = ExtractionSkipReasonName(state_.extraction_gate_reason);
    } else {
        ExtractOutcome outcome = pending_outcome_;
        if (!outcome.ok && outcome.error_code.empty()) {
            outcome.error_code = "aborted";  // 收口没走到,不编数字
        }
        payload["extractOutcome"] = outcome.ok ? "completed" : "failed";
        if (!outcome.ok) payload["errorCode"] = outcome.error_code;
        payload["extractWallMs"] = outcome.extract_wall_ms;
        payload["reviewCandidates"] = outcome.review_candidates;
        payload["autoQueued"] = outcome.auto_queued;
        if (outcome.usage_reported) {
            payload["usageReported"] = true;
            payload["inputTokens"] = outcome.input_tokens;
            payload["outputTokens"] = outcome.output_tokens;
            payload["cachedTokens"] = outcome.cached_tokens;
        }
    }
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::MemoryExtractionAssessed;
    if (!trigger_turn_id.empty()) {
        draft.turn_id = trigger_turn_id;
    }
    draft.payload = std::move(payload);
    (void)writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
}

void MemoryTurnLedger::RecordReceiptV3Locked(trajectory::v3::V3Writer& writer,
                                             const memory::MemoryWriteReceipt& receipt,
                                             const std::string& turn_id) {
    nlohmann::json payload{
        {"source", memory::MemoryWriteSourceName(receipt.source)},
        {"operation", receipt.operation},
        {"outcome", memory::MemoryWriteReceiptOutcomeName(receipt.outcome)},
        {"layer", receipt.layer},
    };
    if (!receipt.kind.empty()) payload["kind"] = receipt.kind;
    if (!turn_id.empty()) payload["turnId"] = turn_id;
    if (receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued) {
        payload["jobId"] = receipt.job_id;
    } else {
        payload["errorCode"] = receipt.error_code;
    }
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::MemoryWriteReceipted;
    if (!turn_id.empty()) {
        draft.turn_id = turn_id;
    }
    draft.payload = std::move(payload);
    (void)writer.AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
}

void MemoryTurnLedger::RecordReceiptLocked(const memory::MemoryWriteReceipt& receipt,
                                           const std::string& turn_id) {
    if (trajectory_ == nullptr) return;
    // v3 场走 v3 写口(Bug 2 同门):receipted 事实行,载荷 camelCase。
    if (auto* v3_writer = trajectory_->v3_main_writer()) {
        RecordReceiptV3Locked(*v3_writer, receipt, turn_id);
        return;
    }
    auto* recorder = trajectory_->main();
    if (recorder == nullptr) return;

    nlohmann::json payload{
        {"source", memory::MemoryWriteSourceName(receipt.source)},
        {"operation", receipt.operation},
        {"outcome", memory::MemoryWriteReceiptOutcomeName(receipt.outcome)},
        {"layer", receipt.layer},
    };
    if (!receipt.kind.empty()) payload["kind"] = receipt.kind;
    if (!turn_id.empty()) payload["turn_id"] = turn_id;
    if (receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued) {
        payload["job_id"] = receipt.job_id;
    } else {
        payload["error_code"] = receipt.error_code;
    }

    trajectory::EventScope scope = recorder->base_scope();
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    // 谁发起的写:显式命令与候选接受归 user,模型工具归 tool,宿主抽取
    // 归 host(与 memory.save.requested 的 actor 口径同款)。
    switch (receipt.source) {
        case memory::MemoryWriteSource::ExplicitCommandSave:
        case memory::MemoryWriteSource::ExplicitForget:
        case memory::MemoryWriteSource::CandidateAccept:
            scope.actor = trajectory::Actor::User;
            scope.origin = trajectory::Origin::ExternalUser;
            break;
        case memory::MemoryWriteSource::ModelToolSave:
            scope.actor = trajectory::Actor::Tool;
            scope.origin = trajectory::Origin::BuiltinTool;
            break;
        case memory::MemoryWriteSource::AutoExtraction:
            scope.actor = trajectory::Actor::Host;
            scope.origin = trajectory::Origin::ScheduledHost;
            break;
    }
    scope.visibility = {trajectory::Visibility::HostOnly};
    scope.training_policy = trajectory::TrainingPolicy::Exclude;

    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::MemoryWriteReceipted;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    (void)recorder->Record(request, trajectory::Durability::ProcessCrash);
}

}  // namespace lubancode::app
