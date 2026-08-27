#include "mcp/client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "mcp/rich_result.hpp"

namespace lubancode::mcp {

namespace {

constexpr int kDefaultTimeoutMs = 30000;
constexpr int kToolCallTimeoutMs = 120000;
constexpr char kClientVersion[] = "0.9.0";

// 协议版本协商账(MCP 富结果单 P0.4):客户端认得的版本,降序;请求里
// 永远发最高版,响应版本必须落在表内——不认识的未来版明败,不硬着头皮
// 说方言。2024-11-05 在表尾,旧 server(只会回自己的版本)照常工作。
constexpr const char* kSupportedProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

bool IsSupportedProtocolVersion(const std::string& version) {
    for (const char* supported : kSupportedProtocolVersions) {
        if (version == supported) {
            return true;
        }
    }
    return false;
}

}  // namespace

Client::Client(std::string server_name)
    : server_name_(std::move(server_name)),
      default_timeout_ms_(kDefaultTimeoutMs),
      tool_call_timeout_ms_(kToolCallTimeoutMs) {}

void Client::SetTimeoutsForTest(int default_timeout_ms, int tool_call_timeout_ms) {
    default_timeout_ms_ = default_timeout_ms;
    tool_call_timeout_ms_ = tool_call_timeout_ms;
}

Client::~Client() {
    Shutdown();
}

TransportStartResult Client::StartProcess(const std::string& command, const std::vector<std::string>& args,
                                           const std::vector<std::pair<std::string, std::string>>& env) {
    owned_transport_ = std::make_unique<StdioTransportAdapter>();
    transport_ = owned_transport_.get();
    ++transport_generation_;  // 逐枚追踪单:换一代记一笔(重启/换进程分得清)
    return owned_transport_->Start(command, args, env, [this](std::string line) { OnLine(std::move(line)); });
}

void Client::AttachTransportForTest(Transport* transport) {
    owned_transport_.reset();
    transport_ = transport;
    ++transport_generation_;
}

std::uint64_t Client::transport_generation() const {
    return transport_generation_;
}

void Client::OnLine(const std::string& line) {
    // 这个回调跑在传输层的读线程上——任何异常漏出去都没人接,整个进程直接
    // std::terminate。所以整个函数体包在 try/catch 里,坏消息一律丢弃。
    // 用 json::exception 基类兜底:parse_error(整行不是 JSON)和 type_error
    // (字段存在但类型不对,比如 id 是字符串)两种都接得住。
    try {
        nlohmann::json parsed = nlohmann::json::parse(line);

        if (!parsed.is_object() || !parsed.contains("id") || !parsed["id"].is_number_integer()) {
            // 没有 id 的消息(服务器主动发的通知之类)、或者 id 类型不对,
            // 协议层眼下用不上,忽略。
            return;
        }

        const std::int64_t id = parsed["id"].get<std::int64_t>();

        std::shared_ptr<PendingEntry> entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(id);
            if (it == pending_.end()) {
                // 迟到响应:超时删过 pending,响应现在才到。丢弃但留账
                // (逐枚追踪单:另记 late_response_dropped,关联原请求,
                // 不能投给新调用)——迟丢账走回调,宿主翻成 trace 事件。
                if (late_response_sink_) {
                    late_response_sink_(id);
                }
                return;
            }
            entry = it->second;
            entry->response = std::move(parsed);
            entry->done = true;
        }
        cv_.notify_all();
    } catch (const nlohmann::json::exception&) {
        // 非法 JSON 行/字段类型不对,忽略——不是我们能处理的东西,不该把
        // 整个客户端(乃至整个进程)搞崩。
        return;
    }
}

