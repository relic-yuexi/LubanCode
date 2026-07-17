#include "mcp/client.hpp"

#include <chrono>

namespace lubancode::mcp {

namespace {

constexpr int kDefaultTimeoutMs = 30000;
constexpr int kToolCallTimeoutMs = 120000;
constexpr char kProtocolVersion[] = "2024-11-05";
constexpr char kClientVersion[] = "0.9.0";

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
    return owned_transport_->Start(command, args, env, [this](std::string line) { OnLine(std::move(line)); });
}

void Client::AttachTransportForTest(Transport* transport) {
    owned_transport_.reset();
    transport_ = transport;
}

void Client::OnLine(const std::string& line) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error&) {
        // 非法 JSON 行,忽略——不是我们能处理的东西,不该把整个客户端搞崩。
        return;
    }

    if (!parsed.is_object() || !parsed.contains("id") || parsed["id"].is_null()) {
        // 没有 id 的消息(服务器主动发的通知之类),协议层眼下用不上,忽略。
        return;
    }

    const std::int64_t id = parsed["id"].get<std::int64_t>();

    std::shared_ptr<PendingEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;  // 不认识的 id(可能是超时后迟到的响应),丢掉。
        }
        entry = it->second;
        entry->response = std::move(parsed);
        entry->done = true;
    }
    cv_.notify_all();
}

std::expected<nlohmann::json, std::string> Client::SendRequestAndWait(const std::string& method,
                                                                       const nlohmann::json& params,
                                                                       int timeout_ms) {
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

    std::unique_lock<std::mutex> lock(mutex_);
    const bool woke = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&entry, this]() {
        return entry->done || !transport_->IsAlive();
    });
    pending_.erase(id);

    if (!entry->done) {
        if (!woke || !transport_->IsAlive()) {
            const std::string stderr_tail = transport_->StderrTail();
            std::string message = "MCP 服务器 " + server_name_ + " 在等待 " + method + " 响应时";
            message += transport_->IsAlive() ? "超时" : "进程已退出";
            if (!stderr_tail.empty()) {
                message += "(stderr: " + stderr_tail + ")";
            }
            return std::unexpected(message);
        }
        return std::unexpected("MCP 服务器 " + server_name_ + " 等待 " + method + " 响应异常中止");
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
        return std::unexpected("MCP 服务器 " + server_name_ + " 响应缺少 result 字段: " + method);
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
    const nlohmann::json params = {{"protocolVersion", kProtocolVersion},
                                    {"capabilities", nlohmann::json::object()},
                                    {"clientInfo", {{"name", "lubancode"}, {"version", kClientVersion}}}};
    auto result = SendRequestAndWait("initialize", params, default_timeout_ms_);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
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
        if (!info.name.empty()) {
            tools.push_back(std::move(info));
        }
    }
    return tools;
}

tools::Tool::Result Client::CallTool(const std::string& tool_name, const nlohmann::json& arguments) {
    const nlohmann::json params = {{"name", tool_name}, {"arguments", arguments}};
    auto result = SendRequestAndWait("tools/call", params, tool_call_timeout_ms_);
    if (!result.has_value()) {
        return tools::Tool::Result{result.error(), true};
    }

    const nlohmann::json& value = *result;
    const bool is_error = value.value("isError", false);

    std::string text;
    if (value.contains("content") && value["content"].is_array()) {
        for (const auto& block : value["content"]) {
            const std::string type = block.value("type", std::string());
            if (type == "text") {
                text += block.value("text", std::string());
            } else {
                text += "[不支持的内容类型: " + (type.empty() ? std::string("未知") : type) + "]";
            }
        }
    }

    return tools::Tool::Result{text, is_error};
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
