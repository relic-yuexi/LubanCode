// ws_transport.hpp 的实现:监听、升级、鉴权门、Session 读写、artifact 只读面。
// 读头/恒时比较/artifact 加载走 http_support(与助理 Web 承载共用一份,
// HC-03);这层只留承载策略:token 门、CORS/不可变缓存头、升级握手。
#include "app_server/ws_transport.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include <nlohmann/json.hpp>

#include "app_server/http_support.hpp"

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

// 只读面的固定应答头:CORS 放行(参考前端从 file:// 或本机静态服务开页,
// 字节要跨源取;口子只在回环/过 token 门后才到这一步)+ 内容寻址可缓存。
constexpr std::string_view kArtifactExtraHeaders =
    "Access-Control-Allow-Origin: *\r\nCache-Control: private, max-age=86400, immutable\r\n";

// HTTP 应答(纯文本小应答:拒升级/404/403 用)。
bool SendHttpResponse(net::Socket& socket, const char* status_line, const char* text) {
    const std::string response = ws::MakeHttpResponse(status_line, "text/plain; charset=utf-8",
                                                      std::string_view(text), kArtifactExtraHeaders);
    return socket.SendAll(response);
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

void WsTransport::Session::CloseGracefully(int drain_ms) {
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (!close_sent_) {
            close_sent_ = true;
            SendRaw(ws::MakeCloseFrame(1000));
        }
    }
    // 写锁到此放开:排干是纯读端动作,不与 SendMessage 抢锁。
    socket_.DrainThenClose(drain_ms);
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
//   2. 名字形状与目录配置、尺寸上限、整读、MIME——全在
//      LoadArtifactBytes(http_support,与助理 Web 承载同一份);形状不对/
//      没配目录/文件不在,一律 404,同一种话,不泄露目录里有什么。
//   3. 200 + 字节 + CORS/缓存头。
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
    const ArtifactBytes artifact = LoadArtifactBytes(options_.artifact_dir, name);
    if (!artifact.ok) {
        SendHttpResponse(socket, "404 Not Found", "no such artifact");
        return;
    }
    const std::string response =
        ws::MakeHttpResponse("200 OK", artifact.mime, artifact.bytes, kArtifactExtraHeaders);
    socket.SendAll(response);
}

}  // namespace lubancode::app_server