std::expected<nlohmann::json, std::string> Client::SendRequestAndWait(const std::string& method,
                                                                       const nlohmann::json& params, int timeout_ms,
                                                                       std::int64_t* jsonrpc_request_id_out) {
    if (transport_ == nullptr) {
        return std::unexpected("MCP 服务器 " + server_name_ + " 传输层未就绪");
    }

    std::int64_t id;
    auto entry = std::make_shared<PendingEntry>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
        pending_[id] = entry;
    }

    nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    if (!transport_->WriteLine(request.dump())) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
        return std::unexpected("MCP 服务器 " + server_name_ + " 写入请求失败(进程可能已退出): " + method);
    }

    // 分段等待(每段 100ms)轮询进程死活:进程死了没人来 notify 这个 cv,
    // 一口气 wait_for 满 timeout_ms 的话,tools/call 会白等上 120s 才发现
    // 对面早就没了。分段醒来查一次 IsAlive,死进程几百毫秒内就能失败返回。
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!entry->done && transport_->IsAlive()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto slice = std::min<std::chrono::steady_clock::duration>(deadline - now, std::chrono::milliseconds(100));
        cv_.wait_for(lock, slice, [&entry]() { return entry->done; });
    }
    pending_.erase(id);

    if (!entry->done) {
        const std::string stderr_tail = transport_->StderrTail();
        std::string message = "MCP 服务器 " + server_name_ + " 在等待 " + method + " 响应时";
        message += transport_->IsAlive() ? "超时" : "进程已退出";
        if (!stderr_tail.empty()) {
            message += "(stderr: " + stderr_tail + ")";
        }
        return std::unexpected(message);
    }

    const nlohmann::json& response = entry->response;
    if (response.contains("error") && !response["error"].is_null()) {
        std::string error_message = "MCP 服务器 " + server_name_ + " 返回错误: ";
        const auto& error = response["error"];
        if (error.contains("message") && error["message"].is_string()) {
            error_message += error["message"].get<std::string>();
        } else {
            error_message += error.dump();
        }
        return std::unexpected(error_message);
    }

    if (!response.contains("result")) {
        if (jsonrpc_request_id_out != nullptr) {
            *jsonrpc_request_id_out = id;
        }
        return std::unexpected("MCP 服务器 " + server_name_ + " 响应缺少 result 字段: " + method);
    }
    if (jsonrpc_request_id_out != nullptr) {
        *jsonrpc_request_id_out = id;
    }
    return response["result"];
}

bool Client::SendNotification(const std::string& method, const nlohmann::json& params) {
    if (transport_ == nullptr) {
        return false;
    }
    nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    return transport_->WriteLine(notification.dump());
}

std::expected<void, std::string> Client::Initialize() {
    const nlohmann::json params = {{"protocolVersion", kSupportedProtocolVersions[0]},
                                    {"capabilities", nlohmann::json::object()},
                                    {"clientInfo", {{"name", "lubancode"}, {"version", kClientVersion}}}};
    auto result = SendRequestAndWait("initialize", params, default_timeout_ms_);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
    // 版本协商:响应里必须带认得的 protocolVersion。旧 server 回
    // 2024-11-05 照常工作;没带/带认不得的版本,明说不兼容,不硬聊。
    if (!result->contains("protocolVersion") || !(*result)["protocolVersion"].is_string()) {
        return std::unexpected("MCP 服务器 " + server_name_ +
                               " 的 initialize 响应缺 protocolVersion 字段,无法协商协议版本");
    }
    const std::string server_version = (*result)["protocolVersion"].get<std::string>();
    if (!IsSupportedProtocolVersion(server_version)) {
        return std::unexpected("MCP 服务器 " + server_name_ + " 只支持协议版本 " + server_version +
                               ",LubanCode 认得的版本是 2025-06-18/2025-03-26/2024-11-05,无法握手");
    }
    negotiated_protocol_version_ = server_version;
    SendNotification("notifications/initialized", nlohmann::json::object());
    return {};
}

