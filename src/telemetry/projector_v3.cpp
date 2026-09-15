// v3 session 账的 Telemetry 投影(T07 / V3-GAP-02)。映射合同见
// projector.hpp 的 ProjectV3LedgerFile 注释;折叠规矩与 v2 半场同源:
// 不联网、不读墙钟、不随机发号——同一账、同一钥匙、同一 projector
// version,重放两次输出逐字节相同。
//
// 折叠算法:一遍顺序扫 timeline(seq 序),起行开 span、终行收 span;
// assistant usage owner 在 timeline 末端统一结算(model.response.completed
// 先于 assistant 落盘,结算时 owner 才齐)。确定性来源:
//   - id 由 identity 层 HMAC 派生(key + 锚行 id + 角色);
//   - 时间全取行内 timestamp(UTC ISO-8601,ParseV3TimestampMs);单调钟
//     v3 账没有,记 0 不猜;终行时间早于起行(时钟回拨形状)钳回起点
//     并记 warning,不让整条 stream 因合同校验停摆;
//   - 输出排序按 (锚行 seq, 锚行 id),与扫描序一致地稳定(v3 行 id 不
//     零填,字典序≠seq 序,不能用 v2 的 id 排序)。
#include "telemetry/projector.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "telemetry/identity.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/session_switch.hpp"

