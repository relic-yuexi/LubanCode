// 飞书长连接 pbbp2 帧编解码(飞书/企微设计单 F1,§5.3):纯函数,零 IO。
//
// 帧是 protobuf(pbbp2),字段号设计单已钉死:
//   Header { key=1 string, value=2 string }            // 皆 required
//   Frame  { SeqID=1 uint64, LogID=2 uint64, service=3 int32,
//            method=4 int32,                            // 0=控制帧 1=数据帧
//            headers=5 repeated Header,
//            payload_encoding=6 string?, payload_type=7 string?,
//            payload=8 bytes?, LogIDNew=9 string? }
//
// varint / length-delimited 标准 wire 格式,自实现编解码(照 ws_frame 的
// 纯函数风格 + 字节样例单测);真源是 larksuite/oapi-sdk-go v3_main 的
// ws/pbbp2.pb.go,实现按设计单钉死的字段表,不引 protobuf 库。
//
// 解析纪律:1-4 缺失即坏帧(设计单 §5.3);未知字段号按 wire 类型跳过
//(前向兼容——服务端加字段不解码不报错);wire 类型不匹配即坏帧。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::channel::feishu {

// method 取值(设计单 §5.3):0=控制帧(ping/pong),1=数据帧(事件/ACK)。
inline constexpr std::int32_t kFrameMethodControl = 0;
inline constexpr std::int32_t kFrameMethodData = 1;

// 帧内 headers 的键值口径(设计单 §5.4/§5.5/§5.6):控制帧 type=ping/pong;
// 数据帧 type=event/card、message_id、sum/seq、trace_id、timestamp;回执
// 加 biz_rt(处理毫秒差)。
inline constexpr char kHeaderType[] = "type";
inline constexpr char kHeaderValuePing[] = "ping";
inline constexpr char kHeaderValuePong[] = "pong";
inline constexpr char kHeaderValueEvent[] = "event";
inline constexpr char kHeaderValueCard[] = "card";
inline constexpr char kHeaderMessageId[] = "message_id";
inline constexpr char kHeaderSum[] = "sum";
inline constexpr char kHeaderSeq[] = "seq";
inline constexpr char kHeaderBizRt[] = "biz_rt";

// 单条 pbbp2 帧的字节上限:与 WS 消息帽(transport/ws_frame 8 MiB)同源
//(帧装在 WS 消息里,不另设更宽的帽)。
inline constexpr std::size_t kFeishuMaxFrameBytes = 8 * 1024 * 1024;

struct FeishuFrameHeader {
    std::string key;
    std::string value;
};

struct FeishuFrame {
    std::uint64_t seq_id = 0;   // 字段 1(required;客户端单调递增)
    std::uint64_t log_id = 0;   // 字段 2(required)
    std::int32_t service = 0;   // 字段 3(required;ping 帧填 service_id 数值)
    std::int32_t method = 0;    // 字段 4(required;kFrameMethod*)
    std::vector<FeishuFrameHeader> headers;  // 字段 5(repeated)
    std::optional<std::string> payload_encoding;  // 字段 6(可缺)
    std::optional<std::string> payload_type;      // 字段 7(可缺)
    std::optional<std::string> payload;           // 字段 8(可缺;bytes)
    std::optional<std::string> log_id_new;        // 字段 9(可缺)
};

// 编码:字段 1-4 恒写(proto2 required 语义——值 0 也写 tag+0x00,服务端按
// 缺字段判坏帧);6/7/8/9 空缺不写;headers 按序逐条写字段 5。编码结果
// 确定性(同输入同字节),供字节样例测试。
std::string EncodeFeishuFrame(const FeishuFrame& frame);

// 解析:非 object 起点、截断、wire 类型不匹配、字段 1-4 缺失一律拒绝
//(error 落 *error,不带原文字节);未知字段号跳过。重复标量字段后值胜
//(服务端行为未钉死,取宽容口径,不赌)。
std::optional<FeishuFrame> DecodeFeishuFrame(std::string_view bytes, std::string* error);

// headers 便捷取值:首个命中 key 的 value;没有返回空串。
std::string FeishuFrameHeaderValue(const FeishuFrame& frame, std::string_view key);

}  // namespace lubancode::channel::feishu
