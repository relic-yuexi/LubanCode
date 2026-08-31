// 纯 TelemetryProjector 的实现。合同见 projector.hpp 文件头。
//
// 折叠算法:一遍顺序扫 EventEnvelope(seq 序),起事件开 span、终事件收
// span;悬空收口(开没关)按"missing"明标,不冒充终态。确定性来源:
//   - id 由 identity 层 HMAC 派生(key + 起事件 id + 角色);
//   - 时间全部取信封双时钟,不读墙钟;
//   - 输出排序按起事件 id / metric 名,与扫描顺序无关地稳定。
#include "telemetry/projector.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <utility>

#include "telemetry/identity.hpp"
#include "trajectory/event.hpp"
#include "trajectory/journal.hpp"

namespace lubancode::telemetry {
namespace {

using trajectory::EventEnvelope;
using trajectory::EventKind;

// 折叠期的一枚在册 span(closed 后不再改)。
struct OpenSpan {
    TraceSpan span;
    bool closed = false;
};

void AttrStr(nlohmann::json& attributes, const char* key, std::string value) {
    if (!value.empty()) {
        attributes.emplace(key, std::move(value));
    }
}

void AttrInt(nlohmann::json& attributes, const char* key, std::int64_t value) {
    attributes.emplace(key, value);
}

void AttrBool(nlohmann::json& attributes, const char* key, bool value) {
    attributes.emplace(key, value);
}

// 字节数入桶(§11.4:字节数用桶,不发精确大值)。
std::string BytesBucket(std::size_t bytes) {
    if (bytes == 0) {
        return "0";
    }
    if (bytes <= 1024) {
        return "<=1k";
    }
    if (bytes <= 4096) {
        return "<=4k";
    }
    if (bytes <= 16384) {
        return "<=16k";
    }
    if (bytes <= 65536) {
        return "<=64k";
    }
    return ">64k";
}

struct PreparedInfo {
    std::string model;
    std::string provider;
};

struct ToolInfo {
    std::string tool_name;
    std::string source_kind;
    std::string effect_class;
    std::string input_bucket;
};

// metric 累计器:同名同 labels 聚在一张卡上;输出前按 (name, labels dump)
// 排序,保证稳定。
class MetricSink {
public:
    void Add(std::string name, nlohmann::json labels) {
        const std::string key = name + "|" + labels.dump();
        auto it = samples_.find(key);
        if (it == samples_.end()) {
            MetricSample sample;
            sample.name = std::move(name);
            sample.labels = std::move(labels);
            sample.value = 1;
            samples_.emplace(std::move(key), std::move(sample));
            return;
        }
        it->second.value += 1;
    }

    void AddTokens(const std::string& kind, std::int64_t delta) {
        if (delta <= 0) {
            return;  // §12.4:没报不写 0
        }
        const std::string key = "lubancode.model.tokens|" + kind;
        auto it = samples_.find(key);
        if (it == samples_.end()) {
            MetricSample sample;
            sample.name = "lubancode.model.tokens";
            sample.labels = nlohmann::json{{"kind", kind}};
            sample.value = static_cast<std::uint64_t>(delta);
            samples_.emplace(key, std::move(sample));
            return;
        }
        it->second.value += static_cast<std::uint64_t>(delta);
    }

    std::vector<MetricSample> Take() {
        std::vector<MetricSample> out;
        out.reserve(samples_.size());
        for (auto& [key, sample] : samples_) {
            out.push_back(std::move(sample));
        }
        std::sort(out.begin(), out.end(), [](const MetricSample& a, const MetricSample& b) {
            if (a.name != b.name) {
                return a.name < b.name;
            }
            return a.labels.dump() < b.labels.dump();
        });
        return out;
    }

private:
    std::map<std::string, MetricSample> samples_;
};

// 折叠器:一趟顺序扫描的全部状态。
class StreamProjector {
public:
    StreamProjector(ProjectorOptions options, ProjectionReport& report)
        : options_(std::move(options)), report_(report) {}