std::expected<std::vector<ToolInfo>, std::string> Client::ListTools() {
    auto result = SendRequestAndWait("tools/list", nlohmann::json::object(), default_timeout_ms_);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    const nlohmann::json& value = *result;
    if (!value.contains("tools") || !value["tools"].is_array()) {
        return std::unexpected("MCP 服务器 " + server_name_ + " 的 tools/list 响应里没有 tools 数组");
    }

    // .value() 在字段存在但类型不对时抛 type_error,不能让它穿透出去。
    try {
        std::vector<ToolInfo> tools;
        for (const auto& item : value["tools"]) {
            ToolInfo info;
            info.name = item.value("name", std::string());
            info.description = item.value("description", std::string());
            if (item.contains("inputSchema")) {
                info.input_schema = item["inputSchema"];
            } else {
                info.input_schema = nlohmann::json::object();
            }
            // MCP 富结果单 P0.4:title/outputSchema/annotations 接住;
            // 类型不对的按缺省处理,不因诊断字段坏一颗弃整件工具。
            info.title = item.value("title", std::string());
            if (item.contains("outputSchema") && item["outputSchema"].is_object()) {
                info.output_schema = item["outputSchema"];
            }
            if (item.contains("annotations") && item["annotations"].is_object()) {
                info.annotations = item["annotations"];
            }
            if (!info.name.empty()) {
                tools.push_back(std::move(info));
            }
        }
        return tools;
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected("MCP 服务器 " + server_name_ + " 的 tools/list 响应字段类型不对: " + e.what());
    }
}

tools::Tool::Result Client::CallTool(const std::string& tool_name, const nlohmann::json& arguments,
                                     std::int64_t* jsonrpc_request_id_out, const CallOptions& options) {
    const nlohmann::json params = {{"name", tool_name}, {"arguments", arguments}};
    auto result = SendRequestAndWait("tools/call", params, tool_call_timeout_ms_, jsonrpc_request_id_out);
    if (!result.has_value()) {
        // 逐枚追踪单:server 进程退出/传输断、超时分开记,不靠中文正文
        // 分辨。迟到响应的丢弃在 transport 读线程里(HookRunRecord 之外
        // 另记 late_response_dropped,见 client.hpp 注释)。
        tools::Tool::Result failed{result.error(), true};
        if (result.error().find("超时") != std::string::npos) {
            failed.outcome = "timed_out";
            failed.error_code = "mcp.timeout";
        } else {
            failed.outcome = "transport_error";
            failed.error_code = "mcp.transport_closed";
        }
        return failed;
    }

    // 富结果整链解析(mcp/rich_result.cpp):坏块/坏 base64/伪 MIME/超帽
    // 全按稳定协议错收口;未知 content type 留占位块与码,不吞不崩。
    // CallTool 承诺不抛异常,兜底 catch 翻成 is_error 结果。
    try {
        CallToolParseContext context;
        context.server_name = server_name_;
        context.artifact_dir = options.artifact_dir;
        context.binary_budget =
            landed_artifact_bytes_ >= CallToolParseContext::kSessionArtifactCap
                ? 0
                : std::min<std::size_t>(kMaxCallBinaryBytes,
                                        CallToolParseContext::kSessionArtifactCap - landed_artifact_bytes_);
        if (options.output_schema != nullptr) {
            context.output_schema = *options.output_schema;
        }
        CallToolParseResult parsed = ParseCallToolResult(*result, context);
        if (parsed.protocol_error) {
            tools::Tool::Result failed{parsed.error_message, true};
            failed.outcome = "protocol_error";
            failed.error_code = parsed.error_code;
            if (parsed.details.is_object() && !parsed.details.empty()) {
                failed.details = parsed.details;
            }
            return failed;
        }
        landed_artifact_bytes_ += parsed.landed_bytes;
        tools::Tool::Result out = tools::Tool::Result::FromPayload(std::move(parsed.payload),
                                                                   parsed.server_is_error);
        if (parsed.server_is_error) {
            out.outcome = "tool_error";
        }
        if (parsed.details.is_object() && !parsed.details.empty()) {
            out.details = parsed.details;
        }
        return out;
    } catch (const nlohmann::json::exception& e) {
        tools::Tool::Result failed{
            "MCP 服务器 " + server_name_ + " 的 tools/call 响应字段类型不对: " + e.what(), true};
        failed.outcome = "protocol_error";
        failed.error_code = "mcp.malformed_content";
        return failed;
    }
}

void Client::Shutdown() {
    if (transport_ != nullptr) {
        transport_->Shutdown(2000);
    }
}

bool Client::Alive() const {
    return transport_ != nullptr && transport_->IsAlive();
}

std::string Client::StderrTail() const {
    return transport_ != nullptr ? transport_->StderrTail() : std::string();
}

}  // namespace lubancode::mcp
