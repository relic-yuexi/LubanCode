#include "lsp/client.hpp"

#include <chrono>

namespace lubancode::lsp {

namespace {

constexpr int kDefaultRequestTimeoutMs = 15000;

// URI 转义:除了字母数字和少数安全字符,其余字节一律 %XX。'/' 和 ':' 留着
// (路径分隔符和盘符冒号,file URI 里就该原样出现)。
bool IsUriSafeChar(unsigned char c) {
    if (std::isalnum(c) != 0) {
        return true;
    }
    switch (c) {
        case '-':
        case '.':
        case '_':
        case '~':
        case '/':
        case ':':
            return true;
        default:
            return false;
    }
}

char HexDigit(int v) {
    return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10));
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string PathToUri(const std::string& path_utf8) {
    std::string normalized = path_utf8;
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    std::string encoded;
    encoded.reserve(normalized.size());
    for (const char c : normalized) {
        if (IsUriSafeChar(static_cast<unsigned char>(c))) {
            encoded.push_back(c);
        } else {
            const auto b = static_cast<unsigned char>(c);
            encoded.push_back('%');
            encoded.push_back(HexDigit(b >> 4));
            encoded.push_back(HexDigit(b & 0x0F));
        }
    }
    // Windows 盘符路径("D:/..."):file:///D:/...;POSIX 绝对路径("/home/...")
    // 本身带头一个斜杠:file:///home/...。
    if (!encoded.empty() && encoded.front() == '/') {
        return "file://" + encoded;
    }
    return "file:///" + encoded;
}

std::string UriToPath(const std::string& uri) {
    std::string rest = uri;
    const std::string scheme = "file://";
    if (rest.compare(0, scheme.size(), scheme) == 0) {
        rest.erase(0, scheme.size());
    }
    // 百分号解码。
    std::string decoded;
    decoded.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const int hi = HexValue(rest[i + 1]);
            const int lo = HexValue(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(rest[i]);
    }
    // "/D:/path" 这种(Windows 盘符前多一个斜杠)把头上的斜杠去掉。
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) != 0 &&
        decoded[2] == ':') {
        decoded.erase(0, 1);
    }
#ifdef _WIN32
    for (char& c : decoded) {
        if (c == '/') {
            c = '\\';
        }
    }
#endif
    return decoded;
}

Client::Client(std::string name) : name_(std::move(name)), request_timeout_ms_(kDefaultRequestTimeoutMs) {}

Client::~Client() {
    Shutdown();
}

void Client::SetTimeoutForTest(int request_timeout_ms) {
    request_timeout_ms_ = request_timeout_ms;
}

TransportStartResult Client::StartProcess(const std::string& command, const std::vector<std::string>& args) {
    owned_transport_ = std::make_unique<StdioTransportAdapter>();
    transport_ = owned_transport_.get();
    return owned_transport_->Start(command, args, [this](std::string body) { OnMessage(body); });
}

void Client::AttachTransportForTest(Transport* transport) {
    owned_transport_.reset();
    transport_ = transport;
}

void Client::OnMessage(const std::string& body) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return;  // 非法 JSON,不是我们能处理的东西,不该把整个客户端搞崩
    }
    if (!parsed.is_object()) {
        return;
    }

    const bool has_method = parsed.contains("method") && parsed["method"].is_string();
    const bool has_id = parsed.contains("id") && !parsed["id"].is_null();

    if (has_method && has_id) {
        // 服务器发来的反向请求(clangd 的 window/workDoneProgress/create、
        // workspace/configuration 之类)——统一回 result=null 的空响应,
        // 免得服务器干等;这些能力我们的最小 capabilities 本就没声明,
        // 空回应是安全的。
        if (transport_ != nullptr) {
            const nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", parsed["id"]}, {"result", nullptr}};
            transport_->WriteMessage(reply.dump());
        }
        return;
    }

    if (has_method) {
        // 通知:只认 publishDiagnostics,存进缓存;别的忽略。
        if (parsed["method"].get<std::string>() == "textDocument/publishDiagnostics" && parsed.contains("params") &&
            parsed["params"].is_object()) {
            const auto& params = parsed["params"];
            if (params.contains("uri") && params["uri"].is_string()) {
                const std::string uri = params["uri"].get<std::string>();
                nlohmann::json diags = nlohmann::json::array();
                if (params.contains("diagnostics") && params["diagnostics"].is_array()) {
                    diags = params["diagnostics"];
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    diagnostics_[uri] = std::move(diags);
                }
                cv_.notify_all();
            }
        }
        return;
    }

    if (!has_id || !parsed["id"].is_number_integer()) {
        return;
    }
    const std::int64_t id = parsed["id"].get<std::int64_t>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;  // 不认识的 id(可能是超时后迟到的响应),丢掉
        }
        it->second->response = std::move(parsed);
        it->second->done = true;
    }
    cv_.notify_all();
}