    void Fold(const EventEnvelope& envelope) {
        if (trace_id_.empty()) {
            trace_id_ =
                DeriveTraceId(options_.projection_key, envelope.session_id, envelope.run_id);
            report_.workspace_key = envelope.workspace_key;
            report_.session_id = envelope.session_id;
            report_.run_id = envelope.run_id;
        }
        switch (envelope.kind) {
            case EventKind::RunStarted: OpenRun(envelope); break;
            case EventKind::RunCompleted:
            case EventKind::RunFailed:
            case EventKind::RunCancelled: CloseRun(envelope); break;
            case EventKind::TurnStarted: OpenTurn(envelope); break;
            case EventKind::TurnCompleted:
            case EventKind::TurnFailed:
            case EventKind::TurnCancelled: CloseTurn(envelope); break;
            case EventKind::ModelRequestPrepared: NotePrepared(envelope); break;
            case EventKind::ModelRequestSent: OpenRequest(envelope); break;
            case EventKind::ModelOutputCompleted:
            case EventKind::ModelOutputFailed:
            case EventKind::ModelOutputCancelled: CloseRequest(envelope); break;
            case EventKind::ModelUsageRecorded: NoteUsage(envelope); break;
            case EventKind::ToolExecutionPlanned: NoteToolPlanned(envelope); break;
            case EventKind::ToolInputEffective: NoteToolEffective(envelope); break;
            case EventKind::ToolExecutionStarted: OpenTool(envelope); break;
            case EventKind::ToolExecutionFinished:
            case EventKind::ToolExecutionFailed:
            case EventKind::ToolExecutionCancelled:
            case EventKind::ToolExecutionUnknown: CloseTool(envelope); break;
            case EventKind::ControlApprovalRequested: OpenApproval(envelope); break;
            case EventKind::ControlApprovalResolved:
            case EventKind::ControlApprovalExpired: CloseApproval(envelope); break;
            case EventKind::CompactRequested: OpenCompact(envelope); break;
            case EventKind::CompactApplied:
            case EventKind::CompactFailed:
            case EventKind::CompactCancelled:
            case EventKind::CompactRejected: CloseCompact(envelope); break;
            case EventKind::VerificationStarted: OpenVerification(envelope); break;
            case EventKind::VerificationRecorded:
            case EventKind::VerificationInvalidated: CloseVerification(envelope); break;
            default: break;  // 其余事件不产 span(§11.2 表外);照常计数
        }
        report_.events_projected += 1;
    }

    // 流末收口:开着的 span 按缺终态明标,不冒充。
    void Finish() {
        CloseIfOpen(run_span_, "run");
        for (auto& [turn_id, span] : turns_) {
            CloseIfOpen(span, "turn");
        }
        for (auto& [request_id, span] : requests_) {
            CloseIfOpen(span, "request");
        }
        for (auto& [call_id, span] : tools_) {
            CloseIfOpen(span, "tool");
        }
        for (auto& [approval_id, span] : approvals_) {
            CloseIfOpen(span, "approval");
        }
        CloseIfOpen(compact_span_, "compact");
        for (auto& [verification_id, span] : verifications_) {
            CloseIfOpen(span, "verification");
        }
    }

    std::vector<TraceSpan> TakeSpans() {
        std::vector<TraceSpan> out;
        out.reserve(spans_.size());
        for (OpenSpan& open : spans_) {
            out.push_back(std::move(open.span));
        }
        // 起事件 id 内嵌零填 seq,同 run 内字典序即 seq 序;跨 run 也稳定。
        std::sort(out.begin(), out.end(), [](const TraceSpan& a, const TraceSpan& b) {
            return a.source_event_id < b.source_event_id;
        });
        return out;
    }

