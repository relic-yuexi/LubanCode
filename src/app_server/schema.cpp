// schema.hpp 的实现:纯函数,不碰 IO、不开线程,单测直喂直断。
#include "app_server/schema.hpp"

#include "platform/json_safe.hpp"

namespace lubancode::app_server {

// ---------------------------------------------------------------------------
// 出站信封
// ---------------------------------------------------------------------------

nlohmann::json MakeEvent(std::string_view method, nlohmann::json params) {
    nlohmann::json message = nlohmann::json::object();
    if (kEmitJsonRpcField) {
        message["jsonrpc"] = std::string(kJsonRpcVersion);
    }
    message["method"] = std::string(method);
    message["params"] = std::move(params);
    return message;
}

nlohmann::json MakeResult(std::int64_t id, nlohmann::json result) {
    nlohmann::json message = nlohmann::json::object();
    if (kEmitJsonRpcField) {
        message["jsonrpc"] = std::string(kJsonRpcVersion);
    }
    message["id"] = id;
    message["result"] = std::move(result);
    return message;
}

nlohmann::json MakeError(std::int64_t id, int code, std::string_view message_text,
                         const nlohmann::json& data) {
    nlohmann::json error = nlohmann::json::object();
    error["code"] = code;
    error["message"] = std::string(message_text);
    if (!data.is_null()) {
        error["data"] = data;
    }
    nlohmann::json message = nlohmann::json::object();
    if (kEmitJsonRpcField) {
        message["jsonrpc"] = std::string(kJsonRpcVersion);
    }
    message["id"] = id;
    message["error"] = std::move(error);
    return message;
}

nlohmann::json MakeErrorForUnparseable(int code, std::string_view message_text) {
    nlohmann::json error = nlohmann::json::object();
    error["code"] = code;
    error["message"] = std::string(message_text);
    nlohmann::json message = nlohmann::json::object();
    if (kEmitJsonRpcField) {
        message["jsonrpc"] = std::string(kJsonRpcVersion);
    }
    message["id"] = nullptr; // id 无从捞起,按 JSON-RPC 的规矩回 null
    message["error"] = std::move(error);
    return message;
}

std::string SerializeMessage(const nlohmann::json& message) {
    // DumpJsonSanitized 永不抛:出站行必须永远可解析,这是 stdout 分帧
    // 纪律的底线。坏 UTF-8 被替换字符洗过,好过吐一行解不开的东西。
    return platform::DumpJsonSanitized(message);
}

// ---------------------------------------------------------------------------
// 入站信封
// ---------------------------------------------------------------------------

std::optional<IncomingMessage> ParseIncoming(const std::string& line, EnvelopeError& out_error) {
    out_error = EnvelopeError{};

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        out_error.code = kErrParseError;
        out_error.message = "报文不是合法 JSON";
        return std::nullopt;
    }

    if (!parsed.is_object()) {
        out_error.code = kErrParseError;
        out_error.message = "报文不是 JSON 对象";
        return std::nullopt;
    }

    // id:三态——没有(通知)、数字(请求/响应)、别的类型(坏)。
    const bool has_id = parsed.contains("id");
    std::int64_t id = 0;
    if (has_id) {
        const nlohmann::json& id_value = parsed["id"];
        if (id_value.is_number_integer() || id_value.is_number_unsigned()) {
            id = id_value.get<std::int64_t>();
            out_error.has_id = true;
            out_error.id = id;
        } else if (id_value.is_null()) {
            // null id 只在错误响应里合法;作为入站请求算坏。
            out_error.code = kErrInvalidRequest;
            out_error.message = "id 为 null";
            return std::nullopt;
        } else {
            out_error.code = kErrInvalidRequest;
            out_error.message = "id 必须是整数";
            return std::nullopt;
        }
    }

    const bool has_method = parsed.contains("method") && !parsed["method"].is_null();
    const bool has_result = parsed.contains("result") && !parsed["result"].is_null();
    const bool has_error = parsed.contains("error") && !parsed["error"].is_null();

