// RFC 6455 WebSocket 帧编解码(QQ 机器人接入单 Q1,§十五定案自实现 WS 客户端)。
//
// 定案背景见 todo §十五:ixwebsocket/libwebsockets/libcurl-WS 各有否决理由,
// 自扛 RFC 6455 客户端子集——帧协议是其中最薄的一层,纯函数可精确测试
// (半帧/粘帧/分片/控制帧/超帽逐项 fixture)。
//
// 本文件只管帧字节,不管连接与握手:
//   - 客户端方向编码(必 mask,RFC 6455 §5.3):EncodeClientFrame;
//   - 服务端方向编码(不得 mask,§5.1):EncodeServerFrame——QQ 客户端用不
//     上,但测试里的 mock 网关服务端要发帧,同一份实现免两处各写一套;
//   - 解码只按"服务端发来的帧"验(mask=1 一律协议错,§5.1 客户端不得收
//     掩码帧);未协商扩展,RSV 位非零按协议错断连。
//
// 单条消息上限对齐 Bridge 帧帽(channel/frame.hpp kMaxFrameBytes = 8 MiB):
// QQ 网关事件是短 JSON 文本,远够;超帽按协议错处理(sticky),不是尽力容错。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::channel::qq {

// 单条 WS 消息(拼完分片后)的字节上限,与 Bridge 帧 8 MiB 同帽。
inline constexpr std::size_t kWsMaxMessageBytes = 8 * 1024 * 1024;

enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

// 帧协议错误(解码器 sticky:一次出错,后续 TryNext 恒回同一错)。
enum class WsFrameErrorKind {
    MessageTooLarge,  // 单条消息(含分片累计)超 kWsMaxMessageBytes
    ProtocolError,    // RSV/掩码方向/控制帧分片/continuation 无起点等
};

struct WsFrameError {
    WsFrameErrorKind kind = WsFrameErrorKind::ProtocolError;
    std::string detail;
};

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------

// 客户端方向帧:FIN=1,mask=1(RFC 6455 §5.3 客户端必须掩码)。mask_key 4 字节。
std::vector<std::byte> EncodeClientFrame(WsOpcode opcode, std::string_view payload,
                                         const std::uint8_t mask_key[4]);

// 服务端方向帧:FIN=1,mask=0(mock 网关服务端与文档对拍用;生产客户端不发)。
std::vector<std::byte> EncodeServerFrame(WsOpcode opcode, std::string_view payload);

// ---------------------------------------------------------------------------
// 解码 + 拼装
// ---------------------------------------------------------------------------

// 解码器吐出的完整事件:一条拼完的消息,或一枚控制帧。
struct WsFrameEvent {
    enum class Kind { Message, Ping, Pong, Close };
    Kind kind = Kind::Message;
    WsOpcode message_opcode = WsOpcode::Text;  // Kind::Message 时 Text/Binary
    std::string payload;                       // Message 正文;Ping/Pong 载荷
    std::uint16_t close_code = 0;              // Kind::Close 时;<2 字节载荷按 1005(无码)
    std::string close_reason;                  // Kind::Close 时可空
};

// 增量帧解码 + 分片拼装。Feed() 喂到的字节(半帧/粘帧/一次多帧皆可),
// 反复 TryNext() 取完整事件,直到 nullopt(字节不够,等下次 Feed)。
// 出错后 sticky(见 WsFrameError):协议规矩是断连,不是跳帧容错。
//
// 控制帧规矩(RFC 6455 §5.5):不得分片、载荷 <=125 字节;Ping/Pong 载荷
// 原样带出(客户端上层自动回 Pong,在 qq_ws_client 做,这里不自动回)。
class WsFrameDecoder {
public:
    void Feed(const std::byte* data, std::size_t size);
    void Feed(std::string_view bytes);

    std::expected<std::optional<WsFrameEvent>, WsFrameError> TryNext();

    bool has_error() const { return error_.has_value(); }
    const std::optional<WsFrameError>& error() const { return error_; }

private:
    void Fail(WsFrameErrorKind kind, std::string detail);
    // 处理一帧完整头部+载荷;返回 true = 本轮 TryNext 已产出事件。
    std::expected<std::optional<WsFrameEvent>, WsFrameError> ConsumeOne();

    std::vector<std::byte> buffer_;
    std::size_t cursor_ = 0;

    // 分片拼装状态(Kind::Message 的 continuation 链)。
    bool assembling_ = false;
    WsOpcode assemble_opcode_ = WsOpcode::Text;
    std::string assembled_;

    std::optional<WsFrameError> error_;
};

}  // namespace lubancode::channel::qq