    MetricSink& metrics() { return metrics_; }
    const std::string& trace_id() const { return trace_id_; }
    std::vector<std::string> TakeWarnings() { return std::move(warnings_); }

private:
    OpenSpan* NewSpan(const EventEnvelope& start, const char* name, const char* role) {
        spans_.emplace_back();
        OpenSpan& open = spans_.back();
        open.span.trace_id = trace_id_;
        open.span.span_id = DeriveSpanId(options_.projection_key, start.event_id, role);
        open.span.name = name;
        open.span.start_unix_nano = start.wall_time_ms * 1000000;
        open.span.end_unix_nano = open.span.start_unix_nano;
        open.span.start_monotonic_ns = start.monotonic_ns;
        open.span.end_monotonic_ns = start.monotonic_ns;
        open.span.source_event_id = start.event_id;
        return &open;
    }

    // 父解析(§11.1 树):turn 一律挂 run;其余挂当前 open turn,没有挂 run。
    OpenSpan* ParentOf(const std::optional<std::string>& turn_id) {
        if (turn_id.has_value()) {
            auto it = turns_.find(*turn_id);
            if (it != turns_.end() && !it->second->closed) {
                return it->second;
            }
        }
        return run_span_;
    }

    void Close(OpenSpan* span, const EventEnvelope& terminal, StatusCode status,
               std::string description, nlohmann::json attributes) {
        if (span == nullptr || span->closed) {
            Warn("unmatched_terminal:" + terminal.event_id);
            return;
        }
        span->closed = true;
        span->span.end_unix_nano = terminal.wall_time_ms * 1000000;
        span->span.end_monotonic_ns = terminal.monotonic_ns;
        span->span.status = status;
        span->span.status_description = std::move(description);
        for (auto it = attributes.begin(); it != attributes.end(); ++it) {
            span->span.attributes[it.key()] = it.value();
        }
        span->span.source_terminal_event_id = terminal.event_id;
    }

    void CloseIfOpen(OpenSpan* span, const char* role) {
        if (span == nullptr || span->closed) {
            return;
        }
        span->closed = true;
        AttrStr(span->span.attributes, "lubancode.span.terminal", "missing");
        Warn(std::string("open_span_missing_terminal:") + role);
    }

    void Warn(std::string warning) { warnings_.push_back(std::move(warning)); }

    // ---- 各族开收口 ----

    void OpenRun(const EventEnvelope& envelope) {
        OpenSpan* span = NewSpan(envelope, "lubancode.agent.run", "run");
        AttrStr(span->span.attributes, "lubancode.run.kind",
                trajectory::RunKindName(envelope.run_kind));
        AttrStr(span->span.attributes, "lubancode.run.start_reason",
                envelope.payload.value("start_reason", std::string()));
        run_span_ = span;
        metrics_.Add("lubancode.session.started_total", nlohmann::json::object());
    }

    void CloseRun(const EventEnvelope& envelope) {
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string description;
        if (envelope.kind == EventKind::RunFailed) {
            status = StatusCode::Error;
            description = envelope.payload.value("error_code", std::string("failed"));
            AttrStr(attributes, "error.type", description);
        } else if (envelope.kind == EventKind::RunCancelled) {
            status = StatusCode::Error;
            description = "cancelled";
        }
        Close(run_span_, envelope, status, std::move(description), std::move(attributes));
    }

    void OpenTurn(const EventEnvelope& envelope) {
        if (!envelope.turn_id.has_value()) {
            Warn("turn_started_without_turn_id:" + envelope.event_id);
            return;
        }
        OpenSpan* span = NewSpan(envelope, "lubancode.agent.turn", "turn");
        span->span.parent_span_id = run_span_ != nullptr ? run_span_->span.span_id : "";
        const std::string trigger = envelope.payload.value("trigger", std::string());
        AttrStr(span->span.attributes, "lubancode.turn.trigger", trigger);
        turns_[*envelope.turn_id] = span;
        metrics_.Add("lubancode.turn.started_total", nlohmann::json{{"trigger", trigger}});
    }