    IncomingMessage message;
    if (has_id && has_method) {
        if (!parsed["method"].is_string()) {
            out_error.code = kErrInvalidRequest;
            out_error.message = "method 必须是字符串";
            return std::nullopt;
        }
        message.kind = IncomingMessage::Kind::Request;
        message.request.id = id;
        message.request.method = parsed["method"].get<std::string>();
        if (parsed.contains("params") && !parsed["params"].is_null()) {
            message.request.params = parsed["params"];
        }
        return message;
    }
    if (!has_id && has_method) {
        if (!parsed["method"].is_string()) {
            out_error.code = kErrInvalidRequest;
            out_error.message = "method 必须是字符串";
            return std::nullopt;
        }
        message.kind = IncomingMessage::Kind::Notification;
        message.notification.method = parsed["method"].get<std::string>();
        if (parsed.contains("params") && !parsed["params"].is_null()) {
            message.notification.params = parsed["params"];
        }
        return message;
    }
    if (has_id && (has_result ^ has_error)) {
        // 前端对服务端反向请求(审批/ask_user,骨架期只留位)的答复。
        message.kind = IncomingMessage::Kind::Response;
        message.response.id = id;
        if (has_error) {
            message.response.is_error = true;
            message.response.result = parsed["error"];
        } else {
            message.response.result = parsed["result"];
        }
        return message;
    }

    out_error.code = kErrInvalidRequest;
    out_error.message = has_id ? "带 id 却没有 method/result/error" : "没有 method 也不是响应";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 参数表
// ---------------------------------------------------------------------------

ParamsCheck CheckParamsIsObject(const nlohmann::json& params, std::string_view method) {
    if (!params.is_object()) {
        return ParamsCheck{false, kErrInvalidParams, std::string(method) + ": params 必须是对象"};
    }
    return ParamsCheck{};
}

ParamsCheck CheckInitializeParams(const nlohmann::json& params) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodInitialize);
    if (!base.ok) {
        return base;
    }
    // clientName/clientVersion 可选字符串,给了但类型不对才报。
    for (const char* key : {"clientName", "clientVersion"}) {
        if (params.contains(key) && !params[key].is_null() && !params[key].is_string()) {
            return ParamsCheck{false, kErrInvalidParams, std::string(kMethodInitialize) + ": " + key + " 必须是字符串"};
        }
    }
    return ParamsCheck{};
}

ParamsCheck CheckThreadStartParams(const nlohmann::json& params) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodThreadStart);
    if (!base.ok) {
        return base;
    }
    if (params.contains("cwd") && !params["cwd"].is_null() && !params["cwd"].is_string()) {
        return ParamsCheck{false, kErrInvalidParams, std::string(kMethodThreadStart) + ": cwd 必须是字符串"};
    }
    return ParamsCheck{};
}

namespace {

// 取必填字符串字段。缺了/类型不对/空串都算参数错。
ParamsCheck RequireString(const nlohmann::json& params, std::string_view key, std::string_view method,
                          std::string& out_value) {
    if (!params.contains(key) || params[key].is_null()) {
        return ParamsCheck{false, kErrInvalidParams,
                           std::string(method) + ": 缺必填字段 " + std::string(key)};
    }
    if (!params[key].is_string()) {
        return ParamsCheck{false, kErrInvalidParams,
                           std::string(method) + ": " + std::string(key) + " 必须是字符串"};
    }
    std::string value = params[key].get<std::string>();
    if (value.empty()) {
        return ParamsCheck{false, kErrInvalidParams,
                           std::string(method) + ": " + std::string(key) + " 不许为空"};
    }
    out_value = std::move(value);
    return ParamsCheck{};
}

}  // namespace

