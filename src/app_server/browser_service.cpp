// browser_service.hpp 的实现:browser/* 方法面的 handler、sidecar RPC、
// 事件转发、审批与取消的接线(阶段 3)。
#include "app_server/browser_service.hpp"

#include <algorithm>
#include <utility>

#include "agent/model_image_store.hpp"  // DecodeBase64Strict / ReadImageDimensions
#include "app_server/schema.hpp"
#include "hooks/hash.hpp"
#include "mcp/rich_result.hpp"  // LandToolArtifact:内容寻址落盘
#include "runtime/id_authority.hpp"

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server/browser] %s\n", text.c_str());
}

// 协议方法名 -> sidecar 方法名(一对一映射,只有 start/stop 两枚特殊)。
std::string SidecarMethodFor(std::string_view method) {
    if (method == kMethodBrowserStart) {
        return "session/start";
    }
    if (method == kMethodBrowserStop) {
        return "session/stop";
    }
    constexpr std::string_view kPrefix = "browser/";
    if (method.substr(0, kPrefix.size()) == kPrefix) {
        return std::string(method.substr(kPrefix.size()));
    }
    return std::string(method);
}

// 写动作才过审批(读动作 snapshot/screenshot 不问;start/stop 是宿主自己
// 的生命周期,也不问)。screencast/start|stop(阶段 C)同归只读一档:
// 起停镜像流不改页面状态,与 snapshot/screenshot 同判——不问审批。
bool MethodNeedsApproval(std::string_view method) {
    return method == kMethodBrowserPageOpen || method == kMethodBrowserPageNavigate ||
           method == kMethodBrowserPageBack || method == kMethodBrowserPageForward ||
           method == kMethodBrowserPageReload || method == kMethodBrowserPageSelect ||
           method == kMethodBrowserPageClose || method == kMethodBrowserAction;
}

// 把 sidecar 的 tabs 行(page_id 蛇形)折成协议的 camelCase。
nlohmann::json ShapePageRows(const nlohmann::json& result) {
    nlohmann::json shaped = result;
    if (shaped.is_array()) {
        for (nlohmann::json& row : shaped) {
            if (row.contains("page_id")) {
                row["pageId"] = row["page_id"];
                row.erase("page_id");
            }
        }
    }
    return shaped;
}

std::int64_t NowMs(const std::chrono::steady_clock::time_point& since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - since)
        .count();
}

}  // namespace

BrowserService::BrowserService(BrowserServiceOptions options, EventSink sink)
    : options_(std::move(options)), sink_(std::move(sink)) {}

BrowserService::~BrowserService() {
    Shutdown();
}

void BrowserService::AttachTransportForTest(mcp::Transport* transport) {
    attached_transport_ = transport;
    sidecar_alive_.store(transport != nullptr);
}

void BrowserService::Emit(std::string_view method, const nlohmann::json& params) {
    if (sink_) {
        sink_(method, params);
    }
}

// ---------------------------------------------------------------------------
// sidecar 进程管理与 RPC 配对
// ---------------------------------------------------------------------------

SidecarCallResult BrowserService::EnsureSidecar() {
    if (shutting_down_.load()) {
        return SidecarCallResult{false, {}, "browser.shutting_down", "服务正在收线"};
    }
    if (attached_transport_ != nullptr) {
        return SidecarCallResult{true, {}};
    }
    if (owned_transport_ != nullptr && owned_transport_->IsAlive()) {
        return SidecarCallResult{true, {}};
    }
    if (options_.sidecar_command.empty()) {
        return SidecarCallResult{false, {}, "browser.not_configured",
                                 "browser 面没配 sidecar 命令(装配层给 BrowserServiceOptions.sidecar_command)"
                                 "。设 LUBAN_BROWSER_SIDECAR 指向 browser/sidecar.js 再试。"};
    }
    // 懒起:换一只新 transport(旧进程已死,ChildProcess 不可复用)。
    // StdioTransportAdapter = StdioTransport 的 mcp::Transport 皮(Start +
    // WriteLine/Shutdown/IsAlive 一套)。
    owned_transport_ = std::make_unique<mcp::StdioTransportAdapter>();
    const mcp::TransportStartResult started = owned_transport_->Start(
        options_.sidecar_command, options_.sidecar_args, {},
        [this](const std::string& line) { OnSidecarLine(line); });
    if (!started.success) {
        owned_transport_.reset();
        sidecar_alive_.store(false);
        return SidecarCallResult{false, {}, "browser.sidecar_spawn_failed",
                                 "sidecar 起不来(" + options_.sidecar_command + "):" + started.error};
    }
    sidecar_alive_.store(true);
    spawn_count_.fetch_add(1);
    Diagnose("sidecar 已起: " + options_.sidecar_command);
    return SidecarCallResult{true, {}};
}

void BrowserService::OnSidecarLine(const std::string& line) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        Diagnose("sidecar 行解析失败(丢弃): " + line.substr(0, 200));
        return;
    }
    if (!message.is_object()) {
        return;
    }
    const auto id = message.find("id");
    if (id != message.end() && id->is_number()) {
        // 响应:配到 pending 上;配不上(超时删过/换过代)按迟到丢弃。
        const std::int64_t request_id = id->get<std::int64_t>();
        std::shared_ptr<PendingCall> pending;
        {
            std::lock_guard<std::mutex> lock(rpc_mutex_);
            const auto it = pending_calls_.find(request_id);
            if (it == pending_calls_.end()) {
                Diagnose("sidecar 迟到响应丢弃: id=" + std::to_string(request_id));
                return;
            }
            pending = it->second;
            pending_calls_.erase(it);
        }
        pending->promise.set_value(message);
        rpc_cv_.notify_all();
        return;
    }
    const auto method = message.find("method");
    if (method != message.end() && method->is_string() && *method == std::string("event")) {
        const auto params = message.find("params");
        if (params != message.end() && params->is_object()) {
            HandleSidecarEvent(*params);
        }
    }
}