    void CloseTurn(const EventEnvelope& envelope) {
        if (!envelope.turn_id.has_value()) {
            return;
        }
        auto it = turns_.find(*envelope.turn_id);
        OpenSpan* span = it != turns_.end() ? it->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome;
        std::string description;
        if (envelope.kind == EventKind::TurnCompleted) {
            outcome = envelope.payload.value("outcome", std::string("completed"));
        } else if (envelope.kind == EventKind::TurnFailed) {
            outcome = "failed";
            status = StatusCode::Error;
            description = envelope.payload.value("error_code", std::string("failed"));
            AttrStr(attributes, "error.type", description);
        } else {
            outcome = "cancelled";
            status = StatusCode::Error;
            description = "cancelled";
        }
        AttrStr(attributes, "lubancode.turn.outcome", outcome);
        Close(span, envelope, status, std::move(description), std::move(attributes));
        metrics_.Add("lubancode.turn.completed_total", nlohmann::json{{"outcome", outcome}});
    }

    void NotePrepared(const EventEnvelope& envelope) {
        if (!envelope.request_id.has_value()) {
            return;
        }
        PreparedInfo info;
        info.model = envelope.payload.value("model", std::string());
        info.provider = envelope.payload.value("provider", std::string());
        prepared_[*envelope.request_id] = std::move(info);
    }

    void OpenRequest(const EventEnvelope& envelope) {
        if (!envelope.request_id.has_value()) {
            Warn("request_sent_without_request_id:" + envelope.event_id);
            return;
        }
        const std::string& request_id = *envelope.request_id;
        // 同 request_id 的重试各开一枚 span(§11.3);旧 attempt 未收口属
        // 非常态,按 missing 收掉再开新的。
        auto existing = requests_.find(request_id);
        if (existing != requests_.end() && !existing->second->closed) {
            CloseIfOpen(existing->second, "request");
        }
        OpenSpan* span = NewSpan(envelope, "gen_ai.request", "gen_ai.request");
        span->span.parent_span_id = ParentOf(envelope.turn_id) != nullptr
                                        ? ParentOf(envelope.turn_id)->span.span_id
                                        : std::string();
        const auto prepared = prepared_.find(request_id);
        if (prepared != prepared_.end()) {
            AttrStr(span->span.attributes, "gen_ai.request.model", prepared->second.model);
            AttrStr(span->span.attributes, "gen_ai.request.provider",
                    prepared->second.provider);
        }
        AttrInt(span->span.attributes, "gen_ai.request.attempt",
                static_cast<std::int64_t>(envelope.payload.value("attempt", std::uint64_t(1))));
        // retry_of 关联(§11.3):指向前一枚 attempt 的 span。
        if (envelope.relations.contains("retry_of") &&
            envelope.relations.at("retry_of").is_string()) {
            const std::string retry_of = envelope.relations.at("retry_of").get<std::string>();
            auto previous = request_spans_.find(retry_of);
            if (previous != request_spans_.end()) {
                SpanLink link;
                link.trace_id = trace_id_;
                link.span_id = previous->second;
                link.relation = "retry_of";
                span->span.links.push_back(std::move(link));
            }
        }
        request_spans_[envelope.event_id] = span->span.span_id;
        requests_[request_id] = span;
    }

    void CloseRequest(const EventEnvelope& envelope) {
        if (!envelope.request_id.has_value()) {
            return;
        }
        auto it = requests_.find(*envelope.request_id);
        OpenSpan* span = it != requests_.end() ? it->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome = "completed";
        std::string description;
        if (envelope.kind == EventKind::ModelOutputCompleted) {
            AttrStr(attributes, "gen_ai.request.stop_reason",
                    envelope.payload.value("stop_reason", std::string()));
        } else if (envelope.kind == EventKind::ModelOutputFailed) {
            outcome = "failed";
            status = StatusCode::Error;
            description = envelope.payload.value("error_code", std::string("model_error"));
            AttrStr(attributes, "error.type", description);
        } else {
            outcome = "cancelled";
            status = StatusCode::Error;
            description = "cancelled";
        }
        const auto prepared = prepared_.find(*envelope.request_id);
        const std::string provider =
            prepared != prepared_.end() ? prepared->second.provider : std::string("unknown");
        // v1 stream:usage 随 completed payload(legacy 读,§11.2)。
        if (envelope.kind == EventKind::ModelOutputCompleted && envelope.payload.contains("usage") &&
            envelope.payload.at("usage").is_object()) {
            ApplyUsage(envelope.payload.at("usage"), true, span, attributes);
        }
        Close(span, envelope, status, std::move(description), std::move(attributes));
        metrics_.Add("lubancode.model.request_total",
                     nlohmann::json{{"provider", provider}, {"outcome", outcome}});
    }

