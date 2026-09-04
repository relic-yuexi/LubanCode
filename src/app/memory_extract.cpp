// memory_extract.hpp 的实现。任务分型、转写压缩与 JSON 解析全是纯函数,
// 好单测;只有 RunMemoryExtraction 碰网络。

#include "app/memory_extract.hpp"

#include <initializer_list>
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

namespace {

// 转写里各部件的截断阈值。
constexpr std::size_t kMaxTextBytes = 4 * 1024;
constexpr std::size_t kMaxToolInputBytes = 300;
constexpr std::size_t kMaxToolResultBytes = 240;

std::string ClipBytes(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    text.resize(lubancode::platform::Utf8PrefixBoundary(text, max_bytes));
    return text + "...(截断)";
}

int TaskTypeScore(const std::string& haystack, std::initializer_list<const char*> needles) {
    int score = 0;
    for (const char* needle : needles) {
        if (haystack.find(needle) != std::string::npos) ++score;
    }
    return score;
}

}  // namespace

std::string ClassifyTaskType(const std::string& user_text, const std::vector<std::string>& tool_names) {
    // 工具名拼进判词:调过 read_file/search 偏调研,调过 write/edit/run 偏
    // 修代码。纯词法,不打请求,错了顶多总结侧重偏一点,不伤主链。
    std::string joined_tools;
    for (const std::string& name : tool_names) joined_tools += name + " ";
    const std::string haystack = user_text + "\n" + joined_tools;

    struct Scored {
        const char* name;
        int score;
    };
    const Scored scored[] = {
        {"config", TaskTypeScore(haystack, {"安装", "依赖", "环境", "install", "pip ", "npm ", "uv ", "uv\n",
                                            "conda", "venv", "版本", "编译", "构建", "build", "cmake",
                                            "package.json", "pyproject", "portable", "virtualenv"})},
        {"docs", TaskTypeScore(haystack, {"文档", "README", "readme", "注释", "说明书写", "document", "doc ",
                                          "文档化", "写份", "写一份"})},
        {"research", TaskTypeScore(haystack, {"看看", "在哪", "读一", "分析", "调研", "为什么", "是怎么回事",
                                              "梳理", "找一找", "搜一", "read_file", "search", "web_fetch",
                                              "web_search"})},
        {"code", TaskTypeScore(haystack, {"修复", "实现", "重构", "改一", "改掉", "加个", "加一", "删掉", "bug",
                                          "fix", "报错", "崩了", "write_file", "edit_file", "run_command",
                                          "lsp"})},
    };
    const Scored* best = nullptr;
    for (const Scored& item : scored) {
        if (item.score > 0 && (best == nullptr || item.score > best->score)) {
            best = &item;
        }
    }
    if (best == nullptr) return "other";
    return best->name;
}

std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes) {
    std::string out;
    const auto append = [&out, max_bytes](std::string line) {
        if (out.size() >= max_bytes) return;
        if (out.size() + line.size() + 1 > max_bytes) {
            const std::size_t room = max_bytes - out.size() - 1;
            if (room > 20) line.resize(lubancode::platform::Utf8PrefixBoundary(line, room));
            else line.clear();
        }
        if (!line.empty()) {
            if (!out.empty()) out += "\n";
            out += line;
        }
    };

    for (const api::Message& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                if (message.role == api::Role::User) {
                    append("[用户] " + ClipBytes(text->text, kMaxTextBytes));
                } else {
                    append("[助手] " + ClipBytes(text->text, kMaxTextBytes));
                }
            } else if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                append("[工具调用] " + use->name + "(" + ClipBytes(use->input.dump(), kMaxToolInputBytes) + ")");
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                append(std::string("[工具结果") + (result->is_error ? ",失败" : "") + "] " +
                       ClipBytes(result->content, kMaxToolResultBytes));
            }
            // ThinkingBlock/ImageBlock 不进转写。
        }
    }
    return out;
}

std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type) {
    const std::string base = agent::ModuleTextByPath(prompts_dir, "features/memory-summary-base.md");
    std::string typed = agent::ModuleTextByPath(prompts_dir, "features/memory-summary-" + task_type + ".md");
    if (typed.empty()) {
        typed = agent::ModuleTextByPath(prompts_dir, "features/memory-summary-other.md");
    }
    if (base.empty()) return typed;
    if (typed.empty()) return base;
    return base + "\n\n" + typed;
}

