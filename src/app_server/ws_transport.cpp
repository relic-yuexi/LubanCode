// ws_transport.hpp 的实现:监听、升级、鉴权门、Session 读写、artifact 只读面。
#include "app_server/ws_transport.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "tools/path_utils.hpp"  // Utf8ToPath:UTF-8 路径进 filesystem(Windows ACP 坑)

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

// 升级请求的读入上限:头部就几行,超了就是来捣乱的,断。
constexpr std::size_t kMaxUpgradeBytes = 16 * 1024;

// 单枚 artifact 的读入上限:截图/镜像帧是图,内容寻址落盘时本就有尺寸
// 约束;这里再兜一层,超了按"没有这枚"回,不当流媒体伺候。
constexpr std::uintmax_t kMaxArtifactBytes = 64ull * 1024 * 1024;

// 只读面的固定应答头:CORS 放行(参考前端从 file:// 或本机静态服务开页,
// 字节要跨源取;口子只在回环/过 token 门后才到这一步)+ 内容寻址可缓存。
constexpr std::string_view kArtifactExtraHeaders =
    "Access-Control-Allow-Origin: *\r\nCache-Control: private, max-age=86400, immutable\r\n";

// 一口一口读到 \r\n\r\n 或断/超上限。返回空 = 断/坏。
bool ReadUntilHeaderEnd(net::Socket& socket, std::string& header) {
    char buffer[2048];
    while (header.find("\r\n\r\n") == std::string::npos) {
        if (header.size() > kMaxUpgradeBytes) {
            return false;
        }
        const long got = socket.Recv(buffer, sizeof(buffer));
        if (got <= 0) {
            return false;
        }
        header.append(buffer, buffer + got);
    }
    return true;
}

// HTTP 应答(纯文本小应答:拒升级/404/403 用)。
bool SendHttpResponse(net::Socket& socket, const char* status_line, const char* text) {
    const std::string response = ws::MakeHttpResponse(status_line, "text/plain; charset=utf-8",
                                                      std::string_view(text), kArtifactExtraHeaders);
    return socket.SendAll(response);
}

// 恒时比较:逐字节累积差,不短路——不给计时侧信道留口。长度不同直接
// false(长度本身不是秘密)。
bool ConstantTimeEqual(std::string_view given, std::string_view expected) {
    if (given.size() != expected.size()) {
        return false;
    }
    unsigned diff = 0;
    for (std::size_t i = 0; i < given.size(); ++i) {
        diff |= static_cast<unsigned char>(given[i]) ^ static_cast<unsigned char>(expected[i]);
    }
    return diff == 0;
}

}  // namespace

WsTransport::WsTransport(WsOptions options) : options_(std::move(options)) {
    if (options_.bind_host.empty()) {
        options_.bind_host = "127.0.0.1";
    }
}

WsTransport::~WsTransport() {
    Stop();
}

bool WsTransport::Start() {
    std::string error;
    if (!listener_.Start(options_.bind_host, options_.port, error)) {
        Diagnose("WS 监听起不来: " + error);
        return false;
    }
    started_ = true;
    // 监听信息进 stderr(诊断);token 永不进日志。
    Diagnose("WS 监听: " + options_.bind_host + ":" + std::to_string(listener_.actual_port()) +
             (options_.token.empty() ? "(免鉴权)" : "(首帧 token 门)"));
    return true;
}

bool WsTransport::Session::SendRaw(std::string_view bytes) {
    return socket_.SendAll(bytes);
}

WsTransport::Session::~Session() {
    Close();
}

std::optional<std::string> WsTransport::Session::ReadMessage() {
    while (inbox_.empty()) {
        if (decoder_.failed()) {
            return std::nullopt;
        }
        char buffer[65536];
        const long got = socket_.Recv(buffer, sizeof(buffer));
        if (got <= 0) {
            return std::nullopt; // 断/EOF
        }
        for (const ws::FrameEvent& event : decoder_.Feed(std::string_view(buffer, buffer + got))) {
            switch (event.kind) {
                case ws::FrameEvent::Kind::Text:
                    inbox_.push_back(std::move(event.payload));
                    continue;
                case ws::FrameEvent::Kind::Ping:
                    SendFrame(ws::MakePongFrame(event.payload));
                    continue;
                case ws::FrameEvent::Kind::Close:
                    // 对端收线:回敬 close(尽力),对上层就是 EOF。
                    Close();
                    return std::nullopt;
                case ws::FrameEvent::Kind::Error:
                    Diagnose("WS 帧协议错: " + event.reason);
                    Close();
                    return std::nullopt;
            }
        }
    }
    std::string message = std::move(inbox_.front());
    inbox_.pop_front();
    return message;
}

bool WsTransport::Session::SendMessage(std::string_view payload) {
    return SendFrame(ws::MakeTextFrame(payload));
}

bool WsTransport::Session::SendFrame(std::string_view frame) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (close_sent_) {
        return false;
    }
    return SendRaw(frame);
}