    // usage 五项(v1 legacy 或 v2 canonical)进 request span 属性与 metric。
    void ApplyUsage(const nlohmann::json& usage, bool reported, OpenSpan* span,
                    nlohmann::json& attributes) {
        const char* names[] = {"input_tokens",  "output_tokens", "cache_read_tokens",
                               "cache_creation_tokens", "reasoning_tokens"};
        const char* kinds[] = {"input", "output", "cache_read", "cache_creation", "reasoning"};
        for (int i = 0; i < 5; ++i) {
            if (!usage.contains(names[i]) || !usage.at(names[i]).is_number()) {
                continue;
            }
            const std::int64_t value = usage.at(names[i]).get<std::int64_t>();
            if (span != nullptr) {
                // 属性键随 schema 名(gen_ai.usage.input_tokens),metric
                // label 才用短名(kind=input)——两套命名,各对各家合同。
                const std::string key = std::string("gen_ai.usage.") + names[i];
                AttrInt(span->span.attributes, key.c_str(), value);
            }
            metrics_.AddTokens(kinds[i], value);
        }
        // §12.4:没报写 coverage=unknown,不写 0。
        const char* coverage = reported ? "provider" : "unknown";
        if (span != nullptr) {
            AttrStr(span->span.attributes, "gen_ai.usage.coverage", coverage);
        }
    }

    void NoteUsage(const EventEnvelope& envelope) {
        if (!envelope.request_id.has_value()) {
            return;
        }
        const bool reported = envelope.payload.value("reported_by_provider", false);
        auto it = requests_.find(*envelope.request_id);
        OpenSpan* span = (it != requests_.end() && !it->second->closed) ? it->second : nullptr;
        if (span == nullptr) {
            Warn("late_usage:" + *envelope.request_id);
        }
        nlohmann::json sink;
        if (reported) {
            ApplyUsage(envelope.payload, reported, span, sink);
        } else if (span != nullptr) {
            AttrStr(span->span.attributes, "gen_ai.usage.coverage", "unknown");
        }
    }

    void NoteToolPlanned(const EventEnvelope& envelope) {
        if (!envelope.call_id.has_value()) {
            return;
        }
        tools_info_[*envelope.call_id].tool_name =
            envelope.payload.value("tool_name", std::string());
    }

    void NoteToolEffective(const EventEnvelope& envelope) {
        if (!envelope.call_id.has_value()) {
            return;
        }
        ToolInfo& info = tools_info_[*envelope.call_id];
        info.source_kind = envelope.payload.value("source_kind", std::string());
        info.effect_class = envelope.payload.value("effect_class", std::string());
        if (envelope.payload.contains("effective_arguments")) {
            info.input_bucket =
                BytesBucket(envelope.payload.at("effective_arguments").dump().size());
        }
    }

    void OpenTool(const EventEnvelope& envelope) {
        if (!envelope.call_id.has_value()) {
            Warn("tool_started_without_call_id:" + envelope.event_id);
            return;
        }
        OpenSpan* span = NewSpan(envelope, "lubancode.tool.execute", "tool");
        span->span.parent_span_id = ParentOf(envelope.turn_id) != nullptr
                                        ? ParentOf(envelope.turn_id)->span.span_id
                                        : std::string();
        const std::string& call_id = *envelope.call_id;
        const auto info = tools_info_.find(call_id);
        if (info != tools_info_.end()) {
            AttrStr(span->span.attributes, "tool.name", info->second.tool_name);
            AttrStr(span->span.attributes, "tool.kind", info->second.source_kind);
            AttrStr(span->span.attributes, "tool.effect_class", info->second.effect_class);
            AttrStr(span->span.attributes, "tool.input_bytes_bucket", info->second.input_bucket);
        }
        AttrStr(span->span.attributes, "tool.batch_id",
                envelope.payload.value("batch_id", std::string()));
        if (envelope.payload.contains("position_in_batch")) {
            AttrInt(span->span.attributes, "tool.sequence_in_batch",
                    static_cast<std::int64_t>(
                        envelope.payload.at("position_in_batch").get<std::uint64_t>()));
        }
        tools_[call_id] = span;
    }

