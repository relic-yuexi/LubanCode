// protocol.hpp 的实现:编帧、分帧缓冲、消息解析。纯逻辑,无 IO。

#include "ptc/protocol.hpp"

#include <cstring>

namespace lubancode::ptc {

namespace {

// 读 4 字节小端无符号整数。bytes 不足 4 字节的行为由调用方(先判长度)
// 保证不发生。
std::uint32_t ReadLe32(const char* bytes) {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8U) |
           (static_cast<std::uint32_t>(raw[2]) << 16U) | (static_cast<std::uint32_t>(raw[3]) << 24U);
}

void WriteLe32(char* bytes, std::uint32_t value) {
    auto* raw = reinterpret_cast<unsigned char*>(bytes);
    raw[0] = static_cast<unsigned char>(value & 0xFFU);
    raw[1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    raw[2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    raw[3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
}

// Fail 阶段的合法值(其它阶段名按协议错拒掉,逼着脚本侧如实报账)。
bool IsValidFailStage(const std::string& stage) {
    return stage == "syntax" || stage == "import" || stage == "runtime" || stage == "guard" || stage == "rpc";
}

}  // namespace

std::string EncodeFrame(std::string_view payload) {
    std::string frame;
    frame.resize(kFrameHeaderBytes + payload.size());
    WriteLe32(frame.data(), static_cast<std::uint32_t>(payload.size()));
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
    return frame;
}

std::expected<void, std::string> FrameDecoder::Feed(std::string_view bytes, std::vector<std::string>& out_frames) {
    buffer_.append(bytes.data(), bytes.size());
    while (true) {
        if (buffer_.size() < kFrameHeaderBytes) {
            return {};  // 连长度头都没攒齐,等下一段
        }
        const std::uint32_t length = ReadLe32(buffer_.data());
        if (length > kMaxFramePayloadBytes) {
            return std::unexpected("帧负载 " + std::to_string(length) + " 字节超过协议上限 " +
                                   std::to_string(kMaxFramePayloadBytes));
        }
        if (buffer_.size() < kFrameHeaderBytes + length) {
            return {};  // 负载没攒齐
        }
        out_frames.emplace_back(buffer_, kFrameHeaderBytes, length);
        buffer_.erase(0, kFrameHeaderBytes + length);
    }
}

std::expected<GuestMessage, std::string> ParseGuestMessage(std::string_view payload) {
    nlohmann::json parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected("帧负载不是合法 JSON");
    }
    if (!parsed.is_object() || !parsed.contains("type") || !parsed["type"].is_string()) {
        return std::unexpected("帧负载缺少 type 字段");
    }
    const std::string type = parsed["type"].get<std::string>();
    GuestMessage message;
    if (type == "hello") {
        message.kind = GuestMessage::Kind::Hello;
        if (parsed.contains("protocol") && parsed["protocol"].is_number_unsigned()) {
            message.protocol = parsed["protocol"].get<std::uint32_t>();
        }
        if (parsed.contains("python") && parsed["python"].is_string()) {
            message.python = parsed["python"].get<std::string>();
        }
        return message;
    }
    if (type == "call") {
        message.kind = GuestMessage::Kind::Call;
        if (!parsed.contains("id") || !parsed["id"].is_number_unsigned()) {
            return std::unexpected("call 帧缺少合法 id");
        }
        message.id = parsed["id"].get<std::uint64_t>();
        if (!parsed.contains("tool") || !parsed["tool"].is_string()) {
            return std::unexpected("call 帧缺少合法 tool");
        }
        message.tool = parsed["tool"].get<std::string>();
        if (parsed.contains("input")) {
            if (!parsed["input"].is_object()) {
                return std::unexpected("call 帧的 input 不是对象");
            }
            message.input = parsed["input"];
        } else {
            message.input = nlohmann::json::object();
        }
        return message;
    }
    if (type == "emit") {
        message.kind = GuestMessage::Kind::Emit;
        if (!parsed.contains("value")) {
            return std::unexpected("emit 帧缺少 value");
        }
        message.value = parsed["value"];
        return message;
    }
    if (type == "fail") {
        message.kind = GuestMessage::Kind::Fail;
        if (!parsed.contains("stage") || !parsed["stage"].is_string() || !IsValidFailStage(parsed["stage"])) {
            return std::unexpected("fail 帧缺少合法 stage(须是 syntax/import/runtime/guard/rpc)");
        }
        message.stage = parsed["stage"].get<std::string>();
        if (parsed.contains("error") && parsed["error"].is_string()) {
            message.error = parsed["error"].get<std::string>();
        }
        if (parsed.contains("traceback") && parsed["traceback"].is_string()) {
            message.traceback = parsed["traceback"].get<std::string>();
        }
        return message;
    }
    if (type == "done") {
        message.kind = GuestMessage::Kind::Done;
        if (parsed.contains("captured_stdout") && parsed["captured_stdout"].is_string()) {
            message.captured_stdout = parsed["captured_stdout"].get<std::string>();
        }
        if (parsed.contains("calls") && parsed["calls"].is_number_unsigned()) {
            message.calls = parsed["calls"].get<std::uint64_t>();
        }
        return message;
    }
    return std::unexpected("未知帧类型: " + type);
}

std::string BuildResultPayload(std::uint64_t id, bool ok, const nlohmann::json& value, const std::string& error) {
    nlohmann::json payload = nlohmann::json::object();
    payload["type"] = "result";
    payload["id"] = id;
    payload["ok"] = ok;
    if (ok) {
        payload["value"] = value;
    } else {
        payload["error"] = error;
    }
    return payload.dump();
}

std::string BuildAbortPayload(const std::string& reason) {
    nlohmann::json payload = nlohmann::json::object();
    payload["type"] = "abort";
    payload["reason"] = reason;
    return payload.dump();
}

}  // namespace lubancode::ptc