SidecarCallResult BrowserService::Call(const std::string& method, const nlohmann::json& params, int timeout_ms,
                                       const std::atomic<bool>* cancel, std::atomic<std::int64_t>* request_id_out) {
    if (attached_transport_ == nullptr) {
        const SidecarCallResult ensured = EnsureSidecar();
        if (!ensured.ok) {
            return ensured;
        }
    }
    if (cancel != nullptr && cancel->load()) {
        return SidecarCallResult{false, {}, "browser.cancelled", "动作已取消(还没发到 sidecar)。", true};
    }

    const std::int64_t request_id = next_request_id_.fetch_add(1);
    if (request_id_out != nullptr) {
        request_id_out->store(request_id);
    }
    auto pending = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(rpc_mutex_);
        pending_calls_[request_id] = pending;
    }
    mcp::Transport* transport = attached_transport_ != nullptr ? attached_transport_ : owned_transport_.get();
    const nlohmann::json request{{"jsonrpc", "2.0"},
                                 {"id", request_id},
                                 {"method", method},
                                 {"params", params}};
    if (!transport->WriteLine(nlohmann::json(request).dump())) {
        {
            std::lock_guard<std::mutex> lock(rpc_mutex_);
            pending_calls_.erase(request_id);
        }
        // 锁外收口(HandleSidecarGone 自己要拿 rpc_mutex_)。
        HandleSidecarGone("sidecar 写失败(进程已死)");
        return SidecarCallResult{false, {}, "browser.sidecar_dead", "sidecar 进程没响应(写失败),旧 page id 全部作废。"};
    }

    std::future<nlohmann::json> future = pending->promise.get_future();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto grace_deadline = deadline;
    bool cancel_sent = false;
    while (true) {
        if (future.wait_for(std::chrono::milliseconds(25)) == std::future_status::ready) {
            const nlohmann::json response = future.get();
            // 折结果:有 result 走 result;有 error 折稳定码。
            const auto result = response.find("result");
            if (result != response.end()) {
                if (result->is_object() && result->value("cancelled", false)) {
                    return SidecarCallResult{false, *result, result->value("code", std::string("browser.cancelled")),
                                             result->value("message", std::string("动作已取消。")), true};
                }
                return SidecarCallResult{true, *result, "", "", false};
            }
            const auto error = response.find("error");
            if (error != response.end() && error->is_object()) {
                std::string code = "browser.sidecar_error";
                const auto data = error->find("data");
                if (data != error->end() && data->is_object() && data->contains("browserCode")) {
                    code = data->value("browserCode", code);
                }
                return SidecarCallResult{false, {}, code, error->value("message", std::string("sidecar 报错")), false};
            }
            return SidecarCallResult{false, {}, "browser.sidecar_error", "sidecar 回执既没 result 也没 error。"};
        }
        // 取消令:发一次 cancelled 通知,宽限期内等终态(与 MCP 工具路
        // P1.6 的取消先例同款)。
        if (cancel != nullptr && cancel->load() && !cancel_sent) {
            cancel_sent = true;
            const nlohmann::json note{{"jsonrpc", "2.0"},
                                      {"method", "cancelled"},
                                      {"params", {{"requestId", request_id}}}};
            transport->WriteLine(note.dump());
            grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.cancel_grace_ms);
        }
        if (cancel != nullptr && cancel->load() && std::chrono::steady_clock::now() > grace_deadline) {
            std::lock_guard<std::mutex> lock(rpc_mutex_);
            pending_calls_.erase(request_id);
            return SidecarCallResult{false, {}, "browser.cancelled",
                                     "动作已取消(sidecar 宽限期内没给终态,按取消收口;页面未判死)。", true};
        }
        if (attached_transport_ == nullptr && owned_transport_ != nullptr && !owned_transport_->IsAlive()) {
            {
                std::lock_guard<std::mutex> lock(rpc_mutex_);
                pending_calls_.erase(request_id);
            }
            HandleSidecarGone("sidecar 进程退出"); // 锁外(HandleSidecarGone 拿同一把锁)
            return SidecarCallResult{false, {}, "browser.sidecar_dead",
                                     "sidecar 进程死了(旧 page id 全部作废;下次调用自动重起)。"};
        }
        if (!cancel_sent && std::chrono::steady_clock::now() > deadline) {
            std::lock_guard<std::mutex> lock(rpc_mutex_);
            pending_calls_.erase(request_id);
            return SidecarCallResult{false, {}, "browser.sidecar_timeout",
                                     "sidecar " + method + " 超过 " + std::to_string(timeout_ms) + "ms 没回执。"};
        }
        if (cancel_sent && std::chrono::steady_clock::now() > grace_deadline) {
            std::lock_guard<std::mutex> lock(rpc_mutex_);
            pending_calls_.erase(request_id);
            return SidecarCallResult{false, {}, "browser.cancelled",
                                     "动作已取消(sidecar 宽限期内没给终态,按取消收口)。", true};
        }
    }
}

