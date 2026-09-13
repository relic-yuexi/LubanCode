#include "channel/qq/qq_ws_client.hpp"

#include <array>
#include <cstdio>
#include <random>

#include <mbedtls/sha1.h>

#include "channel/qq/ws_frame.hpp"
#include "platform/base64.hpp"

namespace lubancode::channel::qq {

namespace {

// RFC 6455 §1.3 的固定 GUID。
constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct ParsedWsUrl {
    bool tls = false;
    std::string host;
    int port = 0;
    std::string path_query;  // 以 '/' 起,含 query
};

std::optional<ParsedWsUrl> ParseWsUrl(const std::string& url) {
    ParsedWsUrl out;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) {
        out.tls = true;
        rest = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
    } else {
        return std::nullopt;
    }
    const std::size_t slash = rest.find('/');
    std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.path_query = (slash == std::string::npos) ? "/" : rest.substr(slash);
    // 去掉 userinfo(不支持)。
    const std::size_t at = host_port.rfind('@');
    if (at != std::string::npos) {
        host_port = host_port.substr(at + 1);
    }
    std::string port_text;
    const std::size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        out.host = host_port.substr(0, colon);
        port_text = host_port.substr(colon + 1);
    } else {
        out.host = host_port;
        port_text = out.tls ? "443" : "80";
    }
    if (out.host.empty()) {
        return std::nullopt;
    }
    out.port = std::atoi(port_text.c_str());
    if (out.port <= 0 || out.port > 65535) {
        return std::nullopt;
    }
    return out;
}

std::string ToLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 从响应头文本里取首个指定头(大小写不敏感)。找不到返回空串。
std::string HeaderValue(const std::string& headers, const std::string& name) {
    std::string line_prefix = ToLower(name) + ":";
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const std::size_t eol = headers.find("\r\n", pos);
        const std::string line =
            ToLower(headers.substr(pos, (eol == std::string::npos ? headers.size() : eol) - pos));
        if (line.rfind(line_prefix, 0) == 0) {
            std::size_t value_start = pos + line_prefix.size();
            while (value_start < headers.size() &&
                   (headers[value_start] == ' ' || headers[value_start] == '\t')) {
                ++value_start;
            }
            std::size_t value_end = (eol == std::string::npos) ? headers.size() : eol;
            while (value_end > value_start &&
                   (headers[value_end - 1] == ' ' || headers[value_end - 1] == '\t')) {
                --value_end;
            }
            return headers.substr(value_start, value_end - value_start);
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 2;
    }
    return std::string();
}

void FillRandom(std::uint8_t* out, std::size_t count) {
    std::random_device rd;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>(rd() & 0xFF);
    }
}

std::string ComputeAccept(const std::string& key) {
    std::string concatenated = key + kWsGuid;
    std::array<unsigned char, 20> digest{};
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(concatenated.data()),
                 concatenated.size(), digest.data());
    return platform::Base64Encode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

WsError ToWsError(const SocketError& error) {
    switch (error.kind) {
        case SocketErrorKind::Timeout:
            return WsError{WsError::Kind::Timeout, error.detail, 0};
        case SocketErrorKind::Closed:
            return WsError{WsError::Kind::Closed, error.detail, 0};
        default:
            return WsError{WsError::Kind::Failed, error.detail, 0};
    }
}

}  // namespace

std::expected<std::size_t, SocketError> WsClient::ReadSome(char* buf, std::size_t len,
                                                           int timeout_ms) const {
    if (tls_.has_value()) {
        return tls_->ReadSome(buf, len, timeout_ms);
    }
    return socket_.ReadSome(buf, len, timeout_ms);
}

std::expected<void, SocketError> WsClient::WriteAll(std::string_view bytes,
                                                    int timeout_ms) const {
    if (tls_.has_value()) {
        return tls_->WriteAll(bytes, timeout_ms);
    }
    return socket_.WriteAll(bytes, timeout_ms);
}

