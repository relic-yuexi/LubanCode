#include "channel/qq/qq_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>
#include <utility>

#include "channel/qq/qq_proto.hpp"
#include "channel/transport/ws_client.hpp"

namespace lubancode::channel::qq {

namespace {

// configuration.md §10 的退避阶梯(秒)。
constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 16, 30, 60};
constexpr int kBackoffSteps = static_cast<int>(sizeof(kBackoffSeconds) / sizeof(int));

// 组装带阶段/稳定码的连接事件(StageChanged/ConnectFailed/Disconnected 共用)。
GatewayEvent GatewayConnectEvent(GatewayEvent::Kind kind, const std::string& stage,
                                 const std::string& code, const std::string& detail) {
    GatewayEvent event;
    event.kind = kind;
    event.stage = stage;
    event.error_code = code;
    event.detail = detail;
    return event;
}

// 运行期读错误的稳定码(read: 服务端断流/超时/协议错)。
std::string ReadErrorCode(const transport::WsError& error) {
    switch (error.kind) {
        case transport::WsError::Kind::Timeout:
            return "read_timeout";
        case transport::WsError::Kind::Closed:
            return "read_closed";
        case transport::WsError::Kind::Protocol:
            return "read_protocol";
        case transport::WsError::Kind::Failed:
            break;
    }
    return "read_failed";
}

}  // namespace

GatewayHttpFailureClass ClassifyGatewayHttpFailure(
    int status, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& diagnostic_headers) {
    GatewayHttpFailureClass out;
    // 有界 JSON 读平台 code/err_code 与 trace_id(§四;A03 补官方 err_code
    // 形状——API 调用指南的 100017 类未知码,受控诊断须两码并记)。code
    // 宽松收数字/数字串;message 不透传——平台错误文案可能回显请求参数/
    // 敏感值。
    const auto parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    const bool is_json = !parsed.is_discarded() && parsed.is_object();
    std::string platform_code;
    std::string platform_err_code;
    std::string trace_id;
    if (is_json) {
        if (parsed.contains("code")) {
            const auto& code = parsed.at("code");
            if (code.is_number_integer()) {
                platform_code = std::to_string(code.get<std::int64_t>());
            } else if (code.is_string()) {
                platform_code = code.get<std::string>();
            }
        }
        if (parsed.contains("err_code")) {
            const auto& err_code = parsed.at("err_code");
            if (err_code.is_number_integer()) {
                platform_err_code = std::to_string(err_code.get<std::int64_t>());
            } else if (err_code.is_string()) {
                platform_err_code = err_code.get<std::string>();
            }
        }
        if (parsed.contains("trace_id") && parsed.at("trace_id").is_string()) {
            trace_id = parsed.at("trace_id").get<std::string>();
        }
    }
    // Retry-After:只认纯数字秒(HTTP-date 不解析)。头名大小写不敏感
    //(HTTP 头名本就不分大小写;生产链路 MakeHttpFunc 投影时已小写化,
    // 这里自身再归一,直调/假件给原始大小写也认得)。
    for (const auto& [name, value] : diagnostic_headers) {
        std::string lower_name = name;
        for (char& c : lower_name) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (lower_name == "retry-after" && out.retry_after_ms == 0) {
            if (!value.empty() &&
                value.find_first_not_of("0123456789") == std::string::npos) {
                const long long seconds = std::strtoll(value.c_str(), nullptr, 10);
                if (seconds > 0) {
                    out.retry_after_ms = seconds * 1000;
                }
            }
        } else if (trace_id.empty() && lower_name.find("trace") != std::string::npos &&
                   !value.empty()) {
            trace_id = value;  // body 没给 trace_id 时用白名单头兜底
        }
    }
    if (trace_id.size() > 128) {
        trace_id = trace_id.substr(0, 128) + "...";
    }
    const std::string trace_note =
        trace_id.empty() ? std::string() : " trace=" + trace_id;
    const std::string code_note =
        platform_code.empty() ? std::string() : " 平台code=" + platform_code;
    const std::string err_code_note =
        platform_err_code.empty() ? std::string() : " err_code=" + platform_err_code;
    // 受控诊断拼料(A03/100017 待查案):端点类别(稳定码前缀 gateway_url)、
    // HTTP、两业务码、白名单 trace、Retry-After、body 是否 JSON——零令牌
    // 零密钥零正文,全在这条 detail 里。
    const auto notes = [&]() { return code_note + err_code_note + trace_note; };

    if (status == 401) {
        out.code = "gateway_url_unauthorized";
        out.detail = "HTTP 401:鉴权失效(token 无效或过期)" + notes();
        return out;
    }
    if (status == 403) {
        out.code = "gateway_url_forbidden";
        out.detail = "HTTP 403:权限/配置拒绝" + notes();
        return out;
    }
    if (status == 429) {
        out.code = "gateway_url_rate_limited";
        out.detail = "HTTP 429:限流" + notes() +
                     (out.retry_after_ms > 0
                          ? " retry_after=" + std::to_string(out.retry_after_ms / 1000) + "s"
                          : std::string(" (无 Retry-After,走本地阶梯)"));
        return out;
    }
    if (status >= 500) {
        out.code = "gateway_url_server_error";
        out.detail = "HTTP " + std::to_string(status) + ":服务故障" + notes();
        return out;
    }
    if (status == 400) {
        if (is_json && (!platform_code.empty() || !platform_err_code.empty())) {
            out.code = "gateway_url_bad_request";
            out.detail = "HTTP 400:请求被平台拒绝" + notes() +
                         "(未知平台 code,低频重试;不重置密钥)";
            return out;
        }
        out.code = is_json ? "gateway_url_bad_request" : "gateway_url_bad_response";
        out.detail = is_json ? "HTTP 400:JSON 无平台 code" + trace_note
                             : "HTTP 400:非 JSON 响应" + trace_note;
        return out;
    }
    // 其余 4xx:不猜原因,只报事实;不把所有 4xx 当永久失败(§四)。
    out.code = "gateway_url_http_failed";
    out.detail = "HTTP " + std::to_string(status) + (is_json ? ":JSON" : ":非 JSON") +
                 notes();
    return out;
}

