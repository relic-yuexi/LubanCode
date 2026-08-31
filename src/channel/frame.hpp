// Channel Bridge v1 帧编解码(多渠道消息接入单阶段 1)。
//
// 唯一真源是 docs/architecture/channels/bridge-protocol.md §1-2:4 字节
// 大端长度前缀 + UTF-8 JSON 正文,单帧上限 8 MiB,正文须是 JSON object。
// 这里只管"字节 <-> 一份 JSON 正文"的编解码与拆包/粘包/超帽/坏 UTF-8
// 处理;JSON-RPC method 语义在 bridge_protocol.hpp。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

// 单帧正文上限(bridge-protocol.md §1):8 MiB,媒体只传 ArtifactRef/文件
// 路径/URL,不塞大段 base64。
inline constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;

// 帧长度前缀字节数。
inline constexpr std::size_t kFrameHeaderBytes = 4;

enum class FrameErrorCode {
    FrameTooLarge,  // frame_too_large:单帧超 8 MiB
    InvalidUtf8,    // invalid_utf8:帧内坏 UTF-8
    InvalidFrame,   // invalid_frame:JSON 解不开或非 object
};

// 与 bridge-protocol.md §6 domain 错误稳定名对齐,供上层直接拼进
// JSON-RPC error.message。
std::string_view FrameErrorStableName(FrameErrorCode code);

struct FrameError {
    FrameErrorCode code;
    std::string message;
};

// 编码一帧:4 字节大端长度前缀(payload 序列化后的字节数)+ UTF-8 JSON
// 正文。payload 必须是 object(§2 framing 规矩),否则 InvalidFrame。
// 序列化结果超 8 MiB 报 FrameTooLarge。
std::expected<std::vector<std::byte>, FrameError> EncodeFrame(const nlohmann::json& payload);

// 增量帧解码器。设计给"stdin 一次只读到半截"的真实管道用,但本单阶段 1
// 只在纯内存字节缓冲上练——不接真进程 stdio。
//
// 用法:反复 Feed() 喂到的字节,反复调 TryDecodeNext() 取帧,直到它返回
// std::nullopt(数据不够,等下次 Feed)。一旦某次 TryDecodeNext() 返回
// FrameError,解码器进入 sticky 错误态:此后每次调用都回同一个错误,不
// 会"跳过坏帧继续往下解"——协议规矩是"协议错、超帽、连续坏帧:立即停
// adapter,进入 backoff"(§2),不是尽力容错。
class FrameDecoder {
public:
    void Feed(const std::byte* data, std::size_t size);
    void Feed(std::string_view bytes);

    std::expected<std::optional<nlohmann::json>, FrameError> TryDecodeNext();

    bool has_error() const { return error_.has_value(); }
    const std::optional<FrameError>& error() const { return error_; }

    // 缓冲区里还剩多少未消费字节(诊断/测试用)。
    std::size_t pending_bytes() const { return buffer_.size() - cursor_; }

private:
    std::vector<std::byte> buffer_;
    std::size_t cursor_ = 0;
    std::optional<FrameError> error_;

    void Compact();
};

}  // namespace lubancode::channel