    void CloseTool(const EventEnvelope& envelope) {
        if (!envelope.call_id.has_value()) {
            return;
        }
        auto it = tools_.find(*envelope.call_id);
        OpenSpan* span = it != tools_.end() ? it->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome;
        std::string description;
        const auto info = tools_info_.find(*envelope.call_id);
        const std::string tool_kind =
            info != tools_info_.end() ? info->second.source_kind : std::string("unknown");
        switch (envelope.kind) {
            case EventKind::ToolExecutionFinished: {
                outcome = envelope.payload.value("outcome", std::string("finished"));
                if (outcome != "succeeded") {
                    status = StatusCode::Error;
                }
                break;
            }
            case EventKind::ToolExecutionFailed: {
                outcome = "failed";
                status = StatusCode::Error;
                description = envelope.payload.value("error_code", std::string("tool_failed"));
                AttrStr(attributes, "tool.error_code", description);
                break;
            }
            case EventKind::ToolExecutionCancelled: {
                outcome = "cancelled";
                status = StatusCode::Error;
                description = "cancelled";
                AttrBool(attributes, "tool.cancelled", true);
                break;
            }
            default: {
                outcome = "unknown";
                status = StatusCode::Error;
                description = envelope.payload.value("reason", std::string("tool_unknown"));
                break;
            }
        }
        AttrStr(attributes, "tool.outcome", outcome);
        // §11.5:子代理终态 hash 只报在场,不上传原值。
        if (envelope.payload.contains("child_terminal_event_hash")) {
            AttrBool(attributes, "lubancode.subagent.child_terminal_hash_present", true);
        }
        Close(span, envelope, status, std::move(description), std::move(attributes));
        metrics_.Add("lubancode.tool.call_total",
                     nlohmann::json{{"tool_kind", tool_kind}, {"outcome", outcome}});
    }

    void OpenApproval(const EventEnvelope& envelope) {
        const std::string approval_id = envelope.payload.value("approval_id", std::string());
        if (approval_id.empty()) {
            Warn("approval_requested_without_id:" + envelope.event_id);
            return;
        }
        OpenSpan* span = NewSpan(envelope, "lubancode.approval.wait", "approval");
        span->span.parent_span_id = ParentOf(envelope.turn_id) != nullptr
                                        ? ParentOf(envelope.turn_id)->span.span_id
                                        : std::string();
        approvals_[approval_id] = span;
    }

    void CloseApproval(const EventEnvelope& envelope) {
        const std::string approval_id = envelope.payload.value("approval_id", std::string());
        auto it = approvals_.find(approval_id);
        OpenSpan* span = it != approvals_.end() ? it->second : nullptr;
        nlohmann::json attributes;
        const std::string decision = envelope.kind == EventKind::ControlApprovalResolved
                                         ? envelope.payload.value("decision", std::string("resolved"))
                                         : std::string("expired");
        AttrStr(attributes, "lubancode.approval.decision", decision);
        Close(span, envelope, StatusCode::Ok, std::string(), std::move(attributes));
        metrics_.Add("lubancode.approval.decision_total",
                     nlohmann::json{{"decision", decision}});
    }