QqGatewaySession::~QqGatewaySession() {
    CancelInFlight();
}

std::string QqGatewaySession::state_name() const {
    switch (state_.load()) {
        case State::Idle:
            return "idle";
        case State::Connecting:
            return "connecting";
        case State::Authenticating:
            return "authenticating";
        case State::Resuming:
            return "resuming";
        case State::Running:
            return "running";
        case State::Backoff:
            return "backoff";
        case State::Stopped:
            return "stopped";
    }
    return "unknown";
}

std::string QqGatewaySession::session_id() const {
    // A08 线程约束:网关线程写/宿主线程读,std::string 过锁(无原子性)。
    const std::lock_guard<std::mutex> lock(session_state_mutex_);
    return session_id_;
}

void QqGatewaySession::CancelInFlight() {
    // A08 共享所有权:锁内拷 shared_ptr,锁外调 Cancel——连接线程清账或
    // 析构 unique_ptr 都不会让这里的指针悬空(Cancel 期间对象保活)。
    std::shared_ptr<IGatewayTransport> transport;
    {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        transport = in_flight_;
    }
    if (transport != nullptr) {
        transport->Cancel();
    }
}

GatewayEventAck QqGatewaySession::EmitEvent(const GatewayEvent& event) {
    if (!options_.on_event) {
        return GatewayEventAck::Persisted;  // 观测型装配:无宿主即无落盘语义
    }
    return options_.on_event(event);
}