std::expected<MemoryExtraction, std::string> ParseExtractionJson(const std::string& text) {
    // 容错:模型有时仍裹 ```json 围栏,剥掉;取首个 { 到末个 }。
    std::size_t begin = text.find('{');
    std::size_t end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return std::unexpected("抽取输出里找不到 JSON object");
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.substr(begin, end - begin + 1));
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(std::string("抽取输出不是合法 JSON: ") + e.what());
    }
    if (!root.is_object()) return std::unexpected("抽取输出不是 JSON object");

    MemoryExtraction extraction;
    extraction.task_type = root.value("task_type", std::string("other"));
    if (extraction.task_type != "code" && extraction.task_type != "research" &&
        extraction.task_type != "config" && extraction.task_type != "docs") {
        extraction.task_type = "other";
    }
    extraction.summary = root.value("summary", std::string());
    if (root.contains("retrieval_terms") && root["retrieval_terms"].is_array()) {
        for (const auto& item : root["retrieval_terms"]) {
            if (item.is_string() && extraction.retrieval_terms.size() < 8) {
                extraction.retrieval_terms.push_back(item.get<std::string>());
            }
        }
    }
    if (root.contains("candidates") && root["candidates"].is_array()) {
        for (const auto& item : root["candidates"]) {
            if (!item.is_object() || extraction.candidates.size() >= 3) continue;
            ProposedCandidate candidate;
            candidate.kind = item.value("kind", std::string());
            if (candidate.kind != "fact" && candidate.kind != "preference" &&
                candidate.kind != "feedback") {
                continue;
            }
            candidate.title = item.value("title", std::string());
            candidate.summary = item.value("summary", std::string());
            candidate.content = item.value("content", std::string());
            candidate.confidence = item.value("confidence", std::string("inferred"));
            // 时间线锚点:材料里明确给出的日期才留;形状不像日期(模型编的
            // 相对时间、口语时间)一律落空,不造假也不拦整条候选。
            const std::string occurred = item.value("occurred_at", std::string());
            if (memory::LooksLikeMemoryDate(occurred)) candidate.occurred_at = occurred;
            if (candidate.title.empty() || candidate.content.empty()) continue;
            if (item.contains("keywords") && item["keywords"].is_array()) {
                for (const auto& keyword : item["keywords"]) {
                    if (keyword.is_string()) candidate.keywords.push_back(keyword.get<std::string>());
                }
            }
            if (item.contains("paths") && item["paths"].is_array()) {
                for (const auto& path : item["paths"]) {
                    if (path.is_string()) candidate.paths.push_back(path.get<std::string>());
                }
            }
            extraction.candidates.push_back(std::move(candidate));
        }
    }
    return extraction;
}

std::expected<MemoryExtraction, std::string> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs,
                                                                 const std::string& reasoning_effort,
                                                                 agent::BackgroundCallAccounting* accounting) {
    // 采样走 SampleModel 原语(批一·病四):攒流/usage/兜错/看门狗的路只有
    // 一份,这里只剩提示拼装与解析;错误只回 message(旧口径)。
    agent::SampleRequest sample;
    sample.model = model;
    sample.system = system_prompt;
    sample.reasoning_effort = reasoning_effort;
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{transcript});
    sample.messages.push_back(std::move(message));
    sample.max_tokens = 1500;

    agent::SampleOptions sample_options;
    sample_options.timeout_secs = timeout_secs;
    const agent::SampleResult sampled = agent::SampleModel(backend, sample, sample_options);

    // usage 出账(分角色记账):抽取这轮采样不混普通 turn 的账。
    if (accounting != nullptr) {
        accounting->usage.input_tokens += sampled.usage.input_tokens;
        accounting->usage.cache_read_tokens += sampled.usage.cache_read_tokens;
        accounting->usage.cache_creation_tokens += sampled.usage.cache_creation_tokens;
        accounting->usage.output_tokens += sampled.usage.output_tokens;
        accounting->usage.output_reasoning_tokens += sampled.usage.output_reasoning_tokens;
        accounting->usage_reported = sampled.usage_reported;
        accounting->duration_ms = sampled.duration_ms;
    }

    return FinishMemoryExtraction(sampled);
}

std::expected<MemoryExtraction, std::string> FinishMemoryExtraction(const agent::SampleResult& sampled) {
    if (!sampled.ok) return std::unexpected(sampled.error.message);
    if (sampled.text.empty()) return std::unexpected("抽取输出为空");
    return ParseExtractionJson(sampled.text);
}

// ---- 记忆写入调度单 P0(§六/§10):调度账实现 -----------------------------