void WsTransport::Session::Close() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (close_sent_) {
        return;
    }
    close_sent_ = true;
    // 尽力回敬一条 close(对端多半已经走了,失败不追)。
    SendRaw(ws::MakeCloseFrame(1000));
    socket_.Close();
}

std::unique_ptr<WsTransport::Session> WsTransport::Accept() {
    while (true) {
        std::optional<net::Socket> accepted = listener_.Accept(options_.accept_poll_ms);
        if (!accepted.has_value()) {
            if (listener_.stopped() || !listener_.last_error().empty()) {
                return nullptr;
            }
            continue; // 超时:接着等
        }
        net::Socket& socket = *accepted;
        // ---- HTTP 头(升级请求或只读 GET 都是一段 HTTP 头起手) ----
        std::string header;
        if (!ReadUntilHeaderEnd(socket, header)) {
            continue; // 对端跑了/捣乱,断掉等下一条
        }
        // 只读 GET(阶段 D):artifact 字节口子,在这层就地应答,继续等
        // 下一条连接——不占 Session,不进协议线。
        const ws::HttpRequestHead head = ws::ParseHttpRequestHead(header);
        if (head.method == "GET" && head.target.rfind("/artifact/", 0) == 0) {
            ServeArtifactGet(socket, head);
            continue;
        }
        // ---- HTTP 升级 ----
        const ws::UpgradeParseResult upgrade = ws::ParseUpgradeRequest(header);
        if (!upgrade.valid) {
            Diagnose("WS 升级拒: " + upgrade.error);
            SendHttpResponse(socket, "400 Bad Request", "not a websocket upgrade");
            continue;
        }
        if (!socket.SendAll(ws::MakeUpgradeResponse(ws::ComputeAcceptKey(upgrade.websocket_key)))) {
            continue;
        }
        // make_unique 够不着 private 构造(它不是友元),成员函数里直接
        // new——Session 的生杀都在 WsTransport 手里。
        std::unique_ptr<Session> session(new Session(std::move(socket)));
        // ---- 首帧 token 门 ----
        if (!options_.token.empty()) {
            const std::optional<std::string> first = session->ReadMessage();
            if (!first.has_value() || !CheckAuthTokenFrame(*first, options_.token)) {
                Diagnose("WS 首帧鉴权不过,断线");
                session->Close();
                continue;
            }
        }
        return session;
    }
}

bool CheckAuthTokenFrame(std::string_view message, std::string_view expected_token) {
    const nlohmann::json parsed = nlohmann::json::parse(message, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    if (!parsed.contains("method") || !parsed["method"].is_string() ||
        parsed["method"].get<std::string>() != "app_server/auth") {
        return false;
    }
    if (!parsed.contains("params") || !parsed["params"].is_object() ||
        !parsed["params"].contains("token") || !parsed["params"]["token"].is_string()) {
        return false;
    }
    const std::string given = parsed["params"]["token"].get<std::string>();
    return ConstantTimeEqual(given, expected_token);
}

// GET /artifact/<内容寻址名> 的执行体(阶段 D)。次序有讲究:
//   1. token 门(配了 token 才有这道)——403 的正文不区分"没带/带错",
//      不给试探省事;token 恒时比较,不落日志、不进应答。
//   2. 名字形状(IsValidArtifactName:art-<hex>.(png|jpeg|jpg),此外一概
//      不认)与目录配置——形状不对/没配目录/文件不在,一律 404,同一种
//      话,不泄露目录里有什么。
//   3. 读文件(上限兜底)→ 200 + 字节 + CORS/缓存头。
// 应答完连接即关;失败也关(调用方 Accept 循环 continue,socket 析构)。
void WsTransport::ServeArtifactGet(net::Socket& socket, const ws::HttpRequestHead& head) const {
    if (!options_.token.empty()) {
        const std::string given = !head.bearer_token.empty()
                                      ? head.bearer_token
                                      : ws::QueryParam(head.query, "token");
        if (!ConstantTimeEqual(given, options_.token)) {
            SendHttpResponse(socket, "403 Forbidden", "token required");
            return;
        }
    }
    const std::string name = head.target.substr(std::string_view("/artifact/").size());
    if (options_.artifact_dir.empty() || !ws::IsValidArtifactName(name)) {
        SendHttpResponse(socket, "404 Not Found", "no such artifact");
        return;
    }
    const std::filesystem::path path =
        tools::Utf8ToPath(options_.artifact_dir) / tools::Utf8ToPath(name);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxArtifactBytes) {
        SendHttpResponse(socket, "404 Not Found", "no such artifact");
        return;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SendHttpResponse(socket, "404 Not Found", "no such artifact");
        return;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!file && file.gcount() != static_cast<std::streamsize>(size)) {
        SendHttpResponse(socket, "404 Not Found", "no such artifact");
        return;
    }
    // 扩展名已由形状校验收口(png/jpeg/jpg 三选一)。
    const char* mime = name.compare(name.size() - 3, 3, "png") == 0 ? "image/png" : "image/jpeg";
    const std::string response =
        ws::MakeHttpResponse("200 OK", mime, bytes, kArtifactExtraHeaders);
    socket.SendAll(response);
}

}  // namespace lubancode::app_server