namespace lubancode::telemetry {
namespace {

using trajectory::v3::EventKindV3;
using trajectory::v3::EventLine;
using trajectory::v3::MessageLine;
using trajectory::v3::V3Ledger;

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

// 载荷字符串键(缺键/异型回空,不猜)。
std::string PayloadStr(const nlohmann::json& payload, const char* key) {
    if (payload.is_object() && payload.contains(key) && payload.at(key).is_string()) {
        return payload.at(key).get<std::string>();
    }
    return std::string();
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

// 行 timestamp → unix nano。认不动回 0(不猜;end>=start 合同由钳制兜住)。
std::int64_t LineUnixNano(const std::string& timestamp) {
    const auto ms = trajectory::v3::ParseV3TimestampMs(timestamp);
    return ms.has_value() ? *ms * 1000000 : 0;
}

// metric 累计器(与 v2 半场同规矩:同名同 labels 聚一张卡,输出前排序)。
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
            samples_.emplace(std::move(key), std::move(sample));
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

// assistant usage owner 的结算材料(schema §五:唯一可累计 owner)。
struct OwnerInfo {
    const MessageLine* message = nullptr;
};

struct PreparedInfo {
    std::string provider;
    std::string model;
};

struct ToolMeta {
    std::string tool_name;      // tool.execution.started 的 toolName/identity
    std::string source_kind;    // toolIdentity.registrationSource
    std::string declared_name;  // 声明块折叠出的逻辑名(FoldToolActions)
    std::string input_bucket;   // 声明参数 dump 尺寸入桶
};

// 折叠期的一枚在册 span。
struct OpenSpan {
    TraceSpan span;
    bool closed = false;
    std::uint64_t anchor_seq = 0;
};

class V3Fold {
public:
    V3Fold(const ProjectorOptions& options, ProjectionReport& report)
        : options_(options), report_(report) {}

    void Run(const V3Ledger& ledger) {
        trace_id_ = DeriveTraceId(options_.projection_key, ledger.session_id, ledger.run_id);
        report_.trace_id = trace_id_;
        report_.session_id = ledger.session_id;
        report_.run_id = ledger.run_id;
        report_.workspace_key = options_.resource.workspace_key;
        IndexToolDeclarations(ledger);
        for (const V3Ledger::Entry& entry : ledger.timeline) {
            if (entry.is_message) {
                FoldMessage(ledger.messages[entry.index]);
            } else {
                FoldEvent(ledger.events[entry.index]);
            }
            report_.events_projected += 1;
        }
        SettleRequests();
        Finish();
    }

    MetricSink& metrics() { return metrics_; }
    std::vector<TraceSpan> TakeSpans() {
        std::vector<OpenSpan> open = std::move(spans_);
        std::sort(open.begin(), open.end(), [](const OpenSpan& a, const OpenSpan& b) {
            if (a.anchor_seq != b.anchor_seq) {
                return a.anchor_seq < b.anchor_seq;
            }
            return a.span.source_event_id < b.span.source_event_id;
        });
        std::vector<TraceSpan> out;
        out.reserve(open.size());
        for (OpenSpan& span : open) {
            out.push_back(std::move(span.span));
        }
        return out;
    }
    std::vector<std::string> TakeWarnings() { return std::move(warnings_); }

private:
    // ---- 通用件 ----

    OpenSpan* NewSpan(const std::string& anchor_id, std::uint64_t anchor_seq,
                      const std::string& timestamp, const char* name, const char* role) {
        spans_.emplace_back();
        OpenSpan& open = spans_.back();
        open.span.trace_id = trace_id_;
        open.span.span_id = DeriveSpanId(options_.projection_key, anchor_id, role);
        open.span.name = name;
        open.anchor_seq = anchor_seq;
        const std::int64_t nano = LineUnixNano(timestamp);
        open.span.start_unix_nano = nano;
        open.span.end_unix_nano = nano;
        open.span.source_event_id = anchor_id;
        return &open;
    }

    // 收口。终行时间早于起行(时钟回拨)钳回起点并记 warning——不猜时长,
    // 也不让 span 合同校验(end>=start)把整条 stream 打停。
    void Close(OpenSpan* span, const std::string& terminal_id, const std::string& timestamp,
               StatusCode status, std::string description, nlohmann::json attributes,
               bool allow_reclose) {
        if (span == nullptr) {
            Warn("unmatched_terminal:" + terminal_id);
            return;
        }
        if (span->closed && !allow_reclose) {
            Warn("unmatched_terminal:" + terminal_id);
            return;
        }
        span->closed = true;
        const std::int64_t nano = LineUnixNano(timestamp);
        if (nano < span->span.start_unix_nano) {
            Warn("span_time_regression:" + span->span.name);
        } else {
            span->span.end_unix_nano = nano;
        }
        span->span.status = status;
        span->span.status_description = std::move(description);
        for (auto it = attributes.begin(); it != attributes.end(); ++it) {
            span->span.attributes[it.key()] = it.value();
        }
        span->span.source_terminal_event_id = terminal_id;
    }

    void CloseIfOpen(OpenSpan* span, const char* role) {
        if (span == nullptr || span->closed) {
            return;
        }
        span->closed = true;
        AttrStr(span->span.attributes, "lubancode.span.terminal", "missing");
        Warn(std::string("open_span_missing_terminal:") + role);
    }

    // 缺起点:span 锚在终行上、时长 0,terminal=missing_start 明标 partial,
    // 不拿更早的记录时间猜时长。
    OpenSpan* AnchorAtTerminal(const std::string& terminal_id, std::uint64_t seq,
                               const std::string& timestamp, const char* name, const char* role) {
        OpenSpan* span = NewSpan(terminal_id, seq, timestamp, name, role);
        AttrStr(span->span.attributes, "lubancode.span.terminal", "missing_start");
        Warn(std::string("span_missing_start:") + role + ":" + terminal_id);
        return span;
    }

    void Warn(std::string warning) { warnings_.push_back(std::move(warning)); }

    // turn 归属:每条带 turnId 的行首见即开 turn span(v3 无 turn 生命周期
    // 事件,起点=首条携带该 id 的行,终点一律 missing——partial,不猜)。
    OpenSpan* EnsureTurn(const std::optional<std::string>& turn_id,
                         const std::optional<std::string>& parent_turn_id,
                         const std::string& anchor_id, std::uint64_t seq,
                         const std::string& timestamp) {
        if (!turn_id.has_value()) {
            return nullptr;
        }
        auto found = turns_.find(*turn_id);
        if (found != turns_.end()) {
            return found->second;
        }
        OpenSpan* span = NewSpan(anchor_id, seq, timestamp, "lubancode.agent.turn", "turn");
        OpenSpan* parent = nullptr;
        if (parent_turn_id.has_value()) {
            auto parent_found = turns_.find(*parent_turn_id);
            if (parent_found != turns_.end()) {
                parent = parent_found->second;
            }
        }
        OpenSpan* root = parent != nullptr ? parent : session_span_;
        span->span.parent_span_id = root != nullptr ? root->span.span_id : std::string();
        turns_.emplace(*turn_id, span);
        metrics_.Add("lubancode.turn.started_total", nlohmann::json::object());
        return span;
    }

    OpenSpan* ParentOf(const std::optional<std::string>& turn_id) {
        if (turn_id.has_value()) {
            auto found = turns_.find(*turn_id);
            if (found != turns_.end()) {
                return found->second;
            }
        }
        return session_span_;
    }

    void AttachParent(OpenSpan* span, const std::optional<std::string>& turn_id) {
        OpenSpan* parent = ParentOf(turn_id);
        span->span.parent_span_id = parent != nullptr ? parent->span.span_id : std::string();
    }

    // ---- 声明侧材料:工具名与声明参数尺寸(FoldToolActions 纯读折叠)----

    void IndexToolDeclarations(const V3Ledger& ledger) {
        for (const trajectory::v3::ToolActionSnapshot& action :
             trajectory::v3::FoldToolActions(ledger)) {
            ToolMeta& meta = tool_meta_[action.tool_call_id];
            if (action.tool_name.has_value()) {
                meta.declared_name = *action.tool_name;
            }
            if (action.declared_args.has_value() && action.declared_args->is_object()) {
                meta.input_bucket = BytesBucket(action.declared_args->dump().size());
            }
        }
    }

    // ---- 消息 ----

    void FoldMessage(const MessageLine& message) {
        EnsureTurn(message.turn_id, message.parent_turn_id, message.message_id, message.seq,
                   message.timestamp);
        const std::string role =
            message.message.value("role", std::string());  // 拷贝隔离(value 直返引用)
        if (role != "assistant") {
            return;  // system/user/tool 不是 usage owner(§五)
        }
        const std::string request_id = message.request_id.value_or("");
        if (request_id.empty()) {
            return;  // 无请求归属的 assistant:不累计,不猜
        }
        if (owners_.contains(request_id)) {
            Warn("usage.v3_owner_duplicate: " + request_id);
            return;
        }
        owners_.emplace(request_id, OwnerInfo{&message});
    }

    // ---- 事件 ----

    void FoldEvent(const EventLine& event) {
        EnsureTurn(event.turn_id, event.parent_turn_id, event.event_id, event.seq,
                   event.timestamp);
        switch (event.kind) {
            case EventKindV3::SessionStarted: OpenSession(event); break;
            case EventKindV3::SessionEnded: CloseSession(event); break;
            case EventKindV3::ModelRequestPrepared: NotePrepared(event); break;
            case EventKindV3::ModelRequestSent: OpenRequest(event); break;
            case EventKindV3::ModelRequestFailed:
            case EventKindV3::ModelResponseCompleted:
            case EventKindV3::ModelResponseFailed:
            case EventKindV3::ModelResponseCancelled: CloseRequest(event); break;
            case EventKindV3::ModelUsageAppended: NoteAppended(event); break;
            case EventKindV3::ToolExecutionPending: NoteToolPending(event); break;
            case EventKindV3::ToolExecutionStarted: OpenTool(event); break;
            case EventKindV3::ToolExecutionFinished:
            case EventKindV3::ToolExecutionFailed:
            case EventKindV3::ToolExecutionCancelled:
            case EventKindV3::ToolExecutionRejected:
            case EventKindV3::ToolExecutionUnknown: CloseTool(event); break;
            case EventKindV3::CompactRequested: OpenCompact(event, true); break;
            case EventKindV3::CompactStarted: OpenCompact(event, false); break;
            case EventKindV3::CompactApplied:
            case EventKindV3::CompactFailed:
            case EventKindV3::CompactCancelled:
            case EventKindV3::CompactRejected: CloseCompact(event); break;
            case EventKindV3::HookDispatchRequested: OpenHook(event); break;
            case EventKindV3::HookSkipped: NoteHookSkipped(event); break;
            case EventKindV3::HookCompleted:
            case EventKindV3::HookFailed:
            case EventKindV3::HookCancelled:
            case EventKindV3::HookUnknown: CloseHook(event); break;
            default: break;  // 映射表外的事件不产 span(§映射合同),照常计数
        }
    }

    void OpenSession(const EventLine& event) {
        if (session_span_ != nullptr) {
            return;  // 一卷一枚 session 账;重复 session.started 不再造
        }
        OpenSpan* span = NewSpan(event.event_id, event.seq, event.timestamp, "lubancode.session",
                                 "session");
        AttrStr(span->span.attributes, "lubancode.run.kind", PayloadStr(event.payload, "runKind"));
        session_span_ = span;
        metrics_.Add("lubancode.session.started_total", nlohmann::json::object());
    }

    void CloseSession(const EventLine& event) {
        const std::string quality = PayloadStr(event.payload, "closeQuality");
        const StatusCode status = quality == "incomplete" ? StatusCode::Error : StatusCode::Ok;
        const std::string description = quality == "incomplete" ? std::string("incomplete")
                                                                : std::string();
        if (session_span_ == nullptr) {
            session_span_ = AnchorAtTerminal(event.event_id, event.seq, event.timestamp,
                                             "lubancode.session", "session");
        }
        Close(session_span_, event.event_id, event.timestamp, status, description,
              nlohmann::json::object(), /*allow_reclose=*/false);
    }

    void NotePrepared(const EventLine& event) {
        const std::string request_id = event.request_id.value_or("");
        if (request_id.empty()) {
            return;
        }
        PreparedInfo& info = prepared_[request_id];
        info.provider = PayloadStr(event.payload, "provider");
        info.model = PayloadStr(event.payload, "model");
    }

    void OpenRequest(const EventLine& event) {
        if (!event.request_id.has_value()) {
            Warn("request_sent_without_request_id:" + event.event_id);
            return;
        }
        const std::string& request_id = *event.request_id;
        // 同 request_id 旧 attempt 未收口(非常态):按 missing 收掉再开新的。
        auto existing = requests_.find(request_id);
        if (existing != requests_.end() && !existing->second->closed) {
            CloseIfOpen(existing->second, "request");
        }
        OpenSpan* span = NewSpan(event.event_id, event.seq, event.timestamp, "gen_ai.request",
                                 "gen_ai.request");
        AttachParent(span, event.turn_id);
        const auto prepared = prepared_.find(request_id);
        if (prepared != prepared_.end()) {
            AttrStr(span->span.attributes, "gen_ai.request.model", prepared->second.model);
            AttrStr(span->span.attributes, "gen_ai.request.provider", prepared->second.provider);
        }
        requests_[request_id] = span;
    }

    void CloseRequest(const EventLine& event) {
        if (!event.request_id.has_value()) {
            return;
        }
        const std::string& request_id = *event.request_id;
        auto found = requests_.find(request_id);
        OpenSpan* span = found != requests_.end() ? found->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome = "completed";
        std::string description;
        switch (event.kind) {
            case EventKindV3::ModelResponseCompleted: {
                AttrStr(attributes, "gen_ai.request.stop_reason",
                        PayloadStr(event.payload, "finishReason"));
                break;
            }
            case EventKindV3::ModelResponseFailed: {
                outcome = "failed";
                status = StatusCode::Error;
                description = PayloadStr(event.payload, "errorCode");
                if (description.empty()) {
                    description = "model_error";
                }
                AttrStr(attributes, "error.type", description);
                break;
            }
            case EventKindV3::ModelRequestFailed: {
                outcome = "failed";
                status = StatusCode::Error;
                description = "transport_failed";
                AttrStr(attributes, "error.type", description);
                break;
            }
            default: {
                outcome = "cancelled";
                status = StatusCode::Error;
                description = "cancelled";
                break;
            }
        }
        if (span == nullptr) {
            span = AnchorAtTerminal(event.event_id, event.seq, event.timestamp, "gen_ai.request",
                                    "gen_ai.request");
            requests_[request_id] = span;
        }
        Close(span, event.event_id, event.timestamp, status, std::move(description),
              std::move(attributes), /*allow_reclose=*/false);
        request_outcomes_[request_id] = outcome;
    }

    // model.usage.appended:迟到/更正/无消息请求的观察承载(§五 owner 表),
    // 不参与累计——单列警告(带数字,人工对账),不升级成 owner。
    void NoteAppended(const EventLine& event) {
        const std::string request_id = event.request_id.value_or(event.event_id);
        std::string note = "usage.v3_appended_observed: " + request_id;
        if (event.payload.contains("usage") && event.payload.at("usage").is_object()) {
            const nlohmann::json& usage = event.payload.at("usage");
            if (usage.contains("inputTokens") && usage.at("inputTokens").is_number_integer()) {
                note += " in=" + std::to_string(usage.at("inputTokens").get<std::int64_t>());
            }
            if (usage.contains("outputTokens") && usage.at("outputTokens").is_number_integer()) {
                note += " out=" + std::to_string(usage.at("outputTokens").get<std::int64_t>());
            }
        }
        note += "(观察,不入累计)";
        Warn(std::move(note));
    }

    void NoteToolPending(const EventLine& event) {
        const std::string action_id = event.action_id.value_or("");
        if (action_id.empty()) {
            return;
        }
        const std::string tool_name = PayloadStr(event.payload, "toolName");
        if (!tool_name.empty()) {
            tool_meta_[action_id].tool_name = tool_name;
        }
    }

    void OpenTool(const EventLine& event) {
        if (!event.action_id.has_value()) {
            Warn("tool_started_without_action_id:" + event.event_id);
            return;
        }
        const std::string& action_id = *event.action_id;
        // 同 actionId 重试 attempt:旧 attempt 未收口属非常态,先按 missing 收。
        auto existing = tools_.find(action_id);
        if (existing != tools_.end() && !existing->second->closed) {
            CloseIfOpen(existing->second, "tool");
        }
        ToolMeta meta = tool_meta_[action_id];  // 拷贝:meta 表后头还长
        if (event.payload.contains("toolIdentity") &&
            event.payload.at("toolIdentity").is_object()) {
            const nlohmann::json& identity = event.payload.at("toolIdentity");
            const std::string logical = PayloadStr(identity, "logicalName");
            if (!logical.empty()) {
                meta.tool_name = logical;
            }
            meta.source_kind = PayloadStr(identity, "registrationSource");
        }
        const std::string pending_name = PayloadStr(event.payload, "toolName");
        if (!pending_name.empty()) {
            meta.tool_name = pending_name;
        }
        tool_meta_[action_id] = meta;
        OpenSpan* span = NewSpan(event.event_id, event.seq, event.timestamp,
                                 "lubancode.tool.execute", "tool");
        AttachParent(span, event.turn_id);
        const std::string& name =
            !meta.tool_name.empty() ? meta.tool_name : meta.declared_name;
        AttrStr(span->span.attributes, "tool.name", name);
        AttrStr(span->span.attributes, "tool.kind", meta.source_kind);
        AttrStr(span->span.attributes, "tool.input_bytes_bucket", meta.input_bucket);
        AttrStr(span->span.attributes, "tool.batch_id", PayloadStr(event.payload, "batchId"));
        if (event.payload.contains("positionInBatch") &&
            event.payload.at("positionInBatch").is_number_unsigned()) {
            AttrInt(span->span.attributes, "tool.sequence_in_batch",
                    static_cast<std::int64_t>(
                        event.payload.at("positionInBatch").get<std::uint64_t>()));
        }
        tools_[action_id] = span;
    }

    void CloseTool(const EventLine& event) {
        if (!event.action_id.has_value()) {
            return;
        }
        const std::string& action_id = *event.action_id;
        auto found = tools_.find(action_id);
        OpenSpan* span = found != tools_.end() ? found->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome;
        std::string description;
        switch (event.kind) {
            case EventKindV3::ToolExecutionFinished: {
                outcome = "finished";
                break;
            }
            case EventKindV3::ToolExecutionFailed: {
                outcome = "failed";
                status = StatusCode::Error;
                description = PayloadStr(event.payload, "error_code");
                if (description.empty()) {
                    description = "tool_failed";
                }
                AttrStr(attributes, "tool.error_code", description);
                break;
            }
            case EventKindV3::ToolExecutionCancelled: {
                outcome = "cancelled";
                status = StatusCode::Error;
                description = "cancelled";
                AttrBool(attributes, "tool.cancelled", true);
                break;
            }
            case EventKindV3::ToolExecutionRejected: {
                outcome = "rejected";
                status = StatusCode::Error;
                description = "rejected";
                AttrStr(attributes, "tool.error_code",
                        PayloadStr(event.payload, "reason").empty()
                            ? std::string("rejected")
                            : PayloadStr(event.payload, "reason"));
                break;
            }
            default: {
                outcome = "unknown";
                status = StatusCode::Error;
                description = "tool_unknown";
                break;
            }
        }
        AttrStr(attributes, "tool.outcome", outcome);
        if (span == nullptr) {
            span = AnchorAtTerminal(event.event_id, event.seq, event.timestamp,
                                    "lubancode.tool.execute", "tool");
            tools_[action_id] = span;
        }
        Close(span, event.event_id, event.timestamp, status, std::move(description),
              std::move(attributes), /*allow_reclose=*/false);
        const auto meta = tool_meta_.find(action_id);
        const std::string tool_kind =
            meta != tool_meta_.end() && !meta->second.source_kind.empty()
                ? meta->second.source_kind
                : std::string("unknown");
        metrics_.Add("lubancode.tool.call_total",
                     nlohmann::json{{"tool_kind", tool_kind}, {"outcome", outcome}});
    }

    void OpenCompact(const EventLine& event, bool from_requested) {
        if (!event.compact_id.has_value()) {
            Warn("compact_without_compact_id:" + event.event_id);
            return;
        }
        const std::string& compact_id = *event.compact_id;
        auto existing = compacts_.find(compact_id);
        if (existing != compacts_.end()) {
            return;  // requested 已开;started 只是相位推进,不另开
        }
        OpenSpan* span = NewSpan(event.event_id, event.seq, event.timestamp, "lubancode.compact",
                                 "compact");
        AttachParent(span, event.turn_id);
        AttrStr(span->span.attributes, "lubancode.compact.trigger",
                PayloadStr(event.payload, "trigger"));
        if (!from_requested) {
            // requested 缺席、从 started 起锚:执行起点是真的,requested 相位
            // 没观察到——点名,不冒充完整生命周期。
            Warn("compact_requested_missing:" + compact_id);
        }
        compacts_[compact_id] = span;
    }

    void CloseCompact(const EventLine& event) {
        if (!event.compact_id.has_value()) {
            return;
        }
        const std::string& compact_id = *event.compact_id;
        auto found = compacts_.find(compact_id);
        OpenSpan* span = found != compacts_.end() ? found->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome = "applied";
        std::string description;
        if (event.kind == EventKindV3::CompactFailed) {
            outcome = "failed";
            status = StatusCode::Error;
            description = PayloadStr(event.payload, "errorCode");
            if (description.empty()) {
                description = "compact_failed";
            }
        } else if (event.kind == EventKindV3::CompactCancelled) {
            outcome = "cancelled";
            status = StatusCode::Error;
            description = "cancelled";
        } else if (event.kind == EventKindV3::CompactRejected) {
            outcome = "rejected";
            status = StatusCode::Error;
            description = "rejected";
        }
        if (span == nullptr) {
            span = AnchorAtTerminal(event.event_id, event.seq, event.timestamp,
                                    "lubancode.compact", "compact");
            compacts_[compact_id] = span;
        }
        Close(span, event.event_id, event.timestamp, status, std::move(description),
              std::move(attributes), /*allow_reclose=*/false);
        metrics_.Add("lubancode.compact.total", nlohmann::json{{"outcome", outcome}});
    }

    void OpenHook(const EventLine& event) {
        if (!event.hook_dispatch_id.has_value()) {
            Warn("hook_dispatch_without_id:" + event.event_id);
            return;
        }
        const std::string& dispatch_id = *event.hook_dispatch_id;
        auto existing = hooks_.find(dispatch_id);
        if (existing != hooks_.end() && !existing->second->closed) {
            return;  // 已开:requested 不重复
        }
        OpenSpan* span = NewSpan(event.event_id, event.seq, event.timestamp,
                                 "lubancode.hook.dispatch", "hook");
        AttachParent(span, event.turn_id);
        AttrStr(span->span.attributes, "lubancode.hook.point",
                PayloadStr(event.payload, "hookPoint"));
        if (event.payload.contains("matchedHandlers") &&
            event.payload.at("matchedHandlers").is_array()) {
            AttrInt(span->span.attributes, "lubancode.hook.matched_handlers",
                    static_cast<std::int64_t>(
                        event.payload.at("matchedHandlers").size()));
        }
        hooks_[dispatch_id] = span;
    }

    void NoteHookSkipped(const EventLine& event) {
        if (!event.hook_dispatch_id.has_value()) {
            return;
        }
        metrics_.Add("lubancode.hook.dispatch_total",
                     nlohmann::json{{"outcome", "skipped"}});
    }

    // invocation 终态收 dispatch:洋葱串行允许多枚 invocation,最后一枚
    // 终事件为 dispatch 终点(重收覆盖终态,输出取折叠终局)。
    void CloseHook(const EventLine& event) {
        if (!event.hook_dispatch_id.has_value()) {
            return;
        }
        const std::string& dispatch_id = *event.hook_dispatch_id;
        auto found = hooks_.find(dispatch_id);
        OpenSpan* span = found != hooks_.end() ? found->second : nullptr;
        nlohmann::json attributes;
        StatusCode status = StatusCode::Ok;
        std::string outcome = "completed";
        std::string description;
        if (event.kind == EventKindV3::HookFailed) {
            outcome = "failed";
            status = StatusCode::Error;
            description = PayloadStr(event.payload, "error_code");
            if (description.empty()) {
                description = "hook_failed";
            }
        } else if (event.kind == EventKindV3::HookCancelled) {
            outcome = "cancelled";
            status = StatusCode::Error;
            description = "cancelled";
        } else if (event.kind == EventKindV3::HookUnknown) {
            outcome = "unknown";
            status = StatusCode::Error;
            description = "unknown";
        }
        if (span == nullptr) {
            span = AnchorAtTerminal(event.event_id, event.seq, event.timestamp,
                                    "lubancode.hook.dispatch", "hook");
            hooks_[dispatch_id] = span;
        }
        Close(span, event.event_id, event.timestamp, status, std::move(description),
              std::move(attributes), /*allow_reclose=*/true);
        hook_outcomes_[dispatch_id] = outcome;
    }

    // ---- 结算:请求 metric 与 usage owner ----

    void SettleRequests() {
        for (const auto& [request_id, outcome] : request_outcomes_) {
            const auto owner = owners_.find(request_id);
            const auto prepared = prepared_.find(request_id);
            std::string provider = "unknown";
            std::string model;
            if (owner != owners_.end() && owner->second.message != nullptr) {
                const MessageLine& message = *owner->second.message;
                provider = message.provider.value_or(std::string());
                model = message.model.value_or(std::string());
            }
            if (provider.empty() && prepared != prepared_.end()) {
                provider = prepared->second.provider;
            }
            if (provider.empty()) {
                provider = "unknown";
            }
            if (model.empty() && prepared != prepared_.end()) {
                model = prepared->second.model;
            }
            const auto span_found = requests_.find(request_id);
            OpenSpan* span = span_found != requests_.end() ? span_found->second : nullptr;
            if (span != nullptr) {
                // 实际 provider/model 以 owner 为准(T06 同口径),prepared 只兜底。
                // 直接赋值:open 时按 prepared 写过同名键,emplace 不覆盖。
                if (!model.empty()) {
                    span->span.attributes["gen_ai.request.model"] = model;
                }
                if (!provider.empty() && provider != "unknown") {
                    span->span.attributes["gen_ai.request.provider"] = provider;
                }
                ApplyOwnerUsage(span, owner != owners_.end() ? owner->second.message : nullptr);
            }
            metrics_.Add("lubancode.model.request_total",
                         nlohmann::json{{"provider", provider}, {"outcome", outcome}});
        }
        // 有 owner 却无请求账的 assistant(sent 缺席的非常态):点名不累计。
        for (const auto& [request_id, owner] : owners_) {
            if (request_outcomes_.contains(request_id)) {
                continue;
            }
            (void)owner;
            Warn("usage.v3_owner_without_request: " + request_id);
        }
    }

    // usage 五项(schema §五键集;与 T06 UsageFromV3Owner 同键同折算)。
    // 键在场才报位(0 也是明报);owner 缺实报 coverage=unknown,不写 0。
    void ApplyOwnerUsage(OpenSpan* span, const MessageLine* owner) {
        const bool reported =
            owner != nullptr && owner->usage.has_value() && owner->usage->is_object();
        if (reported) {
            const nlohmann::json& usage = *owner->usage;
            static const char* kNames[] = {"inputTokens", "cacheReadTokens", "cacheWriteTokens",
                                           "outputTokens", "reasoningTokens"};
            static const char* kAttrs[] = {"gen_ai.usage.input_tokens",
                                           "gen_ai.usage.cache_read_tokens",
                                           "gen_ai.usage.cache_creation_tokens",
                                           "gen_ai.usage.output_tokens",
                                           "gen_ai.usage.reasoning_tokens"};
            static const char* kKinds[] = {"input", "cache_read", "cache_creation", "output",
                                           "reasoning"};
            for (int i = 0; i < 5; ++i) {
                if (!usage.contains(kNames[i]) || !usage.at(kNames[i]).is_number_integer()) {
                    continue;
                }
                const std::int64_t value = usage.at(kNames[i]).get<std::int64_t>();
                AttrInt(span->span.attributes, kAttrs[i], value);
                metrics_.AddTokens(kKinds[i], value);
            }
        }
        AttrStr(span->span.attributes, "gen_ai.usage.coverage",
                reported ? "provider" : "unknown");
    }

    // ---- 流末收口 ----

    void Finish() {
        CloseIfOpen(session_span_, "session");
        for (const auto& [turn_id, span] : turns_) {
            (void)turn_id;
            CloseIfOpen(span, "turn");
        }
        for (const auto& [request_id, span] : requests_) {
            (void)request_id;
            CloseIfOpen(span, "request");
        }
        for (const auto& [action_id, span] : tools_) {
            (void)action_id;
            CloseIfOpen(span, "tool");
        }
        for (const auto& [compact_id, span] : compacts_) {
            (void)compact_id;
            CloseIfOpen(span, "compact");
        }
        for (const auto& [dispatch_id, span] : hooks_) {
            (void)dispatch_id;
            CloseIfOpen(span, "hook");
        }
        // hook metric 按 dispatch 计一次(skipped 已在事件处计,不在此重复)。
        for (const auto& [dispatch_id, outcome] : hook_outcomes_) {
            (void)dispatch_id;
            metrics_.Add("lubancode.hook.dispatch_total", nlohmann::json{{"outcome", outcome}});
        }
    }

    ProjectorOptions options_;
    ProjectionReport& report_;
    std::string trace_id_;
    std::deque<OpenSpan> spans_;  // deque:引用不因追加失效
    OpenSpan* session_span_ = nullptr;
    std::map<std::string, OpenSpan*> turns_;
    std::map<std::string, OpenSpan*> requests_;
    std::map<std::string, std::string> request_outcomes_;
    std::map<std::string, OwnerInfo> owners_;
    std::map<std::string, PreparedInfo> prepared_;
    std::map<std::string, OpenSpan*> tools_;
    std::map<std::string, ToolMeta> tool_meta_;
    std::map<std::string, OpenSpan*> compacts_;
    std::map<std::string, OpenSpan*> hooks_;
    std::map<std::string, std::string> hook_outcomes_;
    MetricSink metrics_;
    std::vector<std::string> warnings_;
};

}  // namespace

ProjectionReport ProjectV3LedgerFile(const trajectory::v3::V3Ledger& ledger,
                                     const ProjectorOptions& options) {
    ProjectionReport report;
    if (options.projection_key.empty()) {
        report.error_code = "telemetry.options_missing_key";
        report.message = "projection_key 为空:id 派生无钥即无确定性";
        return report;
    }
    V3Fold fold(options, report);
    fold.Run(ledger);
    report.spans = fold.TakeSpans();
    report.metrics = fold.metrics().Take();
    report.warnings = fold.TakeWarnings();

    // 二道门(与 v2 半场同款):每枚 span 属性与 resource 过 Redactor,
    // manifest 合并进报告;再过合同校验。
    report.resource_attributes = BuildResourceAttributes(options.resource);
    auto resource_redacted =
        RedactAttributes(report.resource_attributes, options.data_class, AttributeDomain::Resource);
    report.resource_attributes = std::move(resource_redacted.attributes);
    report.redaction.removed_fields += resource_redacted.manifest.removed_fields;
    report.redaction.truncated_fields += resource_redacted.manifest.truncated_fields;
    report.redaction.data_class = options.data_class;
    for (TraceSpan& span : report.spans) {
        auto redacted =
            RedactAttributes(span.attributes, options.data_class, AttributeDomain::Span);
        span.attributes = std::move(redacted.attributes);
        report.redaction.removed_fields += redacted.manifest.removed_fields;
        report.redaction.truncated_fields += redacted.manifest.truncated_fields;
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

}  // namespace lubancode::telemetry