bool QqGatewaySession::AcceptDispatch(const GatewayPayload& payload) {
    // 已收到:先记 last_seq(心跳合同口径——不管宿主落盘与否,这条确实
    // 到了客户端)。durable 游标只在宿主确认接住后推进。
    if (payload.s >= 0) {
        last_seq_.store(payload.s);
    }
    if (payload.s < 0) {
        return true;  // 无序号的 Dispatch(协议外形状):不推进游标,照常消化
    }
    // 按序推进(乱序/重复推送不回退游标:平台补发的旧序号幂等消化)。
    const auto advance_durable = [this](std::int64_t s) {
        const std::int64_t durable = durable_seq_.load();
        if (s > durable) {
            durable_seq_.store(s);
        }
    };
    if (payload.t == "READY" || payload.t == "RESUMED") {
        // 会话帧的落账归网关自身(鉴权窗内同步完成,无宿主落盘语义)——
        // 网关已安全消费,游标照常推进(重复出现同样幂等)。
        advance_durable(payload.s);
        return true;
    }
    GatewayEvent event;
    if (payload.t == "C2C_MESSAGE_CREATE") {
        event.kind = GatewayEvent::Kind::C2cMessageCreate;
        event.c2c_d = payload.d;
    } else if (payload.t == "INTERACTION_CREATE") {
        event.kind = GatewayEvent::Kind::InteractionCreate;
        event.interaction_d = payload.d;
    } else {
        // 未建模的兄弟事件(FRIEND_ADD/C2C_MSG_RECEIVE…):照实转交宿主留
        // 明确终结记录(计数/留痕),不静默吞(A04)。
        event.kind = GatewayEvent::Kind::UnsupportedDispatch;
        event.detail = payload.t;
    }
    event.seq = payload.s;
    if (EmitEvent(event) != GatewayEventAck::Persisted) {
        return false;  // 没接住:不推进 durable 游标,调用方走可恢复故障
    }
    advance_durable(payload.s);
    return true;
}

void QqGatewaySession::SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms) {
    const std::int64_t step = 100;
    for (std::int64_t slept = 0; slept < ms && !stop->load(); slept += step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long long>(std::min<std::int64_t>(step, ms - slept))));
    }
}

void QqGatewaySession::RunLoop(std::atomic<bool>* stop) {
    int attempt = 0;
    while (!stop->load()) {
        bool session_invalidated = false;
        state_.store(State::Connecting);
        const RunOutcome outcome =
            RunOneConnection(stop, &session_invalidated, /*attempt_number=*/attempt + 1);
        if (session_invalidated) {
            {
                const std::lock_guard<std::mutex> lock(session_state_mutex_);
                session_id_.clear();  // op9 不可恢复:下一轮重新 Identify
            }
            GatewayEvent event;
            event.kind = GatewayEvent::Kind::SessionInvalidated;
            event.detail = "invalid session";
            event.seq = last_seq_.load();
            (void)EmitEvent(event);
        }
        if (stop->load()) {
            break;
        }
        // 连接稳定过(收到过 ACK)则退避归零;否则沿阶梯走。
        if (outcome.stable) {
            attempt = 0;
        } else {
            attempt = std::min(attempt + 1, kBackoffSteps);
        }
        connect_attempts_.store(attempt);
        const int base_ms = kBackoffSeconds[attempt == 0 ? 0 : attempt - 1] * 1000;
        std::mt19937 rng(static_cast<unsigned>(options_.now_ms() & 0xFFFFFFFF));
        std::uniform_int_distribution<int> jitter(0, base_ms / 10);  // 10% jitter
        int backoff_ms = static_cast<int>((base_ms + jitter(rng)) * options_.backoff_scale);
        // §四:429 服从有效 Retry-After(服务端建议高于阶梯时取建议),但封
        // max_backoff_ms 上限——退避不被服务端钉死。
        if (outcome.retry_after_ms > backoff_ms) {
            backoff_ms = static_cast<int>(
                std::min<std::int64_t>(outcome.retry_after_ms, options_.max_backoff_ms));
        }
        state_.store(State::Backoff);
        // 退避排程单独成事件:只带 attempt 与下次尝试时刻,不带"原因"——
        // 根因归 ConnectFailed/Disconnected,退避不许覆盖它(§三);attempt
        // 与 ConnectFailed 的尝试编号同轮(§四)。
        GatewayEvent backoff;
        backoff.kind = GatewayEvent::Kind::BackoffScheduled;
        backoff.attempt = attempt;
        backoff.next_retry_at_ms = options_.now_ms() + backoff_ms;
        (void)EmitEvent(backoff);
        SleepInterruptible(stop, backoff_ms);
    }
    state_.store(State::Stopped);
    (void)EmitEvent(GatewayEvent{GatewayEvent::Kind::Stopped});
}