ParamsCheck CheckTurnStartParams(const nlohmann::json& params, std::string& out_thread_id,
                                 std::string& out_text, std::vector<nlohmann::json>& out_images) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodTurnStart);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "threadId", kMethodTurnStart, out_thread_id);
    if (!check.ok) {
        return check;
    }
    check = RequireString(params, "text", kMethodTurnStart, out_text);
    if (!check.ok) {
        return check;
    }
    // images 可选:给了就必须是数组,元素须是对象且 mediaType/data 是
    // 字符串(宽松;宽高/filename 不在校验层卡,执行链自管)。
    if (params.contains("images") && !params["images"].is_null()) {
        if (!params["images"].is_array()) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodTurnStart) + ": images 必须是数组"};
        }
        for (const nlohmann::json& image : params["images"]) {
            if (!image.is_object() || !image.contains("mediaType") || !image["mediaType"].is_string() ||
                !image.contains("data") || !image["data"].is_string()) {
                return ParamsCheck{false, kErrInvalidParams,
                                   std::string(kMethodTurnStart) +
                                       ": images 元素须是带 mediaType/data 字符串的对象"};
            }
            out_images.push_back(image);
        }
    }
    return ParamsCheck{};
}

ParamsCheck CheckThreadStopParams(const nlohmann::json& params, std::string& out_thread_id) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodThreadStop);
    if (!base.ok) {
        return base;
    }
    return RequireString(params, "threadId", kMethodThreadStop, out_thread_id);
}

ParamsCheck CheckThreadLifecycleParams(const nlohmann::json& params, std::string& out_thread_id) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodThreadArchive);
    if (!base.ok) {
        return base;
    }
    return RequireString(params, "threadId", kMethodThreadArchive, out_thread_id);
}

ParamsCheck CheckTraceQueryParams(const nlohmann::json& params, std::string& out_thread_id,
                                   std::uint64_t& out_last_seq) {
    out_last_seq = 0; // 缺省全量
    const ParamsCheck base = CheckParamsIsObject(params, kMethodTraceQuery);
    if (!base.ok) {
        return base;
    }
    const ParamsCheck check = RequireString(params, "threadId", kMethodTraceQuery, out_thread_id);
    if (!check.ok) {
        return check;
    }
    // lastSeq 与 workflow/query 同口径:可选,给了须非负整数,缺省 0。
    if (params.contains("lastSeq") && !params["lastSeq"].is_null()) {
        const auto& value = params["lastSeq"];
        if ((value.is_number_integer() && value.get<std::int64_t>() >= 0) || value.is_number_unsigned()) {
            out_last_seq = static_cast<std::uint64_t>(value);
        } else {
            return ParamsCheck{false, kErrInvalidParams, "trace/query 的 lastSeq 须是非负整数"};
        }
    }
    // 可选过滤字段:类型不对报参数错,不静默忽略。
    for (const char* key : {"executionId", "toolUseId", "turnId"}) {
        if (params.contains(key) && !params[key].is_null() && !params[key].is_string()) {
            return ParamsCheck{false, kErrInvalidParams, std::string("trace/query 的 ") + key + " 须是字符串"};
        }
    }
    if (params.contains("errorsOnly") && !params["errorsOnly"].is_null() && !params["errorsOnly"].is_boolean()) {
        return ParamsCheck{false, kErrInvalidParams, "trace/query 的 errorsOnly 须是布尔"};
    }
    return ParamsCheck{true};
}

ParamsCheck CheckWorkflowQueryParams(const nlohmann::json& params, std::string& out_run_id,
                                     std::uint64_t& out_last_seq) {
    out_last_seq = 0; // 缺省全量;调用方传入的旧值不沿用
    const ParamsCheck base = CheckParamsIsObject(params, kMethodWorkflowQuery);
    if (!base.ok) {
        return base;
    }
    const ParamsCheck check = RequireString(params, "runId", kMethodWorkflowQuery, out_run_id);
    if (!check.ok) {
        return check;
    }
    // lastSeq 可选:给了须是非负整数(正数字面量在 nlohmann 里是
    // number_integer,显式 unsigned 才是 number_unsigned——两种都认,
    // 负数拒);缺省 0 = 全量事件。
    if (params.contains("lastSeq") && !params["lastSeq"].is_null()) {
        if (!params["lastSeq"].is_number_integer()) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodWorkflowQuery) + ": lastSeq 必须是非负整数"};
        }
        const std::int64_t value = params["lastSeq"].get<std::int64_t>();
        if (value < 0) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodWorkflowQuery) + ": lastSeq 必须是非负整数"};
        }
        out_last_seq = static_cast<std::uint64_t>(value);
    }
    return ParamsCheck{};
}