std::expected<nlohmann::json, std::string> Client::SendRequestAndWait(const std::string& method,
                                                                        const nlohmann::json& params) {
    if (transport_ == nullptr) {
        return std::unexpected("LSP 服务器 " + name_ + " 传输层未就绪");
    }

    std::int64_t id;
    auto entry = std::make_shared<PendingEntry>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
        pending_[id] = entry;
    }

    nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    if (!transport_->WriteMessage(request.dump())) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
        return std::unexpected("LSP 服务器 " + name_ + " 写入请求失败(进程可能已退出): " + method);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool woke = cv_.wait_for(lock, std::chrono::milliseconds(request_timeout_ms_), [&entry, this]() {
        return entry->done || !transport_->IsAlive();
    });
    pending_.erase(id);

    if (!entry->done) {
        if (!woke || !transport_->IsAlive()) {
            const std::string stderr_tail = transport_->StderrTail();
            std::string message = "LSP 服务器 " + name_ + " 在等待 " + method + " 响应时";
            message += transport_->IsAlive() ? "超时" : "进程已退出";
            if (!stderr_tail.empty()) {
                message += "(stderr: " + stderr_tail + ")";
            }
            return std::unexpected(message);
        }
        return std::unexpected("LSP 服务器 " + name_ + " 等待 " + method + " 响应异常中止");
    }

    const nlohmann::json& response = entry->response;
    if (response.contains("error") && !response["error"].is_null()) {
        std::string error_message = "LSP 服务器 " + name_ + " 返回错误: ";
        const auto& error = response["error"];
        if (error.contains("message") && error["message"].is_string()) {
            error_message += error["message"].get<std::string>();
        } else {
            error_message += error.dump();
        }
        return std::unexpected(error_message);
    }

    if (!response.contains("result")) {
        return std::unexpected("LSP 服务器 " + name_ + " 响应缺少 result 字段: " + method);
    }
    return response["result"];
}

bool Client::SendNotification(const std::string& method, const nlohmann::json& params) {
    if (transport_ == nullptr) {
        return false;
    }
    nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    return transport_->WriteMessage(notification.dump());
}

std::expected<void, std::string> Client::Initialize(const std::string& root_uri) {
    // capabilities 保持最小:定位类请求和 publishDiagnostics 都是服务器
    // 天生会做的事,不用声明花哨能力;声明得越少,服务器要我们配合的
    // 反向请求也越少。
    const nlohmann::json params = {{"processId", nullptr},
                                    {"rootUri", root_uri},
                                    {"capabilities", nlohmann::json::object()},
                                    {"clientInfo", {{"name", "lubancode"}}}};
    auto result = SendRequestAndWait("initialize", params);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
    SendNotification("initialized", nlohmann::json::object());
    return {};
}

void Client::EnsureDidOpen(const std::string& uri, const std::string& language_id, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_uris_.insert(uri).second) {
            return;  // 已经开过,不重复发
        }
    }
    const nlohmann::json params = {
        {"textDocument", {{"uri", uri}, {"languageId", language_id}, {"version", 1}, {"text", text}}}};
    SendNotification("textDocument/didOpen", params);
}

std::expected<nlohmann::json, std::string> Client::Definition(const std::string& uri, int line, int character) {
    const nlohmann::json params = {{"textDocument", {{"uri", uri}}},
                                    {"position", {{"line", line}, {"character", character}}}};
    return SendRequestAndWait("textDocument/definition", params);
}

std::expected<nlohmann::json, std::string> Client::References(const std::string& uri, int line, int character) {
    const nlohmann::json params = {{"textDocument", {{"uri", uri}}},
                                    {"position", {{"line", line}, {"character", character}}},
                                    {"context", {{"includeDeclaration", true}}}};
    return SendRequestAndWait("textDocument/references", params);
}

std::expected<nlohmann::json, std::string> Client::DocumentSymbol(const std::string& uri) {
    const nlohmann::json params = {{"textDocument", {{"uri", uri}}}};
    return SendRequestAndWait("textDocument/documentSymbol", params);
}

std::optional<nlohmann::json> Client::WaitDiagnostics(const std::string& uri, int wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [this, &uri]() { return diagnostics_.count(uri) != 0; });
    auto it = diagnostics_.find(uri);
    if (it == diagnostics_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void Client::Shutdown() {
    if (shutdown_done_) {
        return;
    }
    shutdown_done_ = true;
    if (transport_ == nullptr) {
        return;
    }
    if (transport_->IsAlive()) {
        // 体面关停三步走:shutdown 请求(短等,不纠缠)、exit 通知、传输层
        // 兜底(等 2s,不退就 Job Object 整树杀掉)。shutdown 等待用一个
        // 收窄的临时超时——会话收尾不该被一个卡死的服务器拖 15s。
        const int saved_timeout = request_timeout_ms_;
        request_timeout_ms_ = 2000;
        (void)SendRequestAndWait("shutdown", nullptr);
        request_timeout_ms_ = saved_timeout;
        SendNotification("exit", nlohmann::json::object());
    }
    transport_->Shutdown(2000);
}

bool Client::Alive() const {
    return transport_ != nullptr && transport_->IsAlive();
}

std::string Client::StderrTail() const {
    return transport_ != nullptr ? transport_->StderrTail() : std::string();
}

}  // namespace lubancode::lsp