std::expected<WsClient, WsError> WsClient::Connect(const WsConnectOptions& options) {
    const auto parsed = ParseWsUrl(options.url);
    if (!parsed.has_value()) {
        return std::unexpected(
            WsError{WsError::Kind::Protocol, "url must be ws:// or wss://", 0});
    }

    auto socket = TcpSocket::Connect(parsed->host, parsed->port, options.connect_timeout_ms);
    if (!socket.has_value()) {
        return std::unexpected(ToWsError(socket.error()));
    }

    std::optional<TlsClientStream> tls;
    if (parsed->tls) {
        auto stream = TlsClientStream::Connect(&*socket, parsed->host, options.ca_pem,
                                               options.connect_timeout_ms);
        if (!stream.has_value()) {
            return std::unexpected(WsError{WsError::Kind::Failed,
                                           "tls: " + stream.error().detail, 0});
        }
        tls = std::move(*stream);
    }

    WsClient client(std::move(*socket), std::move(tls));

    // 升级握手(RFC 6455 §4.2.1)。
    std::array<std::uint8_t, 16> key_raw{};
    FillRandom(key_raw.data(), key_raw.size());
    const std::string key =
        platform::Base64Encode(std::string_view(
            reinterpret_cast<const char*>(key_raw.data()), key_raw.size()));
    const std::string host_header =
        parsed->host + ":" + std::to_string(parsed->port);
    std::string request;
    request.reserve(256);
    request += "GET " + parsed->path_query + " HTTP/1.1\r\n";
    request += "Host: " + host_header + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "\r\n";
    const auto written = client.WriteAll(request, options.io_timeout_ms);
    if (!written.has_value()) {
        return std::unexpected(ToWsError(written.error()));
    }

    // 收响应头到 \r\n\r\n(带帽)。
    std::string received;
    received.reserve(1024);
    while (true) {
        const std::size_t eoh = received.find("\r\n\r\n");
        if (eoh != std::string::npos) {
            client.pending_bytes_ = received.substr(eoh + 4);
            received.resize(eoh);
            break;
        }
        if (received.size() > kWsHandshakeHeaderCap) {
            return std::unexpected(
                WsError{WsError::Kind::Protocol, "handshake response header over cap", 0});
        }
        char chunk[512];
        const auto got = client.ReadSome(chunk, sizeof(chunk), options.connect_timeout_ms);
        if (!got.has_value()) {
            return std::unexpected(ToWsError(got.error()));
        }
        received.append(chunk, *got);
    }

    const std::size_t sp1 = received.find(' ');
    const std::size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                       : received.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos ||
        received.substr(sp1 + 1, sp2 - sp1 - 1) != "101") {
        return std::unexpected(WsError{WsError::Kind::Protocol,
                                       "upgrade not accepted: " + received.substr(0, 64), 0});
    }
    if (ToLower(HeaderValue(received, "Upgrade")) != "websocket") {
        return std::unexpected(
            WsError{WsError::Kind::Protocol, "missing upgrade: websocket header", 0});
    }
    const std::string accept = HeaderValue(received, "Sec-WebSocket-Accept");
    if (accept.empty() || accept != ComputeAccept(key)) {
        return std::unexpected(
            WsError{WsError::Kind::Protocol, "sec-websocket-accept mismatch", 0});
    }

    return client;
}

std::expected<void, WsError> WsClient::SendText(std::string_view text) {
    std::uint8_t mask_key[4];
    FillRandom(mask_key, sizeof(mask_key));
    const auto frame = EncodeClientFrame(WsOpcode::Text, text, mask_key);
    const auto written = WriteAll(
        std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()),
        10'000);
    if (!written.has_value()) {
        return std::unexpected(ToWsError(written.error()));
    }
    ++sent_messages_;
    return {};
}

std::expected<std::string, WsError> WsClient::ReadMessage(int timeout_ms) {
    // 先消化握手尾巴/上次剩余字节,再按需读网络。
    if (!pending_bytes_.empty()) {
        decoder_.Feed(pending_bytes_);
        pending_bytes_.clear();
    }
    while (true) {
        auto next = decoder_.TryNext();
        if (!next.has_value()) {
            const auto& err = next.error();
            return std::unexpected(WsError{err.kind == WsFrameErrorKind::MessageTooLarge
                                                ? WsError::Kind::Protocol
                                                : WsError::Kind::Protocol,
                                            err.detail, 0});
        }
        if (next->has_value()) {
            const WsFrameEvent& event = **next;
            switch (event.kind) {
                case WsFrameEvent::Kind::Message:
                    if (event.message_opcode != WsOpcode::Text &&
                        event.message_opcode != WsOpcode::Binary) {
                        continue;
                    }
                    ++received_messages_;
                    return event.payload;
                case WsFrameEvent::Kind::Ping: {
                    // 自动回 Pong(载荷回传,RFC 6455 §5.5.2)。
                    std::uint8_t mask_key[4];
                    FillRandom(mask_key, sizeof(mask_key));
                    const auto frame = EncodeClientFrame(WsOpcode::Pong, event.payload, mask_key);
                    const auto written =
                        WriteAll(std::string_view(reinterpret_cast<const char*>(frame.data()),
                                                  frame.size()),
                                 3'000);
                    if (!written.has_value()) {
                        return std::unexpected(ToWsError(written.error()));
                    }
                    continue;
                }
                case WsFrameEvent::Kind::Pong:
                    continue;  // 未发心跳探测时忽略(QQ 网关不发 ping 探测语义)
                case WsFrameEvent::Kind::Close:
                    return std::unexpected(
                        WsError{WsError::Kind::Closed, "peer sent close", event.close_code});
            }
        }
        // 字节不够:读网络。
        char chunk[4096];
        const auto got = ReadSome(chunk, sizeof(chunk), timeout_ms);
        if (!got.has_value()) {
            return std::unexpected(ToWsError(got.error()));
        }
        decoder_.Feed(std::string_view(chunk, *got));
    }
}

std::expected<void, WsError> WsClient::Close(std::uint16_t code, std::string_view reason) {
    // close 载荷 = 2 字节码 + reason(RFC 6455 §5.5.1)。
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.data(), reason.size());
    std::uint8_t mask_key[4];
    FillRandom(mask_key, sizeof(mask_key));
    const auto frame = EncodeClientFrame(WsOpcode::Close, payload, mask_key);
    (void)WriteAll(std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()),
                   3'000);
    // 尽力等对端 close(短超时)。
    char chunk[512];
    const auto got = ReadSome(chunk, sizeof(chunk), 1'500);
    (void)got;
    if (tls_.has_value()) {
        tls_->CloseNotify();
    }
    socket_.Close();
    return {};
}

void WsClient::Cancel() {
    socket_.ShutdownBoth();
}

}  // namespace lubancode::channel::qq
