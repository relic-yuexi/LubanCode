// PTC(Programmatic Tool Calling) framed stdin/stdout RPC 协议。
//
// 脚本不是直接摸宿主 C++ 对象:Python 子进程与宿主之间只走这一条窄协议
// (规格"脚本不是直接摸宿主"节)。帧格式:
//
//   [4 字节小端长度 N][N 字节 UTF-8 JSON]
//
// 消息方向与类型:
//   Python -> 宿主:
//     {"type":"hello","protocol":1,"python":"3.11.8"}        启动握手
//     {"type":"call","id":7,"tool":"read_file","input":{...}}  一枚 stub 调用
//     {"type":"emit","value":{...}}                            脚本最终摘要
//     {"type":"fail","stage":"syntax|import|runtime|guard|rpc","error":"...",
//      "traceback":"..."}                                      脚本自身失败
//   宿主 -> Python:
//     {"type":"result","id":7,"ok":true,"value":{...}}         调用结果
//     {"type":"result","id":7,"ok":false,"error":"..."}        调用失败/被拒
//     {"type":"abort","reason":"..."}                          宿主要脚本收场
//
// 纯逻辑,不碰 IO:编帧/解帧/消息解析都可在单测里钉死。宿主侧的分帧缓冲
// 与子进程管道在 runner 里。

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::ptc {

// 协议版本号。握手时不一致按协议错处理(熔断器认这个)。
inline constexpr std::uint32_t kProtocolVersion = 1;

// 帧头长度(字节)。
inline constexpr std::size_t kFrameHeaderBytes = 4;

// 单帧负载上限。脚本侧 emit 大结果应落盘(P2),不靠超大帧硬扛;超限帧
// 直接判协议错,不让一段坏输出把宿主内存吃光。
inline constexpr std::size_t kMaxFramePayloadBytes = 32 * 1024 * 1024;

// 编一帧:长度头 + JSON 文本。payload 是完整的 JSON 文本(调用方保证)。
std::string EncodeFrame(std::string_view payload);

// 解帧器的纯逻辑:喂进任意字节块,吐出完整帧的负载。内部缓冲跨调用保留,
// 半截帧攒着等下一段。解析失败(长度头超上限、JSON 不完整)返回错误。
class FrameDecoder {
public:
    // 喂一段原始字节。每识别出一帧就 append 到 out_frames。
    std::expected<void, std::string> Feed(std::string_view bytes, std::vector<std::string>& out_frames);

    std::size_t buffered() const { return buffer_.size(); }

private:
    std::string buffer_;
};

// Python -> 宿主的消息(解析后的形状)。
struct GuestMessage {
    enum class Kind { Hello, Call, Emit, Fail, Done };
    Kind kind = Kind::Hello;
    std::uint64_t id = 0;         // Call:调用号
    std::string tool;             // Call:工具名
    nlohmann::json input;         // Call:入参对象
    nlohmann::json value;         // Emit:最终摘要值
    std::string stage;            // Fail:syntax/import/runtime/guard/rpc
    std::string error;            // Fail:错误说明
    std::string traceback;        // Fail:原始 traceback(截尾)
    std::uint32_t protocol = 0;   // Hello:协议版本
    std::string python;           // Hello:解释器版本串
    std::string captured_stdout;  // Done:脚本 print 捕获(截尾)
    std::uint64_t calls = 0;      // Done:脚本侧发起的调用数
};

// 解析一帧负载(完整 JSON 文本)。认不出 type/字段形状不对返回错误。
std::expected<GuestMessage, std::string> ParseGuestMessage(std::string_view payload);

// 宿主 -> Python:result 帧负载。
std::string BuildResultPayload(std::uint64_t id, bool ok, const nlohmann::json& value,
                               const std::string& error);

// 宿主 -> Python:abort 帧负载(Esc 取消/撞线收场)。
std::string BuildAbortPayload(const std::string& reason);

}  // namespace lubancode::ptc