ParamsCheck CheckTurnInterruptParams(const nlohmann::json& params, std::string& out_thread_id,
                                     std::string& out_turn_id) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodTurnInterrupt);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "threadId", kMethodTurnInterrupt, out_thread_id);
    if (!check.ok) {
        return check;
    }
    // turnId 可选:给了但不是字符串才报;缺/空 = 打断该 thread 当前回合。
    if (params.contains("turnId") && !params["turnId"].is_null()) {
        if (!params["turnId"].is_string()) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodTurnInterrupt) + ": turnId 必须是字符串"};
        }
        out_turn_id = params["turnId"].get<std::string>();
    }
    return ParamsCheck{};
}

// ---------------------------------------------------------------------------
// 出站事件参数
// ---------------------------------------------------------------------------

nlohmann::json MakeThreadStartedParams(const std::string& thread_id, const std::string& cwd) {
    return nlohmann::json{{"threadId", thread_id}, {"cwd", cwd}};
}

nlohmann::json MakeThreadStoppedParams(const std::string& thread_id) {
    return nlohmann::json{{"threadId", thread_id}};
}

nlohmann::json MakeTurnStartedParams(const std::string& thread_id, const std::string& turn_id) {
    return nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}};
}

nlohmann::json MakeTurnCompletedParams(const std::string& thread_id, const std::string& turn_id,
                                       std::string_view status, const std::string& error_message,
                                       const nlohmann::json& usage, int steps_used) {
    nlohmann::json params = nlohmann::json{{"threadId", thread_id},
                                           {"turnId", turn_id},
                                           {"status", std::string(status)},
                                           {"usage", usage},
                                           {"stepsUsed", steps_used}};
    if (!error_message.empty()) {
        params["error"] = error_message;
    }
    return params;
}

nlohmann::json MakeItemStartedParams(const std::string& thread_id, const std::string& turn_id,
                                     const std::string& item_id, std::string_view item_type,
                                     nlohmann::json payload) {
    nlohmann::json item = nlohmann::json{{"id", item_id}, {"type", std::string(item_type)}};
    if (payload.is_object()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            item[it.key()] = it.value();
        }
    }
    return nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}, {"item", std::move(item)}};
}

nlohmann::json MakeItemDeltaParams(const std::string& thread_id, const std::string& turn_id,
                                   const std::string& item_id, std::string_view delta_text) {
    return nlohmann::json{{"threadId", thread_id},
                          {"turnId", turn_id},
                          {"itemId", item_id},
                          {"delta", std::string(delta_text)}};
}

nlohmann::json MakeItemCompletedParams(const std::string& thread_id, const std::string& turn_id,
                                       const std::string& item_id, nlohmann::json payload) {
    nlohmann::json item = nlohmann::json{{"id", item_id}};
    if (payload.is_object()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            item[it.key()] = it.value();
        }
    }
    return nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}, {"item", std::move(item)}};
}

nlohmann::json MakeQueueOverflowParams(const std::string& thread_id, const std::string& turn_id,
                                       std::uint64_t dropped, std::uint64_t coalesced) {
    return nlohmann::json{{"threadId", thread_id},
                          {"turnId", turn_id},
                          {"dropped", dropped},
                          {"coalesced", coalesced}};
}

nlohmann::json MakeTurnUsageParams(const std::string& thread_id, const std::string& turn_id,
                                   const nlohmann::json& usage, const std::string& model) {
    nlohmann::json params = nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}, {"usage", usage}};
    if (!model.empty()) {
        params["model"] = model;
    }
    return params;
}

