#include "channel/transport/ws_frame.hpp"

#include <array>
#include <cstring>

namespace lubancode::channel::transport {

namespace {

constexpr std::size_t kMaxControlPayload = 125;  // RFC 6455 §5.5

void AppendFrameHeader(std::vector<std::byte>& out, WsOpcode opcode, bool mask,
                       std::uint64_t payload_len) {
    std::uint8_t first = 0x80 | (static_cast<std::uint8_t>(opcode) & 0x0F);  // FIN=1
    out.push_back(static_cast<std::byte>(first));
    std::uint8_t second = mask ? 0x80 : 0x00;
    if (payload_len <= 125) {
        second |= static_cast<std::uint8_t>(payload_len);
        out.push_back(static_cast<std::byte>(second));
    } else if (payload_len <= 0xFFFF) {
        second |= 126;
        out.push_back(static_cast<std::byte>(second));
        out.push_back(static_cast<std::byte>((payload_len >> 8) & 0xFF));
        out.push_back(static_cast<std::byte>(payload_len & 0xFF));
    } else {
        second |= 127;
        out.push_back(static_cast<std::byte>(second));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::byte>((payload_len >> shift) & 0xFF));
        }
    }
}

}  // namespace

std::vector<std::byte> EncodeClientFrame(WsOpcode opcode, std::string_view payload,
                                         const std::uint8_t mask_key[4]) {
    std::vector<std::byte> out;
    out.reserve(payload.size() + 14);
    AppendFrameHeader(out, opcode, /*mask=*/true, payload.size());
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>(mask_key[i]));
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        const std::uint8_t masked =
            static_cast<std::uint8_t>(payload[i]) ^ mask_key[i % 4];
        out.push_back(static_cast<std::byte>(masked));
    }
    return out;
}