void BrowserService::HandleSidecarGone(const std::string& reason) {
    if (!sidecar_alive_.exchange(false)) {
        return;
    }
    Diagnose("sidecar 没了: " + reason);
    Emit(kEventBrowserCrashed, nlohmann::json{{"reason", "sidecar: " + reason}});
    // 在飞 RPC 全部按 sidecar_dead 收口(下一次调用 EnsureSidecar 自动重起)。
    std::map<std::int64_t, std::shared_ptr<PendingCall>> stranded;
    {
        std::lock_guard<std::mutex> lock(rpc_mutex_);
        stranded.swap(pending_calls_);
    }
    for (auto& [id, pending] : stranded) {
        nlohmann::json error{{"error", {{"code", -32000},
                                        {"message", "sidecar 进程没了"},
                                        {"data", {{"browserCode", "browser.sidecar_dead"}}}}}};
        pending->promise.set_value(std::move(error));
    }
    rpc_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// sidecar 事件 -> 协议事件
// ---------------------------------------------------------------------------

void BrowserService::HandleSidecarEvent(const nlohmann::json& params) {
    const std::string type = params.value("type", std::string());
    nlohmann::json shaped = params;
    shaped.erase("type");
    if (type == "session/started") {
        Emit(kEventBrowserStarted, shaped);
    } else if (type == "session/stopped") {
        Emit(kEventBrowserStopped, shaped);
    } else if (type == "session/crashed") {
        // 浏览器崩了(sidecar 进程还活着):旧 page id 全作废,browser/start
        // 可重开。在飞动作会从 sidecar 收到 browser.crashed 错误收口。
        Emit(kEventBrowserCrashed, shaped);
    } else if (type == "page/created") {
        Emit(kEventBrowserPageCreated, shaped);
    } else if (type == "page/closed") {
        Emit(kEventBrowserPageClosed, shaped);
    } else if (type == "page/navigation") {
        Emit(kEventBrowserNavigation, shaped);
    } else if (type == "page/selected") {
        Emit(kEventBrowserPageUpdated, shaped);
    } else if (type == "user_epoch") {
        Emit(kEventBrowserUserEpoch, shaped);
    } else if (type == "download/event") {
        Emit(kEventBrowserDownloadEvent, shaped);
    } else if (type == "journal/console") {
        Emit(kEventBrowserConsoleEvent, shaped);
    } else if (type == "journal/network") {
        Emit(kEventBrowserNetworkEvent, shaped);
    } else if (type == "screencast/frame") {
        // 镜像流帧(阶段 C):不直接 Emit——落盘可能比帧到达慢,排进有界
        // 队列由专职工作线程消化(见 HandleScreencastFrame 文件头注)。
        HandleScreencastFrame(shaped);
    } else {
        Diagnose("未知 sidecar 事件(丢弃): " + type);
    }
}

// ---------------------------------------------------------------------------
// 动作管线
// ---------------------------------------------------------------------------

nlohmann::json BrowserService::AcceptAction(const std::string& method, const std::string& sidecar_method,
                                            nlohmann::json input, nlohmann::json sidecar_params,
                                            bool needs_approval) {
    auto action = std::make_shared<BrowserAction>();
    action->action_id = "br-" + std::to_string(runtime::ProcessIdAuthority().NextSeq());
    action->method = method;
    action->sidecar_method = sidecar_method;
    action->owner = input.value("owner", std::string("user"));
    action->thread_id = input.value("threadId", std::string());
    action->input = std::move(input);
    action->sidecar_params = std::move(sidecar_params);
    action->needs_approval = needs_approval;
    action->started_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(actions_mutex_);
        actions_[action->action_id] = action;
        action_queue_.push_back(action);
        if (!action_worker_.joinable()) {
            action_worker_ = std::thread([this] { ActionWorkerLoop(); });
        }
    }
    action_cv_.notify_all();
    nlohmann::json result;
    result["actionId"] = action->action_id;
    result["accepted"] = true;
    result["owner"] = action->owner;
    if (!action->thread_id.empty()) {
        result["threadId"] = action->thread_id;
    }
    return result;
}

