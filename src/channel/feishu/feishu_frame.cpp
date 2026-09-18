#include "channel/feishu/feishu_frame.hpp"

#include <cstring>

namespace lubancode::channel::feishu {

namespace {

// 字段号 × wire 类型 → tag 字节(proto3 编码:field << 3 | wire_type)。
//   varint(0):1/2/3/4;length-delimited(2):5/6/7/8/9(Header 内 1/2 同)。
constexpr std::uint8_t kTagSeqId = (1 << 3) | 0;
constexpr std::uint8_t kTagLogId = (2 << 3) | 0;
constexpr std::uint8_t kTagService = (3 << 3) | 0;
constexpr std::uint8_t kTagMethod = (4 << 3) | 0;
constexpr std::uint8_t kTagHeaders = (5 << 3) | 2;
constexpr std::uint8_t kTagPayloadEncoding = (6 << 3) | 2;
constexpr std::uint8_t kTagPayloadType = (7 << 3) | 2;
constexpr std::uint8_t kTagPayload = (8 << 3) | 2;
constexpr std::uint8_t kTagLogIdNew = (9 << 3) | 2;
constexpr std::uint8_t kTagHeaderKey = (1 << 3) | 2;
constexpr std::uint8_t kTagHeaderValue = (2 << 3) | 2;

void AppendVarint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void AppendLengthDelimited(std::string& out, std::uint8_t tag, std::string_view bytes) {
    out.push_back(static_cast<char>(tag));
    AppendVarint(out, bytes.size());
    out.append(bytes.data(), bytes.size());
}

// 解码侧光标:截断即失败,不部分收账。
struct Cursor {
    std::string_view bytes;
    std::size_t pos = 0;

    bool AtEnd() const { return pos >= bytes.size(); }
    bool ReadVarint(std::uint64_t* out, std::string* error) {
        std::uint64_t value = 0;
        int shift = 0;
        while (true) {
            if (pos >= bytes.size()) {
                if (error != nullptr) *error = "varint truncated";
                return false;
            }
            const std::uint8_t byte = static_cast<std::uint8_t>(bytes[pos++]);
            if (shift >= 64) {
                if (error != nullptr) *error = "varint over 64 bits";
                return false;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                *out = value;
                return true;
            }
            shift += 7;
        }
    }
    bool ReadLengthDelimited(std::string_view* out, std::string* error) {
        std::uint64_t length = 0;
        if (!ReadVarint(&length, error)) {
            return false;
        }
        if (length > bytes.size() - pos) {
            if (error != nullptr) *error = "length-delimited truncated";
            return false;
        }
        *out = bytes.substr(pos, static_cast<std::size_t>(length));
        pos += static_cast<std::size_t>(length);
        return true;
    }
};

// 未知字段跳过(前向兼容):按 wire 类型吃掉载荷。
bool SkipByWireType(Cursor& cursor, std::uint8_t wire_type, std::string* error) {
    switch (wire_type) {
        case 0: {
            std::uint64_t ignored = 0;
            return cursor.ReadVarint(&ignored, error);
        }
        case 1: {  // 64-bit
            if (cursor.bytes.size() - cursor.pos < 8) {
                if (error != nullptr) *error = "fixed64 truncated";
                return false;
            }
            cursor.pos += 8;
            return true;
        }
        case 2: {
            std::string_view ignored;
            return cursor.ReadLengthDelimited(&ignored, error);
        }
        case 5: {  // 32-bit
            if (cursor.bytes.size() - cursor.pos < 4) {
                if (error != nullptr) *error = "fixed32 truncated";
                return false;
            }
            cursor.pos += 4;
            return true;
        }
        default:
            // 3/4(废弃的 group)不认:坏帧。
            if (error != nullptr) *error = "unsupported wire type";
            return false;
    }
}

}  // namespace

std::string EncodeFeishuFrame(const FeishuFrame& frame) {
    std::string out;
    out.reserve(64 + frame.headers.size() * 16 +
                (frame.payload.has_value() ? frame.payload->size() : 0));
    // 字段 1-4 恒写(required 语义:零值也占 tag+0x00,服务端不因缺字段判坏)。
    out.push_back(static_cast<char>(kTagSeqId));
    AppendVarint(out, frame.seq_id);
    out.push_back(static_cast<char>(kTagLogId));
    AppendVarint(out, frame.log_id);
    out.push_back(static_cast<char>(kTagService));
    // service/method 是 int32:非负值就是普通 varint(负数在协议里不出现,
    // 设计单钉死 method 0/1、service 是 service_id 数值)。
    AppendVarint(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(frame.service)));
    out.push_back(static_cast<char>(kTagMethod));
    AppendVarint(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(frame.method)));
    for (const FeishuFrameHeader& header : frame.headers) {
        std::string encoded_header;
        encoded_header.push_back(static_cast<char>(kTagHeaderKey));
        AppendVarint(encoded_header, header.key.size());
        encoded_header.append(header.key);
        encoded_header.push_back(static_cast<char>(kTagHeaderValue));
        AppendVarint(encoded_header, header.value.size());
        encoded_header.append(header.value);
        AppendLengthDelimited(out, kTagHeaders, encoded_header);
    }
    if (frame.payload_encoding.has_value()) {
        AppendLengthDelimited(out, kTagPayloadEncoding, *frame.payload_encoding);
    }
    if (frame.payload_type.has_value()) {
        AppendLengthDelimited(out, kTagPayloadType, *frame.payload_type);
    }
    if (frame.payload.has_value()) {
        AppendLengthDelimited(out, kTagPayload, *frame.payload);
    }
    if (frame.log_id_new.has_value()) {
        AppendLengthDelimited(out, kTagLogIdNew, *frame.log_id_new);
    }
    return out;
}

