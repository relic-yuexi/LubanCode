// ws_transport.hpp 的实现:监听、升级、鉴权门、Session 读写。
#include "app_server/ws_transport.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include <nlohmann/json.hpp>

namespace lubancode::app_server {

namespace {

void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

// 升级请求的读入上限:头部就几行,超了就是来捣乱的,断。
constexpr std::size_t kMaxUpgradeBytes = 16 * 1024;

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

// HTTP 应答(拒升级用;鉴权不在 HTTP 层,这里只有"不是升级请求"一种拒)。
bool SendHttpResponse(net::Socket& socket, const char* status_line, const char* body) {
    std::string response;
    response += "HTTP/1.1 ";
    response += status_line;
    response += "\r\nConnection: close\r\nContent-Length: ";
    response += std::to_string(std::strlen(body));
    response += "\r\n\r\n";
    response += body;
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
        // ---- HTTP 升级 ----
        std::string header;
        if (!ReadUntilHeaderEnd(socket, header)) {
            continue; // 对端跑了/捣乱,断掉等下一条
        }
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
    if (given.size() != expected_token.size()) {
        return false;
    }
    // 恒时比较:逐字节累积差,不短路。
    unsigned diff = 0;
    for (std::size_t i = 0; i < given.size(); ++i) {
        diff |= static_cast<unsigned char>(given[i]) ^ static_cast<unsigned char>(expected_token[i]);
    }
    return diff == 0;
}

}  // namespace lubancode::app_server