void BrowserService::ActionWorkerLoop() {
    while (true) {
        std::shared_ptr<BrowserAction> action;
        {
            std::unique_lock<std::mutex> lock(actions_mutex_);
            action_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                [this] { return !action_queue_.empty() || shutting_down_.load(); });
            if (action_queue_.empty()) {
                if (shutting_down_.load()) {
                    return;
                }
                continue;
            }
            // 用户让路(阶段 B):队里先挑用户动作——用户动作不排在任何
            // Agent 动作后头;没有用户动作才轮到最老的 Agent 动作。同类
            // 之间照旧 FIFO。在途那只让不了:一份浏览器状态一位主人,
            // 串行是 Runtime 的仲裁底线,用户动作顶多等它收尾。
            std::size_t pick = 0;
            for (std::size_t i = 0; i < action_queue_.size(); ++i) {
                if (action_queue_[i]->owner == "user") {
                    pick = i;
                    break;
                }
            }
            action = action_queue_[pick];
            action_queue_.erase(action_queue_.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        RunAction(action);
    }
}

void BrowserService::RunAction(const std::shared_ptr<BrowserAction>& action) {
    // action/started:亮出动作(仲裁第 2 条:Agent 即将点、输、导航时,
    // 前端看得见)。
    nlohmann::json started_params;
    started_params["actionId"] = action->action_id;
    started_params["method"] = action->method;
    started_params["owner"] = action->owner;
    if (!action->thread_id.empty()) {
        started_params["threadId"] = action->thread_id;
    }
    started_params["input"] = action->input;
    Emit(kEventBrowserActionStarted, started_params);

    SidecarCallResult outcome;
    bool proceed = true; // 审批拒了/悬空了/暂停了就不再发 sidecar
    // ---- 暂停门(阶段 B):暂停期间 Agent 动作一律受理不执行、明报
    // "已暂停"(终态事件照发);用户动作不走这道门。审批也不问——
    // 暂停的手闸在用户手里,先问审批等于让 Agent 的事占用户的线。----
    if (paused_.load() && action->owner == "agent") {
        proceed = false;
        outcome = SidecarCallResult{
            false, {}, "browser.paused",
            "浏览器已暂停(browser/pause),Agent 动作受理不执行;browser/resume 后重发,或让用户替你点。"};
    }
    // ---- 审批(owner=agent 的写动作) ----
    else if (action->needs_approval) {
        runtime::ApprovalRequest request;
        request.tool_use_id = action->action_id;
        request.tool_name = action->method;
        request.input = action->input;
        request.reason = "浏览器写动作(owner=agent):宿主可拦可问。";
        std::optional<ApprovalTicket> ticket;
        if (approval_ask_) {
            ticket = approval_ask_(action->thread_id, request, &action->cancelled);
        }
        if (!ticket.has_value() || ticket->future == nullptr) {
            proceed = false;
            outcome = SidecarCallResult{false, {}, "browser.unknown_thread",
                                        "owner=agent 的动作挂在 thread " + action->thread_id + " 上,没有这场 thread。"};
        } else {
            const std::optional<runtime::ApprovalResponse> decision = ticket->future->WaitApproval();
            if (!decision.has_value()) {
                // 悬空收口(打断/超时/取消):把这枚悬起件从账上摘掉——
                // 审批人没答,迟到的答复须按 stale 收口,不许吞进死 promise。
                if (ticket->cancel) {
                    ticket->cancel();
                }
                proceed = false;
                outcome = SidecarCallResult{
                    false, {}, "browser.approval_cancelled",
                    action->cancelled.load()
                        ? "动作被取消,审批按取消收口,未执行。"
                        : "审批悬空收口(打断/超时/断线),动作未执行。",
                    action->cancelled.load()};
            } else if (decision->decision != runtime::InteractionDecision::Accept &&
                       decision->decision != runtime::InteractionDecision::AcceptForSession) {
                // acceptForSession 与 accept 同路放行(会话级放行账已在
                // InteractionLedger 的答复侧落好,后续同方法名免问)。
                proceed = false;
                outcome = SidecarCallResult{
                    false, {}, "browser.permission_denied",
                    "浏览器动作被拒(" + runtime::ToString(decision->decision) + ")" +
                        (decision->reason.empty() ? std::string() : ":" + decision->reason)};
            }
        }
    }
    // 审批放行后才拨下暂停的:一样不执行——暂停对 owner=agent 的动作
    // 一律生效,不分受理先后;已进 sidecar 的在途调用追不回,终态照发。
    if (proceed && paused_.load() && action->owner == "agent") {
        proceed = false;
        outcome = SidecarCallResult{
            false, {}, "browser.paused",
            "浏览器已暂停(browser/pause),Agent 动作受理不执行;browser/resume 后重发,或让用户替你点。"};
    }
    // ---- sidecar 调用 ----
    if (proceed) {
        outcome = Call(action->sidecar_method, action->sidecar_params, options_.action_deadline_ms,
                       &action->cancelled, &action->sidecar_request_id);
    }
    // ---- 截图收口(artifact 引用,不递 base64) ----
    nlohmann::json result = outcome.result;
    if (outcome.ok && action->method == kMethodBrowserScreenshot) {
        std::string error_code;
        std::string error_message;
        result = FinishScreenshot(outcome.result, error_code, error_message);
        if (!error_code.empty()) {
            outcome = SidecarCallResult{false, {}, error_code, error_message};
        }
    }

    // ---- action/completed(终态,must_keep) ----
    nlohmann::json completed_params;
    completed_params["actionId"] = action->action_id;
    completed_params["method"] = action->method;
    completed_params["owner"] = action->owner;
    if (!action->thread_id.empty()) {
        completed_params["threadId"] = action->thread_id;
    }
    completed_params["ok"] = outcome.ok;
    if (outcome.ok) {
        completed_params["result"] = std::move(result);
    } else {
        nlohmann::json error;
        error["code"] = outcome.error_code;
        error["message"] = outcome.error_message;
        completed_params["error"] = std::move(error);
        if (outcome.cancelled) {
            completed_params["cancelled"] = true;
        }
    }
    completed_params["durationMs"] = NowMs(action->started_at);
    Emit(kEventBrowserActionCompleted, completed_params);

    action->finished.store(true);
    std::lock_guard<std::mutex> lock(actions_mutex_);
    actions_.erase(action->action_id);
}

nlohmann::json BrowserService::FinishScreenshot(const nlohmann::json& sidecar_result, std::string& out_error_code,
                                                std::string& out_error_message) {
    out_error_code.clear();
    out_error_message.clear();
    if (options_.artifact_dir.empty()) {
        out_error_code = "browser.artifact_unavailable";
        out_error_message = "服务没配截图 artifact 目录(BrowserServiceOptions.artifact_dir),字节无处落,不冒充可取。";
        return nlohmann::json();
    }
    const std::string data_base64 = sidecar_result.value("dataBase64", std::string());
    const std::expected<std::string, std::string> decoded =
        agent::DecodeBase64Strict(data_base64, 20 * 1024 * 1024);
    if (!decoded.has_value()) {
        out_error_code = "browser.artifact_invalid";
        out_error_message = "截图字节解码失败: " + decoded.error();
        return nlohmann::json();
    }
    const std::string sha256 = hooks::Sha256Hex(*decoded);
    const std::string claimed = sidecar_result.value("sha256", std::string());
    if (!claimed.empty() && claimed != sha256) {
        out_error_code = "browser.artifact_mismatch";
        out_error_message = "截图字节与 sidecar 报的 sha256 对不上,弃用。";
        return nlohmann::json();
    }
    const std::string relative = mcp::LandToolArtifact(options_.artifact_dir, *decoded, "png");
    if (relative.empty()) {
        out_error_code = "browser.artifact_write_failed";
        out_error_message = "截图落盘失败: " + options_.artifact_dir;
        return nlohmann::json();
    }
    const agent::ImageDimensions dims = agent::ReadImageDimensions(*decoded, "image/png");
    nlohmann::json artifact;
    artifact["id"] = "art-" + sha256.substr(0, 8);
    artifact["filename"] = sha256 + ".png";  // P0-2:文件名即内容地址
    artifact["path"] = relative;
    artifact["mime_type"] = "image/png";
    artifact["bytes"] = decoded->size();
    artifact["sha256"] = sha256;
    artifact["stored"] = true;
    nlohmann::json image;
    image["mime_type"] = "image/png";
    image["width"] = dims.width;
    image["height"] = dims.height;
    image["bytes"] = decoded->size();
    image["sha256"] = sha256;
    image["artifact"] = std::move(artifact);

    // screenshot/ready:前端凭 artifact 取图;协议上没有 base64。
    nlohmann::json ready;
    ready["pageId"] = sidecar_result.value("pageId", std::string());
    ready["generation"] = sidecar_result.value("generation", 0);
    ready["url"] = sidecar_result.value("url", std::string());
    ready["fullPage"] = sidecar_result.value("fullPage", false);
    ready["image"] = image;
    Emit(kEventBrowserScreenshotReady, ready);

    nlohmann::json result = sidecar_result;
    result.erase("dataBase64"); // 字节只进 artifact,不进协议
    result["image"] = std::move(image);
    return result;
}

// ---------------------------------------------------------------------------
// 镜像流(阶段 C):有界队列 + 专职工作线程,落盘赶不上帧到达就丢最老帧
// ---------------------------------------------------------------------------

void BrowserService::HandleScreencastFrame(const nlohmann::json& payload) {
    if (shutting_down_.load()) {
        return; // 收线途中不再受理新帧
    }
    PendingScreencastFrame frame;
    frame.page_id = payload.value("pageId", std::string());
    frame.data_base64 = payload.value("dataBase64", std::string());
    const std::string format = payload.value("format", std::string("jpeg"));
    frame.mime_type = format == "png" ? "image/png" : "image/jpeg";
    frame.claimed_sha256 = payload.value("sha256", std::string());
    frame.frame_seq = payload.value("frameSeq", std::uint64_t{0});

    bool spawn_worker = false;
    {
        std::lock_guard<std::mutex> lock(screencast_mutex_);
        const std::size_t capacity = static_cast<std::size_t>(std::max(options_.screencast_queue_capacity, 1));
        if (screencast_queue_.size() >= capacity) {
            // 队满:丢队首(最老那帧),按它的 pageId 记账——丢旧留新是
            // 镜像流的方向(前端要的是"现在的页面",不是补全历史)。
            const std::string evicted_page = screencast_queue_.front().page_id;
            screencast_queue_.pop_front();
            screencast_dropped_[evicted_page] += 1;
            screencast_dropped_total_[evicted_page] += 1;
        }
        screencast_queue_.push_back(std::move(frame));
        if (!screencast_worker_started_.exchange(true)) {
            spawn_worker = true;
        }
    }
    if (spawn_worker) {
        screencast_worker_ = std::thread([this] { ScreencastWorkerLoop(); });
    }
    screencast_cv_.notify_all();
}

void BrowserService::ScreencastWorkerLoop() {
    while (true) {
        PendingScreencastFrame frame;
        {
            std::unique_lock<std::mutex> lock(screencast_mutex_);
            screencast_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                    [this] { return !screencast_queue_.empty() || shutting_down_.load(); });
            if (screencast_queue_.empty()) {
                if (shutting_down_.load()) {
                    return;
                }
                continue;
            }
            frame = std::move(screencast_queue_.front());
            screencast_queue_.pop_front();
        }
        EmitScreencastFrame(frame);
    }
}