std::vector<std::byte> EncodeServerFrame(WsOpcode opcode, std::string_view payload) {
    std::vector<std::byte> out;
    out.reserve(payload.size() + 10);
    AppendFrameHeader(out, opcode, /*mask=*/false, payload.size());
    out.insert(out.end(), reinterpret_cast<const std::byte*>(payload.data()),
               reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
    return out;
}

void WsFrameDecoder::Feed(const std::byte* data, std::size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
}

void WsFrameDecoder::Feed(std::string_view bytes) {
    Feed(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
}

void WsFrameDecoder::Fail(WsFrameErrorKind kind, std::string detail) {
    if (!error_.has_value()) {
        error_ = WsFrameError{kind, std::move(detail)};
    }
}

std::expected<std::optional<WsFrameEvent>, WsFrameError> WsFrameDecoder::TryNext() {
    if (error_.has_value()) {
        return std::unexpected(*error_);
    }
    // 先把已消费的字节收走,再尝试解一帧。
    if (cursor_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        cursor_ = 0;
    }
    while (true) {
        const std::size_t consumed_before = cursor_;
        const auto result = ConsumeOne();
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        if (result->has_value()) {
            return *result;
        }
        if (cursor_ == consumed_before) {
            // 这一轮没消费任何字节:真缺数据,等 Feed。
            return std::nullopt;
        }
        // 消费了帧但没出事件(分片进行中):擦掉已消费字节继续解下一帧。
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        cursor_ = 0;
    }
}

std::expected<std::optional<WsFrameEvent>, WsFrameError> WsFrameDecoder::ConsumeOne() {
    if (buffer_.size() < 2) {
        return std::nullopt;
    }
    const std::uint8_t first = static_cast<std::uint8_t>(buffer_[0]);
    const std::uint8_t second = static_cast<std::uint8_t>(buffer_[1]);

    const bool fin = (first & 0x80) != 0;
    const std::uint8_t rsv = first & 0x70;
    const std::uint8_t opcode_bits = first & 0x0F;
    const bool masked = (second & 0x80) != 0;
    std::uint64_t payload_len = second & 0x7F;
    std::size_t header_len = 2;

    if (rsv != 0) {
        Fail(WsFrameErrorKind::ProtocolError, "rsv bits set without negotiated extension");
        return std::unexpected(*error_);
    }
    if (masked) {
        // 服务端发来的帧不得掩码(RFC 6455 §5.1)。
        Fail(WsFrameErrorKind::ProtocolError, "server frame must not be masked");
        return std::unexpected(*error_);
    }
    if (payload_len == 126) {
        if (buffer_.size() < 4) {
            return std::nullopt;
        }
        payload_len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer_[2])) << 8) |
                      static_cast<std::uint8_t>(buffer_[3]);
        header_len = 4;
    } else if (payload_len == 127) {
        if (buffer_.size() < 10) {
            return std::nullopt;
        }
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) |
                          static_cast<std::uint8_t>(buffer_[static_cast<std::size_t>(2 + i)]);
        }
        if ((payload_len & 0x8000000000000000ULL) != 0) {
            Fail(WsFrameErrorKind::ProtocolError, "64-bit length high bit set");
            return std::unexpected(*error_);
        }
        header_len = 10;
    }

    const bool is_control = (opcode_bits & 0x08) != 0;
    if (is_control) {
        if (!fin) {
            Fail(WsFrameErrorKind::ProtocolError, "control frame fragmented");
            return std::unexpected(*error_);
        }
        if (payload_len > kMaxControlPayload) {
            Fail(WsFrameErrorKind::ProtocolError, "control frame payload over 125 bytes");
            return std::unexpected(*error_);
        }
    } else if (opcode_bits == 0x0) {
        // continuation:必须处在拼装中。
        if (!assembling_) {
            Fail(WsFrameErrorKind::ProtocolError, "continuation frame without start");
            return std::unexpected(*error_);
        }
    } else if (opcode_bits == 0x1 || opcode_bits == 0x2) {
        if (assembling_) {
            Fail(WsFrameErrorKind::ProtocolError, "new data frame during fragmented message");
            return std::unexpected(*error_);
        }
    } else {
        Fail(WsFrameErrorKind::ProtocolError, "reserved opcode");
        return std::unexpected(*error_);
    }

    if (payload_len > kWsMaxMessageBytes ||
        assembled_.size() + payload_len > kWsMaxMessageBytes) {
        Fail(WsFrameErrorKind::MessageTooLarge, "message exceeds ws cap");
        return std::unexpected(*error_);
    }

    if (buffer_.size() < header_len + payload_len) {
        return std::nullopt;  // 半帧:等更多字节
    }

    const char* payload =
        reinterpret_cast<const char*>(buffer_.data()) + static_cast<std::ptrdiff_t>(header_len);
    const auto opcode = static_cast<WsOpcode>(opcode_bits);

    WsFrameEvent event;
    if (is_control) {
        switch (opcode_bits) {
            case 0x9:
                event.kind = WsFrameEvent::Kind::Ping;
                event.payload.assign(payload, static_cast<std::size_t>(payload_len));
                break;
            case 0xA:
                event.kind = WsFrameEvent::Kind::Pong;
                event.payload.assign(payload, static_cast<std::size_t>(payload_len));
                break;
            case 0x8: {
                event.kind = WsFrameEvent::Kind::Close;
                if (payload_len == 1) {
                    Fail(WsFrameErrorKind::ProtocolError, "close frame with 1-byte payload");
                    return std::unexpected(*error_);
                }
                if (payload_len >= 2) {
                    event.close_code = static_cast<std::uint16_t>(
                        (static_cast<std::uint8_t>(payload[0]) << 8) |
                        static_cast<std::uint8_t>(payload[1]));
                    event.close_reason.assign(payload + 2,
                                              static_cast<std::size_t>(payload_len - 2));
                } else {
                    event.close_code = 1005;  // 无码 close(RFC 6455 §7.4.1)
                }
                break;
            }
            default:
                Fail(WsFrameErrorKind::ProtocolError, "reserved control opcode");
                return std::unexpected(*error_);
        }
    } else {
        // 数据帧:拼装。
        if (opcode_bits == 0x0) {
            assembled_.append(payload, static_cast<std::size_t>(payload_len));
        } else {
            assembling_ = true;
            assemble_opcode_ = opcode;
            assembled_.assign(payload, static_cast<std::size_t>(payload_len));
        }
        if (!fin) {
            cursor_ = header_len + payload_len;
            return std::nullopt;  // 等后续 continuation
        }
        assembling_ = false;
        event.kind = WsFrameEvent::Kind::Message;
        event.message_opcode = assemble_opcode_;
        event.payload = std::move(assembled_);
        assembled_.clear();
    }

    cursor_ = header_len + payload_len;
    return event;
}

}  // namespace lubancode::channel::transport