    void OpenCompact(const EventEnvelope& envelope) {
        // compact 一族无 id(§14.4 一场至多一枚在途);旧未收口按 missing 收。
        CloseIfOpen(compact_span_, "compact");
        OpenSpan* span = NewSpan(envelope, "lubancode.compact", "compact");
        span->span.parent_span_id = ParentOf(envelope.turn_id) != nullptr
                                        ? ParentOf(envelope.turn_id)->span.span_id
                                        : std::string();
        AttrStr(span->span.attributes, "lubancode.compact.trigger",
                envelope.payload.value("trigger", std::string()));
        if (envelope.payload.contains("old_epoch")) {
            AttrInt(span->span.attributes, "lubancode.compact.epoch",
                    static_cast<std::int64_t>(envelope.payload.at("old_epoch").get<std::uint64_t>()));
        }
        compact_span_ = span;
    }

    void CloseCompact(const EventEnvelope& envelope) {
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome = "applied";
        std::string description;
        if (envelope.kind == EventKind::CompactFailed) {
            outcome = "failed";
            status = StatusCode::Error;
            description = envelope.payload.value("error_code", std::string("compact_failed"));
        } else if (envelope.kind == EventKind::CompactCancelled) {
            outcome = "cancelled";
            status = StatusCode::Error;
            description = "cancelled";
        } else if (envelope.kind == EventKind::CompactRejected) {
            outcome = "rejected";
            status = StatusCode::Error;
            description = "rejected";
        }
        Close(compact_span_, envelope, status, std::move(description), std::move(attributes));
        metrics_.Add("lubancode.compact.total", nlohmann::json{{"outcome", outcome}});
    }

    void OpenVerification(const EventEnvelope& envelope) {
        const std::string verification_id =
            envelope.payload.value("verification_id", std::string());
        if (verification_id.empty()) {
            Warn("verification_started_without_id:" + envelope.event_id);
            return;
        }
        OpenSpan* span = NewSpan(envelope, "lubancode.verification", "verification");
        span->span.parent_span_id = ParentOf(envelope.turn_id) != nullptr
                                        ? ParentOf(envelope.turn_id)->span.span_id
                                        : std::string();
        AttrStr(span->span.attributes, "lubancode.verification.kind",
                envelope.payload.value("kind", std::string()));
        verifications_[verification_id] = span;
    }

    void CloseVerification(const EventEnvelope& envelope) {
        const std::string verification_id =
            envelope.payload.value("verification_id", std::string());
        auto it = verifications_.find(verification_id);
        OpenSpan* span = it != verifications_.end() ? it->second : nullptr;
        nlohmann::json attributes;
        std::string outcome = "recorded";
        if (envelope.kind == EventKind::VerificationRecorded) {
            const bool passed = envelope.payload.value("passed", false);
            AttrBool(attributes, "lubancode.verification.passed", passed);
            outcome = passed ? "passed" : "failed";
        } else {
            outcome = "invalidated";
        }
        Close(span, envelope, StatusCode::Ok, std::string(), std::move(attributes));
        metrics_.Add("lubancode.verification.total", nlohmann::json{{"outcome", outcome}});
    }