void BrowserService::EmitScreencastFrame(const PendingScreencastFrame& frame) {
    std::uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(screencast_mutex_);
        const auto it = screencast_dropped_.find(frame.page_id);
        if (it != screencast_dropped_.end()) {
            dropped = it->second;
            it->second = 0; // 报完清零(journal 批量的 dropped 同口径)
        }
    }
    if (options_.artifact_dir.empty()) {
        Diagnose("镜像流帧丢弃(没配 artifact_dir,字节无处落): pageId=" + frame.page_id);
        return;
    }
    const std::expected<std::string, std::string> decoded =
        agent::DecodeBase64Strict(frame.data_base64, 20 * 1024 * 1024);
    if (!decoded.has_value()) {
        Diagnose("镜像流帧解码失败(丢弃): " + decoded.error());
        return;
    }
    const std::string sha256 = hooks::Sha256Hex(*decoded);
    if (!frame.claimed_sha256.empty() && frame.claimed_sha256 != sha256) {
        Diagnose("镜像流帧与 sidecar 报的 sha256 对不上(丢弃): pageId=" + frame.page_id);
        return;
    }
    const std::string extension = frame.mime_type == "image/png" ? "png" : "jpeg";
    const std::string relative = mcp::LandToolArtifact(options_.artifact_dir, *decoded, extension);
    if (relative.empty()) {
        Diagnose("镜像流帧落盘失败: " + options_.artifact_dir);
        return;
    }
    const agent::ImageDimensions dims = agent::ReadImageDimensions(*decoded, frame.mime_type);
    nlohmann::json artifact;
    artifact["id"] = "art-" + sha256.substr(0, 8);
    artifact["filename"] = sha256 + "." + extension;  // P0-2:文件名即内容地址
    artifact["path"] = relative;
    artifact["mime_type"] = frame.mime_type;
    artifact["bytes"] = decoded->size();
    artifact["sha256"] = sha256;
    artifact["stored"] = true;

    // frame 事件只带引用与 page_id(§4.3):同截图链,不递 base64。
    nlohmann::json params;
    params["pageId"] = frame.page_id;
    params["frameSeq"] = frame.frame_seq;
    params["width"] = dims.width;
    params["height"] = dims.height;
    params["dropped"] = dropped;
    params["artifact"] = std::move(artifact);
    Emit(kEventBrowserScreencastFrame, params);
}

