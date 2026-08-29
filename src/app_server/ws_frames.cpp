// ws_frames.hpp 的实现:SHA-1、base64、握手解析、帧编解码。全在一条
// 纯函数线上,无锁无线程。
#include "app_server/ws_frames.hpp"

#include <algorithm>

namespace lubancode::app_server::ws {

namespace {

// RFC 6455 §1.3 的固定 GUID。
constexpr const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr std::uint8_t kFinBit = 0x80;
constexpr std::uint8_t kRsvBits = 0x70;
constexpr std::uint8_t kMaskBit = 0x80;

constexpr std::uint8_t kOpcodeContinuation = 0x0;
constexpr std::uint8_t kOpcodeText = 0x1;
constexpr std::uint8_t kOpcodeBinary = 0x2;
constexpr std::uint8_t kOpcodeClose = 0x8;
constexpr std::uint8_t kOpcodePing = 0x9;
constexpr std::uint8_t kOpcodePong = 0xA;

std::uint32_t LeftRotate(std::uint32_t value, unsigned count) {
    return (value << count) | (value >> (32 - count));
}

// 头部值裁空白(两宽容)。
std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// 逗号分隔的字段值里找 token(不区分大小写):"keep-alive, Upgrade" 认
// upgrade。
bool ContainsToken(std::string_view header_value, std::string_view token) {
    std::size_t pos = 0;
    while (pos <= header_value.size()) {
        const std::size_t comma = header_value.find(',', pos);
        const std::string_view item =
            Trim(header_value.substr(pos, comma == std::string_view::npos
                                              ? std::string_view::npos
                                              : comma - pos));
        if (item.size() == token.size() &&
            std::equal(token.begin(), token.end(), item.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

}  // namespace

std::array<std::uint8_t, 20> Sha1Digest(std::string_view bytes) {
    std::uint32_t h0 = 0x67452301;
    std::uint32_t h1 = 0xEFCDAB89;
    std::uint32_t h2 = 0x98BADCFE;
    std::uint32_t h3 = 0x10325476;
    std::uint32_t h4 = 0xC3D2E1F0;

    std::vector<std::uint8_t> message(bytes.begin(), bytes.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> (i * 8)) & 0xFF));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(message[offset + 4 * i]) << 24) |
                   (static_cast<std::uint32_t>(message[offset + 4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(message[offset + 4 * i + 2]) << 8) |
                   static_cast<std::uint32_t>(message[offset + 4 * i + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = LeftRotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = LeftRotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = LeftRotate(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        const std::uint32_t word = i == 0 ? h0 : i == 1 ? h1 : i == 2 ? h2 : i == 3 ? h3 : h4;
        digest[4 * i] = static_cast<std::uint8_t>((word >> 24) & 0xFF);
        digest[4 * i + 1] = static_cast<std::uint8_t>((word >> 16) & 0xFF);
        digest[4 * i + 2] = static_cast<std::uint8_t>((word >> 8) & 0xFF);
        digest[4 * i + 3] = static_cast<std::uint8_t>(word & 0xFF);
    }
    return digest;
}

std::string Base64Encode(std::string_view bytes) {
    static constexpr const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const std::uint32_t triple = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                                     (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
                                     static_cast<std::uint8_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t pair = static_cast<std::uint8_t>(bytes[i]) << 16;
        out.push_back(kAlphabet[(pair >> 18) & 0x3F]);
        out.push_back(kAlphabet[(pair >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const std::uint32_t pair = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                                   (static_cast<std::uint8_t>(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(pair >> 18) & 0x3F]);
        out.push_back(kAlphabet[(pair >> 12) & 0x3F]);
        out.push_back(kAlphabet[(pair >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string ComputeAcceptKey(std::string_view client_key) {
    const std::string salted = std::string(client_key) + kWebSocketGuid;
    const auto digest = Sha1Digest(salted);
    return Base64Encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

UpgradeParseResult ParseUpgradeRequest(std::string_view request_bytes) {
    UpgradeParseResult result;
    // 请求行:GET <path> HTTP/1.1
    const std::size_t line_end = request_bytes.find("\r\n");
    if (line_end == std::string_view::npos) {
        result.error = "升级请求没有请求行";
        return result;
    }
    const std::string_view request_line = request_bytes.substr(0, line_end);
    if (request_line.rfind("GET ", 0) != 0) {
        result.error = "升级请求不是 GET";
        return result;
    }
    if (request_line.find("HTTP/1.1") == std::string_view::npos) {
        result.error = "升级请求不是 HTTP/1.1";
        return result;
    }
    // 头部逐行扫:Upgrade/Connection/Sec-WebSocket-Key/Sec-WebSocket-Version。
    bool has_upgrade = false;
    bool has_connection = false;
    bool has_version_13 = false;
    std::string_view rest = request_bytes.substr(line_end + 2);
    while (!rest.empty()) {
        const std::size_t next_line = rest.find("\r\n");
        std::string_view line = next_line == std::string_view::npos ? rest : rest.substr(0, next_line);
        if (next_line == std::string_view::npos) {
            rest = {};
        } else {
            rest = rest.substr(next_line + 2);
        }
        if (line.empty()) {
            break; // 头部收尾
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue; // 坏行跳过,不为之拒——缺关键头才拒
        }
        const std::string_view name = Trim(line.substr(0, colon));
        const std::string_view value = Trim(line.substr(colon + 1));
        const auto equals = [&](std::string_view expected) {
            return name.size() == expected.size() &&
                   std::equal(expected.begin(), expected.end(), name.begin(),
                              [](char a, char b) {
                                  return std::tolower(static_cast<unsigned char>(a)) ==
                                         std::tolower(static_cast<unsigned char>(b));
                              });
        };
        if (equals("upgrade")) {
            has_upgrade = has_upgrade || ContainsToken(value, "websocket");
        } else if (equals("connection")) {
            has_connection = has_connection || ContainsToken(value, "upgrade");
        } else if (equals("sec-websocket-version")) {
            has_version_13 = has_version_13 || value == "13";
        } else if (equals("sec-websocket-key")) {
            result.websocket_key = std::string(value);
        }
    }
    if (!has_upgrade) {
        result.error = "升级请求缺 Upgrade: websocket";
        return result;
    }
    if (!has_connection) {
        result.error = "升级请求缺 Connection: Upgrade";
        return result;
    }
    if (!has_version_13) {
        result.error = "升级请求缺 Sec-WebSocket-Version: 13";
        return result;
    }
    if (result.websocket_key.empty()) {
        result.error = "升级请求缺 Sec-WebSocket-Key";
        return result;
    }
    result.valid = true;
    return result;
}

std::string MakeUpgradeResponse(std::string_view accept_key) {
    std::string response;
    response.reserve(160);
    response += "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: ";
    response += accept_key;
    response += "\r\n\r\n";
    return response;
}

namespace {

// 出帧共体:FIN=1、不掩码、指定 opcode。长度按 RFC 三档。
std::string MakeFrame(std::uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(kFinBit | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xFF));
        }
    }
    frame.append(payload);
    return frame;
}

}  // namespace

std::string MakeTextFrame(std::string_view payload) {
    return MakeFrame(kOpcodeText, payload);
}

std::string MakeCloseFrame(std::uint16_t code) {
    const char payload[2] = {static_cast<char>((code >> 8) & 0xFF), static_cast<char>(code & 0xFF)};
    return MakeFrame(kOpcodeClose, std::string_view(payload, 2));
}

std::string MakePongFrame(std::string_view payload) {
    return MakeFrame(kOpcodePong, payload);
}

std::vector<FrameEvent> FrameDecoder::Feed(std::string_view chunk) {
    std::vector<FrameEvent> events;
    if (failed_) {
        return events;
    }
    buffer_.append(chunk.begin(), chunk.end());
    while (true) {
        if (buffer_.size() < 2) {
            break; // 头都不齐
        }
        const std::uint8_t first = static_cast<std::uint8_t>(buffer_[0]);
        const std::uint8_t second = static_cast<std::uint8_t>(buffer_[1]);
        const bool fin = (first & kFinBit) != 0;
        const std::uint8_t opcode = first & 0x0F;
        if ((first & kRsvBits) != 0) {
            events.push_back(Fail("帧带 rsv 位(扩展不谈)"));
            return events;
        }
        if ((second & kMaskBit) == 0) {
            events.push_back(Fail("客户端帧必须掩码"));
            return events;
        }
        std::size_t header_size = 2;
        std::uint64_t length = second & 0x7F;
        if (length == 126) {
            if (buffer_.size() < 4) {
                break;
            }
            length = (static_cast<std::uint8_t>(buffer_[2]) << 8) |
                     static_cast<std::uint8_t>(buffer_[3]);
            header_size = 4;
        } else if (length == 127) {
            if (buffer_.size() < 10) {
                break;
            }
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<std::uint8_t>(buffer_[2 + i]);
            }
            header_size = 10;
            if (length > kMaxMessageBytes) {
                events.push_back(Fail("帧长超过上限"));
                return events;
            }
        }
        // 控制帧:不许分片、载荷不超 125。
        if (opcode >= 0x8 && (!fin || length > 125)) {
            events.push_back(Fail("控制帧必须整帧且不超 125 字节"));
            return events;
        }
        const std::size_t mask_offset = header_size;
        const std::size_t payload_offset = header_size + 4;
        if (buffer_.size() < payload_offset + length) {
            break; // 载荷没到齐
        }
        const std::uint8_t mask[4] = {
            static_cast<std::uint8_t>(buffer_[mask_offset]),
            static_cast<std::uint8_t>(buffer_[mask_offset + 1]),
            static_cast<std::uint8_t>(buffer_[mask_offset + 2]),
            static_cast<std::uint8_t>(buffer_[mask_offset + 3]),
        };
        std::string payload(length, '\0');
        for (std::uint64_t i = 0; i < length; ++i) {
            payload[static_cast<std::size_t>(i)] =
                static_cast<char>(static_cast<std::uint8_t>(
                                      buffer_[payload_offset + static_cast<std::size_t>(i)]) ^
                                  mask[i % 4]);
        }
        buffer_.erase(0, payload_offset + static_cast<std::size_t>(length));

        switch (opcode) {
            case kOpcodeText:
                if (frag_pending_) {
                    events.push_back(Fail("分片没拼完又来新起帧"));
                    return events;
                }
                if (!fin) {
                    if (length > kMaxMessageBytes) {
                        events.push_back(Fail("消息超过上限"));
                        return events;
                    }
                    frag_pending_ = true;
                    frag_payload_ = std::move(payload);
                    continue;
                }
                {
                    FrameEvent event;
                    event.kind = FrameEvent::Kind::Text;
                    event.payload = std::move(payload);
                    events.push_back(std::move(event));
                }
                continue;
            case kOpcodeContinuation:
                if (!frag_pending_) {
                    events.push_back(Fail("没有起帧却来了续帧"));
                    return events;
                }
                if (frag_payload_.size() + length > kMaxMessageBytes) {
                    events.push_back(Fail("消息超过上限"));
                    return events;
                }
                frag_payload_ += payload;
                if (fin) {
                    FrameEvent event;
                    event.kind = FrameEvent::Kind::Text;
                    event.payload = std::move(frag_payload_);
                    frag_payload_.clear();
                    frag_pending_ = false;
                    events.push_back(std::move(event));
                }
                continue;
            case kOpcodeBinary:
                events.push_back(Fail("协议只收文本帧"));
                return events;
            case kOpcodeClose: {
                FrameEvent event;
                event.kind = FrameEvent::Kind::Close;
                event.payload = std::move(payload);
                events.push_back(std::move(event));
                failed_ = true; // 对端收线,后续字节没有意义
                return events;
            }
            case kOpcodePing: {
                FrameEvent event;
                event.kind = FrameEvent::Kind::Ping;
                event.payload = std::move(payload);
                events.push_back(std::move(event));
                continue;
            }
            case kOpcodePong:
                continue; // 没发 ping,pong 当噪音忽略
            default:
                events.push_back(Fail("不认识的 opcode"));
                return events;
        }
    }
    return events;
}

FrameEvent FrameDecoder::Fail(std::string reason) {
    failed_ = true;
    buffer_.clear();
    frag_pending_ = false;
    frag_payload_.clear();
    FrameEvent event;
    event.kind = FrameEvent::Kind::Error;
    event.reason = std::move(reason);
    return event;
}

}  // namespace lubancode::app_server::ws