nlohmann::json MakeTurnContextParams(const std::string& thread_id, const std::string& turn_id,
                                     const nlohmann::json& context) {
    return nlohmann::json{{"threadId", thread_id}, {"turnId", turn_id}, {"context", context}};
}

nlohmann::json MakeThreadLifecycleResult(const std::string& thread_id, const std::string& state) {
    nlohmann::json result = nlohmann::json{{"threadId", thread_id}};
    if (!state.empty()) {
        result["state"] = state;
    }
    return result;
}

// ---------------------------------------------------------------------------
// initialize / thread/list 结果
// ---------------------------------------------------------------------------

nlohmann::json MakeInitializeResult(std::string_view lubancode_version, std::string_view platform) {
    nlohmann::json capabilities = nlohmann::json::object();
    // 骨架期已接线的方法面如实报;留位的名字也列在 pending 里,前端能分清
    // "服务器认识但不接"与"压根没有"。
    capabilities["methods"] = std::vector<std::string>{
        std::string(kMethodInitialize),      std::string(kMethodInitialized),
        std::string(kMethodShutdown),        std::string(kMethodThreadStart),
        std::string(kMethodThreadList),      std::string(kMethodThreadStop),
        std::string(kMethodThreadArchive),   std::string(kMethodThreadUnarchive),
        std::string(kMethodThreadDelete),    std::string(kMethodTurnStart),
        std::string(kMethodTurnInterrupt),   std::string(kMethodWorkflowQuery),
        std::string(kMethodTraceQuery),
        // goal 单合流批:typed 命令面(goal 六 + loop 七 + plan 三)。
        std::string(kMethodGoalCreate),      std::string(kMethodGoalGet),
        std::string(kMethodGoalEdit),        std::string(kMethodGoalPause),
        std::string(kMethodGoalResume),      std::string(kMethodGoalClear),
        std::string(kMethodLoopCreate),      std::string(kMethodLoopList),
        std::string(kMethodLoopRead),        std::string(kMethodLoopPause),
        std::string(kMethodLoopResume),      std::string(kMethodLoopCancel),
        std::string(kMethodLoopRunNow),      std::string(kMethodPlanSetMode),
        std::string(kMethodPlanReview),      std::string(kMethodPlanReopen),
        // 浏览器调试工作台阶段 3:browser 面(方法 18 枚,事件 13 族)。
        std::string(kMethodBrowserStart),
        std::string(kMethodBrowserStop),
        std::string(kMethodBrowserStatus),
        std::string(kMethodBrowserPageOpen),
        std::string(kMethodBrowserPageList),
        std::string(kMethodBrowserPageSelect),
        std::string(kMethodBrowserPageClose),
        std::string(kMethodBrowserPageNavigate),
        std::string(kMethodBrowserPageBack),
        std::string(kMethodBrowserPageForward),
        std::string(kMethodBrowserPageReload),
        std::string(kMethodBrowserSnapshot),
        std::string(kMethodBrowserScreenshot),
        std::string(kMethodBrowserAction),
        std::string(kMethodBrowserActionCancel),
        std::string(kMethodBrowserConsoleQuery),
        std::string(kMethodBrowserNetworkQuery),
        std::string(kMethodBrowserDownloadsQuery)};
    capabilities["pending"] = std::vector<std::string>{
        std::string(kMethodThreadResume),    std::string(kMethodThreadRead),
        std::string(kMethodTurnSteer),       std::string(kMethodModelList),
        std::string(kMethodConfigRead),      std::string(kMethodWorkflowList)};
    // 审批与 ask_user 的反向请求:协议位占住,执行链等 Broker(另一条线)。
    capabilities["serverRequests"] = std::vector<std::string>{std::string(kMethodPermissionRequest),
                                                              std::string(kMethodUserAsk)};
    // 事件账里给审批/打断/diff 留的类型与终态,一并报出去,前端画界面
    // 好留坑。
    capabilities["itemTypes"] = std::vector<std::string>{
        std::string(kItemTypeText),      std::string(kItemTypeThinking),   std::string(kItemTypeTool),
        std::string(kItemTypeCommand),   std::string(kItemTypeFileChange), std::string(kItemTypeQuestion),
        std::string(kItemTypeAgent),     std::string(kItemTypeError)};
    capabilities["turnStatuses"] = std::vector<std::string>{
        std::string(kTurnStatusSuccess), std::string(kTurnStatusError), std::string(kTurnStatusCancelled),
        std::string(kTurnStatusInterrupted), std::string(kTurnStatusRejected)};

    return nlohmann::json{{"protocolVersion", std::string(kProtocolVersion)},
                          {"lubancodeVersion", std::string(lubancode_version)},
                          {"platform", std::string(platform)},
                          {"capabilities", std::move(capabilities)}};
}

