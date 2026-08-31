// app-server WebSocket 承载的纯函数层:RFC 6455 的握手算料与帧编解码。
// 不碰 socket、不碰线程——ws_transport 只管把字节搬进来搬出去,这边只认
// 字节形状,单测可以裸跑。
//
// 服务端立场(只做被连的一方):
//   - 握手:吃一段 HTTP 升级请求字节,吐 101 应答字节(Sec-WebSocket-Accept
//     = base64(SHA1(key + GUID)),SHA-1 是协议算料不是安全边界——鉴权走
//     token 门,见 ws_transport)。
//   - 入帧:客户端帧必须掩码,这里解掩;支持分片(continuation 拼装)、
//     ping(回 pong 由 transport 做)、close;binary 帧与 rsv 位一律按协议
//     错收线——本协议只有文本帧(一条 JSON 一帧,与 stdio 的一行一条对齐)。
//   - 出帧:服务端帧不掩码,text/close/pong 三种。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::app_server::ws {

// ---- 基础算料 ----

// SHA-1 摘要(RFC 3174;只为 Sec-WebSocket-Accept 算料,不做安全用途)。
std::array<std::uint8_t, 20> Sha1Digest(std::string_view bytes);

// 标准 base64(带填充)。只编不解——入站方向不需要。
std::string Base64Encode(std::string_view bytes);

// Sec-WebSocket-Key -> Sec-WebSocket-Accept(RFC 6455 §1.3 的 GUID 法)。
std::string ComputeAcceptKey(std::string_view client_key);

// ---- 握手 ----

struct UpgradeParseResult {
    bool valid = false;
    std::string websocket_key;  // Sec-WebSocket-Key 的值(原文)
    std::string error;          // 不合法时的人话(不含请求正文,可进日志)
};

// 解一段完整的 HTTP 升级请求字节(以 \r\n\r\n 收尾)。要求:GET、
// Upgrade 含 websocket、Connection 含 upgrade、Sec-WebSocket-Version: 13、
// Sec-WebSocket-Key 在。头部名不区分大小写,字段值两侧空白宽容。
UpgradeParseResult ParseUpgradeRequest(std::string_view request_bytes);

// 101 应答整包(不含后续帧)。
std::string MakeUpgradeResponse(std::string_view accept_key);

// ---- HTTP 只读面(多前端外壳单阶段 D:artifact 字节口子) ----
//
// WS 端口的升级握手本来就是一段 HTTP,同一只监听顺手认一类普通 GET:
// `GET /artifact/<内容寻址名>`——镜像帧/截图在协议事件里只有 artifact
// 引用(base64 永不进协议),Web 外壳要有取字节的口子才画得出图。这不是
// 协议方法面(与 app_server/auth 同属承载面),报文形状零改动,不 bump
// 版本。除这一类 GET 外,其余非升级请求照旧 400。

// 普通请求头解析(只服务上面那一类 GET,别的方法不认)。头部名不区分
// 大小写;值两侧空白宽容。坏形状(不是请求行开头/没有行尾)给空 method,
// 调用方按"非 GET"落 400。
struct HttpRequestHead {
    std::string method;        // 请求行的方法(原文,约定大写)
    std::string target;        // 目标路径(不含查询串)
    std::string query;         // 查询串(去开头的 ?;没有则空)
    std::string bearer_token;  // Authorization: Bearer <token>(没有则空)
};
HttpRequestHead ParseHttpRequestHead(std::string_view request_bytes);

// 查询串取参数值(?token=... 用;百分号解码,%XX 之外的 % 原样保留)。
std::string QueryParam(const std::string& query, std::string_view name);

// 内容寻址 artifact 名:art-<十六进制 8..64 位>.(png|jpeg|jpg)。只认这
// 一个形状——路径分隔、别的扩展名、百分号编码一律不认,穿越无从谈起。
bool IsValidArtifactName(std::string_view name);

// 简单 HTTP 应答:状态行 + Connection: close + Content-Type/Length + 可选
// 头(每行自带 \r\n)+ 正文字节。只给只读面用。
std::string MakeHttpResponse(std::string_view status_line, std::string_view content_type,
                             std::string_view body, std::string_view extra_headers = {});

// ---- 帧 ----

inline constexpr std::size_t kMaxMessageBytes = 8 * 1024 * 1024;  // 与 LineFramer 上限对齐

// 出帧(服务端不掩码)。
std::string MakeTextFrame(std::string_view payload);
std::string MakeCloseFrame(std::uint16_t code);
std::string MakePongFrame(std::string_view payload);

// 入帧的解码产出。一条 Feed 可能吐多条(挤包)也可能一条不吐(残帧)。
struct FrameEvent {
    enum class Kind { Text, Ping, Close, Error };
    Kind kind = Kind::Text;
    std::string payload;  // Text:整条消息(分片已拼);Ping:载荷;Close:可选
    std::string reason;   // Error 时的人话(可进日志,不含载荷正文)
};

// 增量解码器:喂字节,吐事件。协议错(掩码缺失、rsv、binary、分片乱序、
// 超长)后报废——后续 Feed 一律空返回,transport 据此收线。
class FrameDecoder {
public:
    std::vector<FrameEvent> Feed(std::string_view chunk);
    bool failed() const { return failed_; }

private:
    FrameEvent Fail(std::string reason);

    std::string buffer_;
    bool frag_pending_ = false;   // 有 fin=0 的起帧在拼
    std::string frag_payload_;    // 拼到一半的正文
    bool failed_ = false;
};

}  // namespace lubancode::app_server::ws