std::size_t BrowserService::screencast_queue_depth() {
    std::lock_guard<std::mutex> lock(screencast_mutex_);
    return screencast_queue_.size();
}

std::uint64_t BrowserService::screencast_dropped_total_for_test(const std::string& page_id) {
    std::lock_guard<std::mutex> lock(screencast_mutex_);
    const auto it = screencast_dropped_total_.find(page_id);
    return it == screencast_dropped_total_.end() ? 0 : it->second;
}

// ---------------------------------------------------------------------------
// 方法注册
// ---------------------------------------------------------------------------

void BrowserService::RegisterMethods(
    Dispatcher& dispatcher, const std::function<std::shared_ptr<ThreadRecord>(const std::string&)>& find_thread) {
    // find_thread 只用于审批口的存活校验(真校验在 Server 的 ask 回调里)。
    (void)find_thread;

    // ---- 异步动作面(受理即回 actionId,终态走 browser/action/completed) ----
    // 公共受理皮:checker 先验参数;owner 由内核按连接裁定(阶段 B,§六:
    // 外壳报的 owner 只是意向,principal 说了算);owner=agent 的写动作查
    // 审批口与 threadId;参数折算(threadId/actionId 是协议层的仲裁字段,
    // 不进 sidecar;owner 留给 sidecar——输入动作执行后靠它递 userEpoch;
    // start 的 headed/timeoutMs 翻成 sidecar 的 headless/actionTimeoutMs)。
    const auto register_async = [this, &dispatcher](
                                   std::string_view method,
                                   const std::function<ParamsCheck(const nlohmann::json&)>& checker) {
        dispatcher.RegisterMethod(
            method, [this, method, checker](const IncomingRequest& request, DispatchContext& context)
                             -> std::optional<nlohmann::json> {
            const ParamsCheck base = checker(request.params);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            const nlohmann::json& params = request.params;
            // ---- owner 裁定(内核真值) ----
            // 缺省 = 这条连接自己的手:用户连接缺省 user;agent 连接缺省
            // agent(缺省也逃不过审批——不许靠"不说 owner"绕门)。
            // 假冒:非用户连接报 owner=user,browser.owner_denied 明拒。
            const std::string claimed = params.value("owner", std::string());
            std::string owner;
            if (claimed.empty()) {
                owner = context.principal == "agent" ? "agent" : "user";
            } else if (claimed == "user") {
                if (context.principal == "agent") {
                    return MakeError(request.id, kErrInvalidParams,
                                     std::string(method) + ": owner 由内核按连接裁定,这条连接不是用户连接,假冒 user 被拒。",
                                     nlohmann::json{{"reason", "browser.owner_denied"}});
                }
                owner = "user";
            } else if (claimed == "agent") {
                owner = "agent";
            } else {
                return MakeError(request.id, kErrInvalidParams,
                                 std::string(method) + ": owner 只认 agent|user,收到: " + claimed,
                                 nlohmann::json{{"reason", "browser.bad_owner"}});
            }
            const bool needs_approval = owner == "agent" && MethodNeedsApproval(method);
            if (needs_approval) {
                if (approval_ask_ == nullptr) {
                    return MakeError(request.id, kErrInvalidParams,
                                     std::string(method) + ": owner=agent 须过审批,这台服务没接审批口",
                                     nlohmann::json{{"reason", "browser.approval_unavailable"}});
                }
                if (params.value("threadId", std::string()).empty()) {
                    return MakeError(request.id, kErrInvalidParams,
                                     std::string(method) + ": owner=agent 须带 threadId(审批挂这场 thread)",
                                     nlohmann::json{{"reason", "browser.thread_required"}});
                }
            }
            // 事件与审批展示用裁定后的 owner(外壳报的什么从此作废)。
            nlohmann::json effective = params;
            effective["owner"] = owner;
            if (owner == "user") {
                // 用户不是 Agent:threadId 就算带了也只是噪音,摘掉。
                effective.erase("threadId");
            }
            nlohmann::json sidecar_params = effective;
            sidecar_params.erase("threadId");
            if (method == kMethodBrowserStart) {
                if (params.contains("headed")) {
                    sidecar_params["headless"] = !params.value("headed", false);
                    sidecar_params.erase("headed");
                }
                if (params.contains("timeoutMs")) {
                    sidecar_params["actionTimeoutMs"] = params.value("timeoutMs", 0);
                    sidecar_params.erase("timeoutMs");
                }
            }
            // 先确保 sidecar 活着(起不来的错在受理时就回,不进事件流)。
            const SidecarCallResult ensured = EnsureSidecar();
            if (!ensured.ok) {
                return MakeError(request.id, kErrInvalidParams,
                                 std::string(method) + " 失败: " + ensured.error_message,
                                 nlohmann::json{{"reason", ensured.error_code}});
            }
            return MakeResult(request.id, AcceptAction(std::string(method), SidecarMethodFor(method),
                                                       std::move(effective), std::move(sidecar_params),
                                                       needs_approval));
        });
    };

    register_async(kMethodBrowserStart, [](const nlohmann::json& params) {
        return CheckBrowserStartParams(params);
    });
    register_async(kMethodBrowserStop, [](const nlohmann::json& params) {
        return CheckBrowserStopParams(params);
    });
    register_async(kMethodBrowserPageOpen, [](const nlohmann::json& params) {
        std::string url;
        return CheckBrowserPageOpenParams(params, url);
    });
    register_async(kMethodBrowserPageNavigate, [](const nlohmann::json& params) {
        std::string page_id;
        std::string url;
        return CheckBrowserPageNavigateParams(params, page_id, url);
    });
    register_async(kMethodBrowserPageBack, [](const nlohmann::json& params) {
        std::string page_id;
        return CheckBrowserPageTargetParams(params, kMethodBrowserPageBack, page_id);
    });
    register_async(kMethodBrowserPageForward, [](const nlohmann::json& params) {
        std::string page_id;
        return CheckBrowserPageTargetParams(params, kMethodBrowserPageForward, page_id);
    });
    register_async(kMethodBrowserPageReload, [](const nlohmann::json& params) {
        std::string page_id;
        return CheckBrowserPageTargetParams(params, kMethodBrowserPageReload, page_id);
    });
    register_async(kMethodBrowserPageSelect, [](const nlohmann::json& params) {
        std::string page_id;
        return CheckBrowserPageTargetParams(params, kMethodBrowserPageSelect, page_id);
    });
    register_async(kMethodBrowserPageClose, [](const nlohmann::json& params) {
        std::string page_id;
        return CheckBrowserPageTargetParams(params, kMethodBrowserPageClose, page_id);
    });
    register_async(kMethodBrowserSnapshot, [](const nlohmann::json& params) {
        return CheckParamsIsObject(params, kMethodBrowserSnapshot);
    });
    register_async(kMethodBrowserScreenshot, [](const nlohmann::json& params) {
        return CheckParamsIsObject(params, kMethodBrowserScreenshot);
    });
    register_async(kMethodBrowserAction, [](const nlohmann::json& params) {
        std::string kind;
        return CheckBrowserActionParams(params, kind);
    });
    // 镜像流起停(阶段 C):只读,不问审批(MethodNeedsApproval 未收录);
    // 走同一条异步动作管线只为复用受理/owner 裁定/sidecar 往返的现成
    // 骨架——起停本身很快,不必单独开路。真正的帧不经这条管线,见
    // HandleScreencastFrame。
    register_async(kMethodBrowserScreencastStart, [](const nlohmann::json& params) {
        return CheckBrowserScreencastStartParams(params);
    });
    register_async(kMethodBrowserScreencastStop, [](const nlohmann::json& params) {
        return CheckBrowserScreencastStopParams(params);
    });

    // ---- 同步查询面(读线程直答;错误折 error.data.reason 稳定码) ----
    const auto register_query = [this, &dispatcher](std::string_view method, const char* sidecar_method,
                                                    bool shape_rows, bool add_sidecar_flag) {
        dispatcher.RegisterMethod(
            method, [this, method, sidecar_method, shape_rows, add_sidecar_flag](
                        const IncomingRequest& request, DispatchContext&) -> std::optional<nlohmann::json> {
            const ParamsCheck base = CheckParamsIsObject(request.params, method);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            const SidecarCallResult ensured = EnsureSidecar();
            if (!ensured.ok) {
                return MakeError(request.id, kErrInvalidParams,
                                 std::string(method) + " 失败: " + ensured.error_message,
                                 nlohmann::json{{"reason", ensured.error_code}});
            }
            // journal 查询的参数表(console/network 共用)。
            nlohmann::json sidecar_params = request.params;
            if (method == kMethodBrowserConsoleQuery || method == kMethodBrowserNetworkQuery) {
                std::string page_id;
                std::uint64_t since_seq = 0;
                const ParamsCheck journal = CheckBrowserJournalQueryParams(request.params, method, page_id, since_seq);
                if (!journal.ok) {
                    return MakeError(request.id, journal.code, journal.message);
                }
            }
            const SidecarCallResult outcome =
                Call(sidecar_method, sidecar_params, options_.query_timeout_ms);
            if (!outcome.ok) {
                return MakeError(request.id, kErrInvalidParams,
                                 std::string(method) + " 失败: " + outcome.error_message,
                                 nlohmann::json{{"reason", outcome.error_code}});
            }
            nlohmann::json result = shape_rows ? ShapePageRows(outcome.result) : outcome.result;
            if (add_sidecar_flag && result.is_object()) {
                result["sidecarRunning"] = true;
            }
            if (method == kMethodBrowserStatus && result.is_object()) {
                // 暂停旗在内核这层,sidecar 不知道——status 把它捎上,
                // 外壳的暂停灯有账可对。
                result["paused"] = paused_.load();
            }
            return MakeResult(request.id, std::move(result));
        });
    };

    register_query(kMethodBrowserStatus, "session/status", false, true);
    register_query(kMethodBrowserPageList, "page/list", true, false);
    register_query(kMethodBrowserConsoleQuery, "console/query", false, false);
    register_query(kMethodBrowserNetworkQuery, "network/query", false, false);
    register_query(kMethodBrowserDownloadsQuery, "downloads/query", false, false);

    // ---- browser/action/cancel ----
    dispatcher.RegisterMethod(
        kMethodBrowserActionCancel, [this](const IncomingRequest& request, DispatchContext&)
                                            -> std::optional<nlohmann::json> {
            std::string action_id;
            const ParamsCheck base = CheckBrowserActionCancelParams(request.params, action_id);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            std::shared_ptr<BrowserAction> action;
            {
                std::lock_guard<std::mutex> lock(actions_mutex_);
                const auto it = actions_.find(action_id);
                if (it != actions_.end()) {
                    action = it->second;
                }
            }
            if (action == nullptr || action->finished.load()) {
                return MakeError(request.id, kErrStaleRequestId,
                                 "动作不在跑(收口了/没这只): " + action_id,
                                 nlohmann::json{{"reason", "browser.stale_action"}});
            }
            action->cancelled.store(true);
            const std::int64_t request_id = action->sidecar_request_id.load();
            if (request_id > 0) {
                mcp::Transport* transport =
                    attached_transport_ != nullptr ? attached_transport_ : owned_transport_.get();
                if (transport != nullptr) {
                    const nlohmann::json note{{"jsonrpc", "2.0"},
                                              {"method", "cancelled"},
                                              {"params", {{"requestId", request_id}}}};
                    transport->WriteLine(note.dump());
                }
            }
            return MakeResult(request.id, nlohmann::json{{"actionId", action_id}, {"cancelled", true}});
        });

    // ---- browser/pause|resume(阶段 B) ----
    // 暂停旗的拨杆:同步直答(不碰 sidecar——旗在内核),另发
    // browser/paused|resumed 通报(must_keep,暂停灯丢了就与内核拧着)。
    // 手闸只归用户连接:agent 侧不许按(拿暂停卡死用户、或趁隙放行,
    // 都是 Agent 的手伸进用户的地界)。
    const auto register_pause_toggle = [this, &dispatcher](std::string_view method, bool pause) {
        dispatcher.RegisterMethod(
            method, [this, method, pause](const IncomingRequest& request, DispatchContext& context)
                           -> std::optional<nlohmann::json> {
            const ParamsCheck base = CheckParamsIsObject(request.params, method);
            if (!base.ok) {
                return MakeError(request.id, base.code, base.message);
            }
            if (context.principal == "agent") {
                return MakeError(request.id, kErrInvalidParams,
                                 std::string(method) + ": 暂停手闸只归用户连接,这条连接不是用户连接。",
                                 nlohmann::json{{"reason", "browser.owner_denied"}});
            }
            paused_.store(pause);
            Emit(pause ? kEventBrowserPaused : kEventBrowserResumed, nlohmann::json{{"paused", pause}});
            return MakeResult(request.id, nlohmann::json{{"paused", pause}});
        });
    };
    register_pause_toggle(kMethodBrowserPause, true);
    register_pause_toggle(kMethodBrowserResume, false);
}