nlohmann::json MakeThreadListResult(const std::vector<nlohmann::json>& entries) {
    return nlohmann::json{{"threads", entries}};
}

nlohmann::json MakeThreadStoppedResult() {
    return nlohmann::json::object();
}

// ---------------------------------------------------------------------------
// goal/loop/plan 参数表(goal 单合流批)
// ---------------------------------------------------------------------------

ParamsCheck CheckGoalMutationParams(const nlohmann::json& params, std::string_view method,
                                    std::string& out_thread_id, std::string& out_text) {
    const ParamsCheck base = CheckParamsIsObject(params, method);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "threadId", method, out_thread_id);
    if (!check.ok) {
        return check;
    }
    if (method == kMethodGoalCreate || method == kMethodGoalEdit) {
        // objective 正文必填(create/edit 同口径;空串在 RequireString 拒)。
        check = RequireString(params, "text", method, out_text);
        if (!check.ok) {
            return check;
        }
        if (params.contains("expectedRevision") && !params["expectedRevision"].is_null()) {
            if (!params["expectedRevision"].is_number_integer() ||
                params["expectedRevision"].get<std::int64_t>() < 0) {
                return ParamsCheck{false, kErrInvalidParams,
                                   std::string(method) + ": expectedRevision 必须是非负整数"};
            }
        }
    }
    return ParamsCheck{};
}

ParamsCheck CheckLoopMutationParams(const nlohmann::json& params, std::string_view method,
                                    std::string& out_thread_id, std::string& out_task_id,
                                    std::string& out_text) {
    const ParamsCheck base = CheckParamsIsObject(params, method);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "threadId", method, out_thread_id);
    if (!check.ok) {
        return check;
    }
    if (method == kMethodLoopCreate) {
        // text(prompt)可选:空 = loop.md/内置源(泵每拍现读)。
        if (params.contains("text") && !params["text"].is_null()) {
            if (!params["text"].is_string()) {
                return ParamsCheck{false, kErrInvalidParams, std::string(method) + ": text 必须是字符串"};
            }
            out_text = params["text"].get<std::string>();
        }
        if (params.contains("intervalMs") && !params["intervalMs"].is_null()) {
            if (!params["intervalMs"].is_number_integer() ||
                params["intervalMs"].get<std::int64_t>() < 0) {
                return ParamsCheck{false, kErrInvalidParams,
                                   std::string(method) + ": intervalMs 必须是非负整数"};
            }
        }
        return ParamsCheck{};
    }
    if (method == kMethodLoopList) {
        return ParamsCheck{};  // 只查 threadId
    }
    // read/pause/resume/cancel/run:taskId 必填。
    return RequireString(params, "taskId", method, out_task_id);
}

