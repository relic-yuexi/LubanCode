#include "channel/frame.hpp"

#include "platform/text_encoding.hpp"

namespace lubancode::channel {

std::string_view FrameErrorStableName(FrameErrorCode code) {
    switch (code) {
        case FrameErrorCode::FrameTooLarge: return "frame_too_large";
        case FrameErrorCode::InvalidUtf8: return "invalid_utf8";
        case FrameErrorCode::InvalidFrame: return "invalid_frame";
    }
    return "?";
}

namespace {

void PushBigEndianU32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

std::uint32_t ReadBigEndianU32(const std::byte* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

}  // namespace

std::expected<std::vector<std::byte>, FrameError> EncodeFrame(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return std::unexpected(FrameError{FrameErrorCode::InvalidFrame, "payload must be a JSON object"});
    }
    const std::string body = payload.dump();
    if (body.size() > kMaxFrameBytes) {
        return std::unexpected(FrameError{
            FrameErrorCode::FrameTooLarge,
            "encoded frame body " + std::to_string(body.size()) + " bytes exceeds 8 MiB cap"});
    }
    // dump() 的输出保证是合法 UTF-8(nlohmann::json 内部字符串本就是 UTF-8
    // 存储;这里仍复核一遍,防上游把非 UTF-8 字节塞进了字符串值又逃过了
    // json 层校验的边角场景)。
    if (!platform::IsValidUtf8(body)) {
        return std::unexpected(FrameError{FrameErrorCode::InvalidUtf8, "encoded frame body is not valid UTF-8"});
    }
    std::vector<std::byte> out;
    out.reserve(kFrameHeaderBytes + body.size());
    PushBigEndianU32(out, static_cast<std::uint32_t>(body.size()));
    for (const char ch : body) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

void FrameDecoder::Feed(const std::byte* data, std::size_t size) {
    if (error_.has_value() || size == 0) return;
    buffer_.insert(buffer_.end(), data, data + size);
}

void FrameDecoder::Feed(std::string_view bytes) {
    Feed(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
}

void FrameDecoder::Compact() {
    // 游标越过半个缓冲才搬,避免每帧都整体前移(小成本换少量内存搬运)。
    if (cursor_ > 0 && cursor_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        cursor_ = 0;
    }
}

std::expected<std::optional<nlohmann::json>, FrameError> FrameDecoder::TryDecodeNext() {
    if (error_.has_value()) {
        return std::unexpected(*error_);
    }
    const std::size_t available = buffer_.size() - cursor_;
    if (available < kFrameHeaderBytes) {
        return std::nullopt;  // 长度前缀都没凑齐,等下次 Feed
    }
    const std::uint32_t body_len = ReadBigEndianU32(buffer_.data() + cursor_);
    if (body_len > kMaxFrameBytes) {
        error_ = FrameError{FrameErrorCode::FrameTooLarge,
                            "declared frame length " + std::to_string(body_len) + " exceeds 8 MiB cap"};
        return std::unexpected(*error_);
    }
    if (available < kFrameHeaderBytes + body_len) {
        return std::nullopt;  // 正文还没到齐,等下次 Feed(不消费残余字节)
    }
    const std::byte* body_start = buffer_.data() + cursor_ + kFrameHeaderBytes;
    std::string body(reinterpret_cast<const char*>(body_start), body_len);
    cursor_ += kFrameHeaderBytes + body_len;
    Compact();

    if (!platform::IsValidUtf8(body)) {
        error_ = FrameError{FrameErrorCode::InvalidUtf8, "frame body is not valid UTF-8"};
        return std::unexpected(*error_);
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error& ex) {
        error_ = FrameError{FrameErrorCode::InvalidFrame, std::string("JSON parse error: ") + ex.what()};
        return std::unexpected(*error_);
    }
    if (!parsed.is_object()) {
        error_ = FrameError{FrameErrorCode::InvalidFrame, "frame body must be a JSON object"};
        return std::unexpected(*error_);
    }
    return parsed;
}

}  // namespace lubancode::channel