MeaningfulTextStats ComputeMeaningfulTextStats(const std::string& text) {
    MeaningfulTextStats stats;
    // UTF-8 逐码点走:首字节定宽,续字节(0x80..0xBF)不重复计数。坏序列
    // 按单字节摊开,统计不炸即可——门控判定(P1)另有正反例单测钉着。
    std::uint32_t code_point = 0;
    int pending = 0;  // 还差几个续字节
    auto flush = [&]() {
        if (pending > 0) return;  // 半截序列,丢弃
        if ((code_point >= 0x4E00 && code_point <= 0x9FFF) ||
            (code_point >= 0x3400 && code_point <= 0x4DBF) ||
            (code_point >= 0xF900 && code_point <= 0xFAFF)) {
            ++stats.cjk_char_count;
        }
    };
    bool in_latin_word = false;
    const auto is_latin = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    for (const char byte : text) {
        const unsigned char c = static_cast<unsigned char>(byte);
        if (pending > 0) {
            if ((c & 0xC0) == 0x80) {
                code_point = (code_point << 6) | (c & 0x3F);
                if (--pending == 0) {
                    ++stats.unicode_scalar_count;
                    flush();
                }
                continue;
            }
            pending = 0;  // 坏续字节:落回按首字节重解
        }
        if (c < 0x80) {
            ++stats.unicode_scalar_count;
            code_point = c;
            flush();
            if (is_latin(c)) {
                if (!in_latin_word) {
                    in_latin_word = true;
                    ++stats.latin_word_count;
                }
            } else {
                in_latin_word = false;
            }
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            code_point = c & 0x1F;
            pending = 1;
        } else if ((c & 0xF0) == 0xE0) {
            code_point = c & 0x0F;
            pending = 2;
        } else if ((c & 0xF8) == 0xF0) {
            code_point = c & 0x07;
            pending = 3;
        } else {
            // 坏首字节(0x80..0xBF 孤续字节/0xF8+):按一个标量摊开。
            ++stats.unicode_scalar_count;
        }
    }
    return stats;
}

const char* ExtractionTriggerName(ExtractionTrigger trigger) {
    switch (trigger) {
        case ExtractionTrigger::EveryTurn: return "every_turn";
        case ExtractionTrigger::BatchWatermark: return "batch_watermark";
        case ExtractionTrigger::IdleTimeout: return "idle_timeout";
        case ExtractionTrigger::BeforeCompact: return "before_compact";
        case ExtractionTrigger::SessionEnd: return "session_end";
    }
    return "every_turn";
}

const char* ExtractionDecisionName(ExtractionDecision decision) {
    switch (decision) {
        case ExtractionDecision::Skipped: return "skipped";
        case ExtractionDecision::Called: return "called";
    }
    return "skipped";
}

const char* ExtractionSkipReasonName(ExtractionSkipReason reason) {
    switch (reason) {
        case ExtractionSkipReason::Disabled: return "disabled";
        case ExtractionSkipReason::NoNewHistory: return "no_new_history";
        case ExtractionSkipReason::EmptyTranscript: return "empty_transcript";
        case ExtractionSkipReason::PromptMissing: return "prompt_missing";
        case ExtractionSkipReason::ExtractModeOff: return "extract_mode_off";
        case ExtractionSkipReason::AlreadyMutated: return "already_mutated";
        case ExtractionSkipReason::ShortText: return "short_text";
        case ExtractionSkipReason::AcknowledgementOnly: return "acknowledgement_only";
        case ExtractionSkipReason::SlashCommandOnly: return "slash_command_only";
        case ExtractionSkipReason::NoDurableSignal: return "no_durable_signal";
    }
    return "disabled";
}

std::string StableExtractErrorCode(const std::string& error) {
    // ExtractTurnMemory 失败路的固定文案(编译期字面量);认不出落 other。
    if (error.starts_with("cheap 路由找不到 provider")) return "route_miss";
    if (error.starts_with("抽取输出为空")) return "empty_output";
    if (error.starts_with("抽取输出")) return "parse_failed";  // 不是合法 JSON / 找不到 object
    return "other";
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
    state_ = MemoryTurnState{};
    state_.session_id = std::move(session_id);
    state_.turn_id = std::move(turn_id);
    state_.user_text_stats = ComputeMeaningfulTextStats(user_text);
    state_.extraction_gate_decision = ExtractionDecision::Skipped;
    state_.extraction_gate_reason = ExtractionSkipReason::Disabled;
    turn_open_ = true;
    extraction_called_ = false;
    pending_outcome_ = ExtractOutcome{};
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
        RecordAssessedLocked(foreground_tail_ms);
    }
    turn_open_ = false;
    state_.turn_id.clear();  // 回合间的写路回执(slash 命令)不带回合号
}

void MemoryTurnLedger::RecordAssessedLocked(std::int64_t foreground_tail_ms) {
    if (trajectory_ == nullptr) return;
    auto* recorder = trajectory_->main();
    if (recorder == nullptr) return;

    nlohmann::json payload{
        {"trigger", ExtractionTriggerName(ExtractionTrigger::EveryTurn)},
        {"turn_id", state_.turn_id},
        {"decision", ExtractionDecisionName(state_.extraction_gate_decision)},
        {"user_text_stats",
         nlohmann::json{{"unicode_scalar_count", state_.user_text_stats.unicode_scalar_count},
                        {"cjk_char_count", state_.user_text_stats.cjk_char_count},
                        {"latin_word_count", state_.user_text_stats.latin_word_count}}},
        {"foreground_tail_ms", foreground_tail_ms},
    };
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
        payload["auto_written"] = outcome.auto_written;
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

void MemoryTurnLedger::RecordReceiptLocked(const memory::MemoryWriteReceipt& receipt,
                                           const std::string& turn_id) {
    if (trajectory_ == nullptr) return;
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