ParamsCheck CheckPlanMutationParams(const nlohmann::json& params, std::string_view method,
                                    std::string& out_thread_id) {
    const ParamsCheck base = CheckParamsIsObject(params, method);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "threadId", method, out_thread_id);
    if (!check.ok) {
        return check;
    }
    if (method == kMethodPlanSetMode) {
        std::string mode;
        check = RequireString(params, "mode", method, mode);
        if (!check.ok) {
            return check;
        }
        if (mode != "plan" && mode != "default") {
            return ParamsCheck{false, kErrInvalidParams, std::string(method) + ": mode 只认 plan/default"};
        }
        return ParamsCheck{};
    }
    if (method == kMethodPlanReview) {
        std::string plan_id;
        check = RequireString(params, "planId", method, plan_id);
        if (!check.ok) {
            return check;
        }
        if (!params.contains("planRevision") || !params["planRevision"].is_number_integer() ||
            params["planRevision"].get<std::int64_t>() < 1) {
            return ParamsCheck{false, kErrInvalidParams, std::string(method) + ": planRevision 必须是正整数"};
        }
        std::string sha;
        check = RequireString(params, "sha256", method, sha);
        if (!check.ok) {
            return check;
        }
        std::string decision;
        check = RequireString(params, "decision", method, decision);
        if (!check.ok) {
            return check;
        }
        if (decision != "approved_confirm" && decision != "approved_auto" && decision != "rejected" &&
            decision != "continued") {
            return ParamsCheck{
                false, kErrInvalidParams,
                std::string(method) + ": decision 只认 approved_confirm/approved_auto/rejected/continued"};
        }
    }
    return ParamsCheck{};  // reopen 只查 threadId
}

// ---------------------------------------------------------------------------
// browser 参数表(阶段 3):见 schema.hpp 的分档说明。
// ---------------------------------------------------------------------------

namespace {

// 可选正整数字段:给了须 >= min。
ParamsCheck OptionalPositiveInt(const nlohmann::json& params, const char* key, std::string_view method,
                                std::int64_t min = 1) {
    if (!params.contains(key) || params[key].is_null()) {
        return ParamsCheck{};
    }
    if (!params[key].is_number_integer()) {
        return ParamsCheck{false, kErrInvalidParams,
                           std::string(method) + ": " + key + " 必须是整数(>= " + std::to_string(min) + ")"};
    }
    const std::int64_t value = params[key].get<std::int64_t>();
    if (value < min) {
        return ParamsCheck{false, kErrInvalidParams,
                           std::string(method) + ": " + key + " 必须 >= " + std::to_string(min)};
    }
    return ParamsCheck{};
}

}  // namespace

ParamsCheck CheckBrowserStartParams(const nlohmann::json& params) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodBrowserStart);
    if (!base.ok) {
        return base;
    }
    if (params.contains("engine") && !params["engine"].is_null()) {
        const std::string engine = params.value("engine", std::string());
        if (engine != "chromium" && engine != "webkit") {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodBrowserStart) + ": engine 只认 chromium|webkit"};
        }
    }
    if (params.contains("headed") && !params["headed"].is_null() && !params["headed"].is_boolean()) {
        return ParamsCheck{false, kErrInvalidParams, std::string(kMethodBrowserStart) + ": headed 必须是布尔"};
    }
    if (params.contains("profile") && !params["profile"].is_null()) {
        const std::string profile = params.value("profile", std::string());
        if (profile != "persistent" && profile != "ephemeral") {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodBrowserStart) + ": profile 只认 persistent|ephemeral"};
        }
    }
    if (params.contains("viewport") && !params["viewport"].is_null()) {
        const nlohmann::json& viewport = params["viewport"];
        if (!viewport.is_object() || !viewport.contains("width") || !viewport.contains("height") ||
            !viewport["width"].is_number_integer() || !viewport["height"].is_number_integer() ||
            viewport["width"].get<std::int64_t>() < 1 || viewport["height"].get<std::int64_t>() < 1) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodBrowserStart) + ": viewport 须是 {width,height} 正整数"};
        }
    }
    ParamsCheck check = OptionalPositiveInt(params, "journalCap", kMethodBrowserStart);
    if (!check.ok) {
        return check;
    }
    return OptionalPositiveInt(params, "timeoutMs", kMethodBrowserStart);
}