    ProjectorOptions options_;
    ProjectionReport& report_;
    std::string trace_id_;
    std::deque<OpenSpan> spans_;  // deque:引用不因追加失效
    OpenSpan* run_span_ = nullptr;
    std::map<std::string, OpenSpan*> turns_;
    std::map<std::string, OpenSpan*> requests_;
    std::map<std::string, std::string> request_spans_;  // sent event id -> span id
    std::map<std::string, OpenSpan*> tools_;
    std::map<std::string, PreparedInfo> prepared_;
    std::map<std::string, ToolInfo> tools_info_;
    std::map<std::string, OpenSpan*> approvals_;
    OpenSpan* compact_span_ = nullptr;
    std::map<std::string, OpenSpan*> verifications_;
    MetricSink metrics_;
    std::vector<std::string> warnings_;
};

void MergeManifest(RedactionManifest& into, const RedactionManifest& from) {
    into.removed_fields += from.removed_fields;
    into.truncated_fields += from.truncated_fields;
}

}  // namespace

ProjectionReport ProjectJournalFile(const std::filesystem::path& stream_path,
                                    const ProjectorOptions& options) {
    ProjectionReport report;
    if (options.projection_key.empty()) {
        report.error_code = "telemetry.options_missing_key";
        report.message = "projection_key 为空:id 派生无钥即无确定性";
        return report;
    }
    std::error_code ec;
    if (!std::filesystem::exists(stream_path, ec)) {
        report.error_code = "telemetry.io_error";
        report.message = "Journal 文件不存在: " + stream_path.string();
        return report;
    }

    // 验账前置(§22.5):坏链/坏行停整条 stream,不跳过坏行接着猜。
    const trajectory::JournalVerifyReport verify = trajectory::VerifyJournalFile(stream_path);
    if (!verify.ok) {
        report.error_code = "telemetry.source_corrupt";
        report.message = "Journal 验账不过(" + verify.error_code + "): " + verify.message;
        return report;
    }
    const std::optional<std::vector<std::string>> lines =
        trajectory::ReadJournalLines(stream_path);
    if (!lines.has_value()) {
        report.error_code = "telemetry.io_error";
        report.message = "Journal 读不回: " + stream_path.string();
        return report;
    }

    StreamProjector projector(options, report);
    for (const std::string& line : *lines) {
        const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_discarded()) {
            report.error_code = "telemetry.source_corrupt";
            report.message = "Journal 行不是合法 JSON: " + line.substr(0, 64);
            return report;
        }
        std::string error_code;
        std::string message;
        const std::optional<EventEnvelope> envelope =
            EventEnvelope::FromJsonStrict(json, &error_code, &message);
        if (!envelope.has_value()) {
            report.error_code = "telemetry.source_corrupt";
            report.message = "信封解析拒绝(" + error_code + "): " + message;
            return report;
        }
        projector.Fold(*envelope);
    }
    projector.Finish();
    report.trace_id = projector.trace_id();
    report.spans = projector.TakeSpans();
    report.metrics = projector.metrics().Take();
    report.warnings = projector.TakeWarnings();

    // 二道门:每枚 span 属性与 resource 过 Redactor,manifest 合并进报告。
    report.resource_attributes = BuildResourceAttributes(options.resource);
    auto resource_redacted =
        RedactAttributes(report.resource_attributes, options.data_class, AttributeDomain::Resource);
    report.resource_attributes = std::move(resource_redacted.attributes);
    MergeManifest(report.redaction, resource_redacted.manifest);
    report.redaction.data_class = options.data_class;
    for (TraceSpan& span : report.spans) {
        auto redacted =
            RedactAttributes(span.attributes, options.data_class, AttributeDomain::Span);
        span.attributes = std::move(redacted.attributes);
        MergeManifest(report.redaction, redacted.manifest);
        if (auto violation = ValidateSpan(span)) {
            report.error_code = violation->code;
            report.message = violation->message + "(span " + span.name + ")";
            report.ok = false;
            return report;
        }
    }
    for (const MetricSample& metric : report.metrics) {
        if (auto violation = ValidateMetric(metric)) {
            report.error_code = violation->code;
            report.message = violation->message + "(metric " + metric.name + ")";
            report.ok = false;
            return report;
        }
    }
    report.ok = true;
    return report;
}

nlohmann::json ProjectionReport::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out.emplace("ok", ok);
    out.emplace("error_code", error_code);
    out.emplace("events_projected", events_projected);
    out.emplace("trace_id", trace_id);
    out.emplace("workspace_key", workspace_key);
    out.emplace("session_id", session_id);
    out.emplace("run_id", run_id);
    nlohmann::json span_array = nlohmann::json::array();
    for (const TraceSpan& span : spans) {
        span_array.push_back(span.ToJson());
    }
    out.emplace("spans", std::move(span_array));
    nlohmann::json metric_array = nlohmann::json::array();
    for (const MetricSample& metric : metrics) {
        metric_array.push_back(metric.ToJson());
    }
    out.emplace("metrics", std::move(metric_array));
    out.emplace("resource_attributes", resource_attributes);
    out.emplace("redaction", redaction.ToJson());
    if (!warnings.empty()) {
        out.emplace("warnings", warnings);
    }
    return out;
}

}  // namespace lubancode::telemetry