QqGatewaySession::RunOutcome QqGatewaySession::RunOneConnection(
    std::atomic<bool>* stop, bool* session_was_invalidated, int attempt_number) {
    // 工厂给 unique_ptr,这里即折 shared_ptr:本轮连接与取消方共享所有权
    //(A08)——CancelInFlight 拷走的引用保活对象,清账/析构不悬空。
    std::shared_ptr<IGatewayTransport> transport = options_.transport_factory();
    if (!transport) {
        GatewayEvent event = GatewayConnectEvent(GatewayEvent::Kind::ConnectFailed,
                                                 kStageConnecting, "no_transport",
                                                 "no transport factory");
        event.attempt = attempt_number;
        (void)EmitEvent(event);
        return RunOutcome{};
    }
    // guard 兜底所有 return 路径的清账。
    {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_ = transport;
    }
    const auto clear_in_flight = [this]() {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_.reset();
    };
    struct InFlightGuard {
        QqGatewaySession* session;
        ~InFlightGuard() {
            const std::lock_guard<std::mutex> lock(session->in_flight_mutex_);
            session->in_flight_.reset();
        }
    } in_flight_guard{this};
    bool reached_ready = false;  // 区分 ConnectFailed(没到 READY)与 Disconnected

    // 一轮失败的收口:发 ConnectFailed(未到 READY)或 Disconnected(在线
    // 过才断)——两者都带阶段/稳定码/根因 detail 与尝试编号(§三/§四:
    // Disconnected 不丢 detail,根因与退避通知关联同次尝试)。
    const auto fail = [&](const std::string& stage, const std::string& code,
                          const std::string& reason) {
        clear_in_flight();
        transport->Close(1000, "session end");
        GatewayEvent event = GatewayConnectEvent(reached_ready
                                                      ? GatewayEvent::Kind::Disconnected
                                                      : GatewayEvent::Kind::ConnectFailed,
                                                  stage, code, reason);
        event.attempt = attempt_number;
        (void)EmitEvent(event);
        return RunOutcome{};
    };
    const auto fail_error = [&](const GatewayConnectError& error) {
        const RunOutcome outcome = fail(error.stage, error.error_code, error.detail);
        return RunOutcome{outcome.stable, error.retry_after_ms};
    };

    // 取令牌/查地址在 provider 里(适配器已发 fetching_token/
    // fetching_gateway_url 阶段事件);失败账由 provider 带上来。
    const auto url = options_.gateway_url_provider();
    if (!url.has_value()) {
        return fail_error(url.error());
    }
    (void)EmitEvent(GatewayConnectEvent(GatewayEvent::Kind::StageChanged,
                                        kStageConnecting, std::string(), std::string()));
    const auto connected = transport->Connect(*url);
    if (!connected.has_value()) {
        return fail_error(connected.error());
    }

    // 1) Hello(op=10,带心跳间隔)——WS 已升级,首条即 Hello。
    std::int64_t heartbeat_interval_ms = 0;
    {
        const auto message = transport->ReadMessage(options_.hello_timeout_ms);
        if (!message.has_value()) {
            return fail(kStageConnecting,
                        message.error().kind == transport::WsError::Kind::Timeout ? "hello_timeout"
                                                                        : "hello_failed",
                        "waiting hello: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return fail(kStageConnecting, "hello_bad_payload", "hello payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value() || payload->op != GatewayOp::Hello) {
            return fail(kStageConnecting, "hello_bad_payload", "first message is not HELLO");
        }
        const auto interval = ParseHelloInterval(payload->d, &parse_error);
        if (!interval.has_value()) {
            // A01:官方字段 heartbeat_interval;缺字段/类型错/非正数/超范围
            // 各有明细,连接层按协议错误断线,不带默认间隔硬跑。
            return fail(kStageConnecting, "hello_bad_payload",
                        "HELLO: " + parse_error);
        }
        heartbeat_interval_ms = *interval;
    }

    // 2) Identify / Resume。Resume 是独立阶段"恢复中"(A02):不提前报
    //    connected。Resume 的 seq 走 durable 游标(A04:已安全接收的连续
    //    序号)——宿主没接住的事件由平台补发,不靠 last_seq 假称已收。
    std::string resume_session_id;
    {
        const std::lock_guard<std::mutex> lock(session_state_mutex_);
        resume_session_id = session_id_;
    }
    const bool resuming_session = !resume_session_id.empty();
    const char* auth_stage = resuming_session ? kStageResuming : kStageIdentifying;
    state_.store(resuming_session ? State::Resuming : State::Authenticating);
    (void)EmitEvent(GatewayConnectEvent(GatewayEvent::Kind::StageChanged,
                                        auth_stage, std::string(), std::string()));
    const auto token = options_.token_provider();
    if (!token.has_value()) {
        return fail_error(token.error());
    }
    if (!resuming_session) {
        const auto sent = transport->SendText(BuildIdentify(*token, options_.intents).dump());
        if (!sent.has_value()) {
            return fail(kStageIdentifying, "identify_send_failed",
                        "send identify: " + sent.error());
        }
    } else {
        const auto sent = transport->SendText(
            BuildResume(*token, resume_session_id, durable_seq_.load()).dump());
        if (!sent.has_value()) {
            return fail(kStageResuming, "resume_send_failed",
                        "send resume: " + sent.error());
        }
    }

    // 心跳与 ACK 统一账(A09):鉴权窗与运行期共用同一套节拍/计数——服务端
    // 心跳(官方 opcode 表 op1 双向)立即应答一次,应答计入 missed_acks,ACK
    // 到达清零;多发与误判都不许。
    int missed_acks = 0;
    bool ever_acked = false;
    std::int64_t next_beat = options_.now_ms() + heartbeat_interval_ms;
    const auto beat = [&](const char* stage) -> std::optional<RunOutcome> {
        const auto sent = transport->SendText(
            BuildHeartbeat(last_seq_.load() >= 0
                               ? std::optional<std::int64_t>(last_seq_.load())
                               : std::nullopt)
                .dump());
        if (!sent.has_value()) {
            return fail(stage, "heartbeat_send_failed", "send heartbeat: " + sent.error());
        }
        ++missed_acks;
        if (missed_acks > options_.missed_ack_limit) {
            return fail(stage, "heartbeat_ack_missed",
                        "heartbeat ack missed " + std::to_string(missed_acks) + " times");
        }
        next_beat = options_.now_ms() + heartbeat_interval_ms;
        return std::nullopt;
    };
    // 业务事件派发:鉴权窗补发流与运行期同一条路,统一走 AcceptDispatch
    //（A04 回执制——last_seq 先记、宿主确认接住才推 durable 游标;未建模
    // 的兄弟事件转宿主留终结记录;PersistFailed 按可恢复故障断线,见下
    // 方 switch 的 Dispatch 分支）。
    // Invalid Session 的三分处置(A09):d=true 会话仍可信,断线重连走
    // Resume;d=false 清 session 重新 Identify;d 缺失/非 bool——官方合同
    // 里不存在,会话可信度不可判定,按不可恢复处置(清 session 重新
    // Identify;保守换新会话,不赌 Resume 死循环)。与 WS close code 的
    // 区分:close 走读错误分支(read_closed 稳定码),不经这里。
    const auto handle_invalid_session = [&](const nlohmann::json& payload_json) {
        const auto resumable = ParseInvalidSessionResumable(payload_json);
        if (!resumable.has_value() || !*resumable) {
            *session_was_invalidated = true;
        }
    };

    // 3) 鉴权循环(A02):总期限 ready_timeout_ms。Identify 只认有效 READY;
    //    Resume 接收补发业务事件、等 RESUMED 才算恢复完成——单条补发不冒
    //    充上线,期限到仍未完成即断线退避。期间应答控制帧、维持心跳。
    bool auth_complete = false;
    bool resumed = false;
    const std::int64_t auth_deadline_ms = options_.now_ms() + options_.ready_timeout_ms;
    while (!stop->load()) {
        const std::int64_t now = options_.now_ms();
        if (now >= auth_deadline_ms) {
            return fail(auth_stage, "ready_timeout",
                        std::string(resuming_session ? "no RESUMED within auth window (session_id="
                                                     : "no READY within auth window (session_id=") +
                            (resuming_session ? "kept" : "n/a") + ")");
        }
        std::int64_t remaining_beat = next_beat - now;
        if (remaining_beat <= 0) {
            if (const auto failed = beat(auth_stage)) {
                return *failed;
            }
            remaining_beat = heartbeat_interval_ms;
        }
        const std::int64_t read_budget =
            std::min<std::int64_t>(auth_deadline_ms - now, std::max<std::int64_t>(remaining_beat, 1));
        const auto message = transport->ReadMessage(static_cast<int>(read_budget));
        if (!message.has_value()) {
            if (message.error().kind == transport::WsError::Kind::Timeout) {
                continue;  // 读窗到点:回循环顶(心跳/期限判定)
            }
            return fail(auth_stage, ReadErrorCode(message.error()),
                        "auth read: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return fail(auth_stage, "ready_bad_payload", "auth payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value()) {
            return fail(auth_stage, "ready_bad_payload", "auth payload: " + parse_error);
        }
        if (!payload->op.has_value()) {
            // 未知 op(如 op12/13 的 HTTP 回调族误入 WS):记账不崩,不当
            // 心跳(A09)。
            unexpected_ops_.fetch_add(1);
            continue;
        }
        switch (*payload->op) {
            case GatewayOp::Dispatch: {
                if (payload->t == "READY") {
                    if (resuming_session) {
                        // Resume 路径不认 READY(合同外),继续等 RESUMED;
                        // 游标照记(网关自身已安全消费)。
                        (void)AcceptDispatch(*payload);
                        break;
                    }
                    const auto ready = ParseReady(payload->d, &parse_error);
                    if (!ready.has_value()) {
                        return fail(kStageIdentifying, "ready_bad_payload",
                                    "READY: " + parse_error);
                    }
                    {  // 只在 READY 分支换会话(A02);写锁(A08 线程约束)
                        const std::lock_guard<std::mutex> lock(session_state_mutex_);
                        session_id_ = ready->session_id;
                    }
                    GatewayEvent event;
                    event.kind = GatewayEvent::Kind::SessionReady;
                    event.detail = ready->user_id;
                    event.seq = payload->s;
                    (void)EmitEvent(event);
                    (void)AcceptDispatch(*payload);  // 会话帧:记账 + 游标推进
                    auth_complete = true;
                    break;
                }
                if (payload->t == "RESUMED") {
                    if (!resuming_session) {
                        // Identify 路径不认 RESUMED(合同外),继续等 READY;
                        // 游标照记。
                        (void)AcceptDispatch(*payload);
                        break;
                    }
                    GatewayEvent event;
                    event.kind = GatewayEvent::Kind::SessionResumed;
                    event.detail = resume_session_id;
                    event.seq = payload->s;
                    (void)EmitEvent(event);
                    (void)AcceptDispatch(*payload);  // 会话帧:记账 + 游标推进
                    resumed = true;
                    auth_complete = true;
                    break;
                }
                // 补发/早到业务事件(官方 Resume 合同:先补发遗漏事件,补完才
                // 下发 RESUMED):走同一只 AcceptDispatch——按序记账、宿主确认、
                // 游标推进;PersistFailed 按可恢复故障断线,Resume 从 durable
                // 游标补发窗含这条,不丢信(A04)。
                if (!AcceptDispatch(*payload)) {
                    return fail(auth_stage, "event_persist_failed",
                                "host did not persist dispatch seq " +
                                    std::to_string(payload->s) + " (" + payload->t + ")");
                }
                break;
            }
            case GatewayOp::HeartbeatAck:
                missed_acks = 0;
                ever_acked = true;
                break;
            case GatewayOp::Heartbeat:
                // 服务端心跳(官方双向):立即应答,并入同一 ACK 账。
                if (const auto failed = beat(auth_stage)) {
                    return *failed;
                }
                break;
            case GatewayOp::Reconnect:
                return fail(auth_stage, "server_reconnect_requested",
                            "server requested reconnect");
            case GatewayOp::InvalidSession:
                handle_invalid_session(payload_json);
                return fail(auth_stage, "invalid_session", "invalid session");
            case GatewayOp::Hello:
            case GatewayOp::Identify:
            case GatewayOp::Resume:
                // 服务端不该发:记账不崩(宽容读),不当心跳。
                unexpected_ops_.fetch_add(1);
                break;
        }
        if (auth_complete) {
            break;
        }
    }
    if (!auth_complete) {
        // stop 置位且未完成鉴权:不报 connected,干净收场(A02)。
        clear_in_flight();
        transport->Close(1000, "stop");
        return RunOutcome{ever_acked, /*retry_after_ms=*/0};
    }
    if (resumed) {
        ever_acked = true;  // RESUMED 本身证明服务端认了会话
    }

    // READY/RESUMED 过:connected 成立(§三)。阶段推进放这里,SessionReady/
    // SessionResumed 事件在前——适配器先把 connected 记上,阶段再跟着变。
    reached_ready = true;
    (void)EmitEvent(GatewayConnectEvent(GatewayEvent::Kind::StageChanged,
                                        kStageConnected, std::string(), std::string()));

    // 4) 运行循环:读分发 + 心跳 + ACK 监视(与鉴权窗同一套心跳账)。心跳
    //    节拍锚定绝对时刻(next_beat),事件密集也不重置心跳窗——重置会饿死
    //    心跳,服务端掐线。
    state_.store(State::Running);
    while (!stop->load()) {
        const std::int64_t now = options_.now_ms();
        std::int64_t remaining = next_beat - now;
        if (remaining <= 0) {
            if (const auto failed = beat(kStageConnected)) {
                return *failed;
            }
            remaining = heartbeat_interval_ms;
        }
        const auto message =
            transport->ReadMessage(static_cast<int>(std::max<std::int64_t>(remaining, 1)));
        if (!message.has_value()) {
            if (message.error().kind == transport::WsError::Kind::Timeout) {
                continue;  // 到点,回循环顶发心跳
            }
            return fail(kStageConnected, ReadErrorCode(message.error()),
                        "read: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return fail(kStageConnected, "dispatch_bad_payload", "dispatch payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value()) {
            return fail(kStageConnected, "dispatch_bad_payload",
                        "dispatch payload: " + parse_error);
        }
        if (!payload->op.has_value()) {
            // 未知 op:独立处置——记账不崩,不冒充心跳、不触发应答(A09:
            // 旧 switch 的 value_or(Heartbeat) 会把陌生 op 当心跳放行)。
            unexpected_ops_.fetch_add(1);
            continue;
        }
        switch (*payload->op) {
            case GatewayOp::Dispatch:
                // A04 统一落账口:READY/RESUMED(鉴权窗已处理,重复出现按序
                // 记账)、业务事件、未建模兄弟事件全走 AcceptDispatch——
                // last_seq 先记(已收到),宿主确认接住才推进 durable 游标;
                // PersistFailed 按可恢复故障断线,Resume 从 durable 游标
                // 补发,不跨过失败事件。
                if (!AcceptDispatch(*payload)) {
                    return fail(kStageConnected, "event_persist_failed",
                                "host did not persist dispatch seq " +
                                    std::to_string(payload->s) + " (" + payload->t + ")");
                }
                break;
            case GatewayOp::HeartbeatAck:
                missed_acks = 0;
                ever_acked = true;
                break;
            case GatewayOp::Heartbeat:
                // 服务端心跳(官方 opcode 表双向):立即应答,并入同一 ACK 账
                //(应答后重锚节拍,不与定时心跳叠发)。
                if (const auto failed = beat(kStageConnected)) {
                    return *failed;
                }
                break;
            case GatewayOp::Reconnect:
                return fail(kStageConnected, "server_reconnect_requested",
                            "server requested reconnect");
            case GatewayOp::InvalidSession:
                handle_invalid_session(payload_json);
                return fail(kStageConnected, "invalid_session", "invalid session");
            case GatewayOp::Hello:
            case GatewayOp::Identify:
            case GatewayOp::Resume:
                // 服务端不该发:记账不崩(宽容读),不当心跳。
                unexpected_ops_.fetch_add(1);
                break;
        }
    }
    // stop 置位:干净收场(也算"稳定结束",不涨退避)。
    clear_in_flight();
    transport->Close(1000, "stop");
    return RunOutcome{ever_acked, /*retry_after_ms=*/0};
}

}  // namespace lubancode::channel::qq