ParamsCheck CheckBrowserStopParams(const nlohmann::json& params) {
    return CheckParamsIsObject(params, kMethodBrowserStop);
}

ParamsCheck CheckBrowserPageOpenParams(const nlohmann::json& params, std::string& out_url) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodBrowserPageOpen);
    if (!base.ok) {
        return base;
    }
    return RequireString(params, "url", kMethodBrowserPageOpen, out_url);
}

ParamsCheck CheckBrowserPageNavigateParams(const nlohmann::json& params, std::string& out_page_id,
                                           std::string& out_url) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodBrowserPageNavigate);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "pageId", kMethodBrowserPageNavigate, out_page_id);
    if (!check.ok) {
        return check;
    }
    return RequireString(params, "url", kMethodBrowserPageNavigate, out_url);
}

ParamsCheck CheckBrowserPageTargetParams(const nlohmann::json& params, std::string_view method,
                                         std::string& out_page_id) {
    const ParamsCheck base = CheckParamsIsObject(params, method);
    if (!base.ok) {
        return base;
    }
    return RequireString(params, "pageId", method, out_page_id);
}

ParamsCheck CheckBrowserJournalQueryParams(const nlohmann::json& params, std::string_view method,
                                           std::string& out_page_id, std::uint64_t& out_since_seq) {
    out_since_seq = 0; // 缺省全量
    const ParamsCheck base = CheckParamsIsObject(params, method);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "pageId", method, out_page_id);
    if (!check.ok) {
        return check;
    }
    if (params.contains("sinceSeq") && !params["sinceSeq"].is_null()) {
        if (!params["sinceSeq"].is_number_integer() || params["sinceSeq"].get<std::int64_t>() < 0) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(method) + ": sinceSeq 必须是非负整数(断线补账的 cursor)"};
        }
        out_since_seq = static_cast<std::uint64_t>(params["sinceSeq"].get<std::int64_t>());
    }
    return OptionalPositiveInt(params, "limit", method, 1);
}

ParamsCheck CheckBrowserActionParams(const nlohmann::json& params, std::string& out_kind) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodBrowserAction);
    if (!base.ok) {
        return base;
    }
    ParamsCheck check = RequireString(params, "kind", kMethodBrowserAction, out_kind);
    if (!check.ok) {
        return check;
    }
    if (out_kind != "click" && out_kind != "type" && out_kind != "select" && out_kind != "wait") {
        return ParamsCheck{false, kErrInvalidParams,
                           std::string(kMethodBrowserAction) + ": kind 只认 click|type|select|wait,收到: " +
                               out_kind};
    }
    if (out_kind != "wait") {
        std::string ref;
        check = RequireString(params, "ref", kMethodBrowserAction, ref);
        if (!check.ok) {
            return check;
        }
    }
    if (out_kind == "type") {
        std::string text;
        check = RequireString(params, "text", kMethodBrowserAction, text);
        if (!check.ok) {
            return check;
        }
    }
    if (out_kind == "wait") {
        const bool has_for_text = params.contains("forText") && params["forText"].is_string();
        const bool has_url = params.contains("urlContains") && params["urlContains"].is_string();
        const bool has_ms = params.contains("ms") && params["ms"].is_number_integer();
        if (!has_for_text && !has_url && !has_ms) {
            return ParamsCheck{false, kErrInvalidParams,
                               std::string(kMethodBrowserAction) + ": wait 须给 forText/urlContains/ms 至少一样"};
        }
    }
    return ParamsCheck{};
}

ParamsCheck CheckBrowserActionCancelParams(const nlohmann::json& params, std::string& out_action_id) {
    const ParamsCheck base = CheckParamsIsObject(params, kMethodBrowserActionCancel);
    if (!base.ok) {
        return base;
    }
    return RequireString(params, "actionId", kMethodBrowserActionCancel, out_action_id);
}

}  // namespace lubancode::app_server