// ---------------------------------------------------------------------------
// 取消与收线
// ---------------------------------------------------------------------------

std::size_t BrowserService::CancelActionsForThread(const std::string& thread_id, const std::string& reason) {
    std::vector<std::shared_ptr<BrowserAction>> hits;
    {
        std::lock_guard<std::mutex> lock(actions_mutex_);
        for (auto& [id, action] : actions_) {
            if (!action->thread_id.empty() && action->thread_id == thread_id && !action->finished.load()) {
                hits.push_back(action);
            }
        }
    }
    for (const std::shared_ptr<BrowserAction>& action : hits) {
        action->cancelled.store(true);
        const std::int64_t request_id = action->sidecar_request_id.load();
        if (request_id > 0) {
            mcp::Transport* transport = attached_transport_ != nullptr ? attached_transport_ : owned_transport_.get();
            if (transport != nullptr) {
                const nlohmann::json note{{"jsonrpc", "2.0"},
                                          {"method", "cancelled"},
                                          {"params", {{"requestId", request_id}}}};
                transport->WriteLine(note.dump());
            }
        }
        Diagnose("取消 thread 名下浏览器动作 " + action->action_id + ": " + reason);
    }
    return hits.size();
}

std::size_t BrowserService::active_action_count() {
    std::lock_guard<std::mutex> lock(actions_mutex_);
    return actions_.size();
}

void BrowserService::KillSidecarForTest() {
    if (owned_transport_ != nullptr) {
        owned_transport_->Shutdown(0); // 立刻杀树,不给体面退场
        owned_transport_.reset();
    }
    sidecar_alive_.store(false);
}

void BrowserService::Shutdown() {
    if (shutting_down_.exchange(true)) {
        return;
    }
    // 在飞动作全取消(审批悬着的也醒)。
    {
        std::lock_guard<std::mutex> lock(actions_mutex_);
        for (auto& [id, action] : actions_) {
            action->cancelled.store(true);
        }
    }
    action_cv_.notify_all();
    if (action_worker_.joinable()) {
        action_worker_.join();
    }
    // 镜像流工作线程同样收线(队里剩的帧不再落盘,进程都要退了没意义)。
    screencast_cv_.notify_all();
    if (screencast_worker_.joinable()) {
        screencast_worker_.join();
    }
    // 收尸:杀 sidecar 进程树(profile 锁由 sidecar 的退出钩子释放)。
    if (owned_transport_ != nullptr) {
        owned_transport_->Shutdown(2000);
        owned_transport_.reset();
    }
    sidecar_alive_.store(false);
}

}  // namespace lubancode::app_server