std::optional<FeishuFrame> DecodeFeishuFrame(std::string_view bytes, std::string* error) {
    if (error != nullptr) error->clear();
    if (bytes.empty()) {
        if (error != nullptr) *error = "frame empty";
        return std::nullopt;
    }
    if (bytes.size() > kFeishuMaxFrameBytes) {
        if (error != nullptr) *error = "frame over cap";
        return std::nullopt;
    }
    FeishuFrame frame;
    bool has_seq_id = false;
    bool has_log_id = false;
    bool has_service = false;
    bool has_method = false;
    Cursor cursor{bytes};
    while (!cursor.AtEnd()) {
        std::uint64_t tag_value = 0;
        if (!cursor.ReadVarint(&tag_value, error)) {
            return std::nullopt;
        }
        const std::uint32_t field = static_cast<std::uint32_t>(tag_value >> 3);
        const std::uint8_t wire_type = static_cast<std::uint8_t>(tag_value & 0x07);
        switch (field) {
            case 1: {
                if (wire_type != 0) {
                    if (error != nullptr) *error = "field 1 wire type mismatch";
                    return std::nullopt;
                }
                std::uint64_t value = 0;
                if (!cursor.ReadVarint(&value, error)) {
                    return std::nullopt;
                }
                frame.seq_id = value;
                has_seq_id = true;
                break;
            }
            case 2: {
                if (wire_type != 0) {
                    if (error != nullptr) *error = "field 2 wire type mismatch";
                    return std::nullopt;
                }
                std::uint64_t value = 0;
                if (!cursor.ReadVarint(&value, error)) {
                    return std::nullopt;
                }
                frame.log_id = value;
                has_log_id = true;
                break;
            }
            case 3: {
                if (wire_type != 0) {
                    if (error != nullptr) *error = "field 3 wire type mismatch";
                    return std::nullopt;
                }
                std::uint64_t value = 0;
                if (!cursor.ReadVarint(&value, error)) {
                    return std::nullopt;
                }
                // int32:负数按 10 字节补码回填(static_cast 即两补码语义)。
                frame.service = static_cast<std::int32_t>(value);
                has_service = true;
                break;
            }
            case 4: {
                if (wire_type != 0) {
                    if (error != nullptr) *error = "field 4 wire type mismatch";
                    return std::nullopt;
                }
                std::uint64_t value = 0;
                if (!cursor.ReadVarint(&value, error)) {
                    return std::nullopt;
                }
                frame.method = static_cast<std::int32_t>(value);
                has_method = true;
                break;
            }
            case 5: {
                if (wire_type != 2) {
                    if (error != nullptr) *error = "field 5 wire type mismatch";
                    return std::nullopt;
                }
                std::string_view encoded_header;
                if (!cursor.ReadLengthDelimited(&encoded_header, error)) {
                    return std::nullopt;
                }
                // Header 子消息:key/value 皆 required,缺即坏帧。
                FeishuFrameHeader header;
                bool has_key = false;
                bool has_value = false;
                Cursor header_cursor{encoded_header};
                while (!header_cursor.AtEnd()) {
                    std::uint64_t header_tag = 0;
                    if (!header_cursor.ReadVarint(&header_tag, error)) {
                        return std::nullopt;
                    }
                    const std::uint32_t header_field =
                        static_cast<std::uint32_t>(header_tag >> 3);
                    const std::uint8_t header_wire =
                        static_cast<std::uint8_t>(header_tag & 0x07);
                    std::string_view piece;
                    if (header_field == 1 && header_wire == 2) {
                        if (!header_cursor.ReadLengthDelimited(&piece, error)) {
                            return std::nullopt;
                        }
                        header.key.assign(piece.data(), piece.size());
                        has_key = true;
                    } else if (header_field == 2 && header_wire == 2) {
                        if (!header_cursor.ReadLengthDelimited(&piece, error)) {
                            return std::nullopt;
                        }
                        header.value.assign(piece.data(), piece.size());
                        has_value = true;
                    } else if (!SkipByWireType(header_cursor, header_wire, error)) {
                        return std::nullopt;
                    }
                }
                if (!has_key || !has_value) {
                    if (error != nullptr) *error = "header missing key or value";
                    return std::nullopt;
                }
                frame.headers.push_back(std::move(header));
                break;
            }
            case 6:
            case 7:
            case 8:
            case 9: {
                if (wire_type != 2) {
                    if (error != nullptr) *error = "optional field wire type mismatch";
                    return std::nullopt;
                }
                std::string_view piece;
                if (!cursor.ReadLengthDelimited(&piece, error)) {
                    return std::nullopt;
                }
                std::string value(piece.data(), piece.size());
                if (field == 6) {
                    frame.payload_encoding = std::move(value);
                } else if (field == 7) {
                    frame.payload_type = std::move(value);
                } else if (field == 8) {
                    frame.payload = std::move(value);
                } else {
                    frame.log_id_new = std::move(value);
                }
                break;
            }
            default:
                // 未知字段号:跳过(前向兼容)。
                if (!SkipByWireType(cursor, wire_type, error)) {
                    return std::nullopt;
                }
                break;
        }
    }
    // 设计单 §5.3:解析侧 1-4 缺失即坏帧。
    if (!has_seq_id || !has_log_id || !has_service || !has_method) {
        if (error != nullptr) {
            *error = "frame missing required field (1-4)";
        }
        return std::nullopt;
    }
    return frame;
}

std::string FeishuFrameHeaderValue(const FeishuFrame& frame, std::string_view key) {
    for (const FeishuFrameHeader& header : frame.headers) {
        if (header.key == key) {
            return header.value;
        }
    }
    return std::string();
}

}  // namespace lubancode::channel::feishu
