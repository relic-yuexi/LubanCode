// 飞书 pbbp2 帧编解码册(飞书/企微设计单 F1,§5.3):字节样例直钉。
// 字段号与 wire 格式是设计单钉死的真源——这里的手算字节就是协议合同:
//   Frame{SeqID=1 uint64, LogID=2 uint64, service=3 int32, method=4 int32,
//         headers=5 repeated Header, payload_encoding=6 string?,
//         payload_type=7 string?, payload=8 bytes?, LogIDNew=9 string?}
//   Header{key=1 string, value=2 string}(皆 required)
// 解析纪律:1-4 缺失即坏帧;未知字段号跳过;wire 类型不匹配即坏帧。
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "channel/feishu/feishu_frame.hpp"

namespace lubancode::channel::feishu {
namespace {

// 字节串比较用十六进制拼法,断言失败时 doctest 打印的字符串可读。
std::string Hex(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

TEST_CASE("feishu_frame: 最小控制帧字节样例(SeqID/LogID/service/method + 单 header)") {
    FeishuFrame frame;
    frame.seq_id = 1;
    frame.log_id = 2;
    frame.service = 25;
    frame.method = kFrameMethodControl;
    frame.headers.push_back(FeishuFrameHeader{kHeaderType, kHeaderValuePing});
    const std::string bytes = EncodeFeishuFrame(frame);
    // 手算:
    //   08 01            字段1(SeqID=1, varint)
    //   10 02            字段2(LogID=2)
    //   18 19            字段3(service=25)
    //   20 00            字段4(method=0;零值也写——required 语义)
    //   2a 0c            字段5(Header 子消息,12 字节)
    //     0a 04 "type"   Header.key
    //     12 04 "ping"   Header.value
    CHECK(Hex(bytes) == "0801100218192000"
                        "2a0c"
                        "0a0474797065"
                        "120470696e67");
    // 解回:逐字段对上。
    std::string error;
    const auto decoded = DecodeFeishuFrame(bytes, &error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq_id == 1);
    CHECK(decoded->log_id == 2);
    CHECK(decoded->service == 25);
    CHECK(decoded->method == 0);
    REQUIRE(decoded->headers.size() == 1);
    CHECK(decoded->headers[0].key == "type");
    CHECK(decoded->headers[0].value == "ping");
    CHECK_FALSE(decoded->payload.has_value());
    CHECK(error.empty());
}

TEST_CASE("feishu_frame: 多字节 varint(SeqID=300)与数据帧全字段") {
    FeishuFrame frame;
    frame.seq_id = 300;  // varint 两字节:0xAC 0x02
    frame.log_id = 0;    // 零值也编码
    frame.service = 7;
    frame.method = kFrameMethodData;
    frame.headers.push_back(FeishuFrameHeader{"type", "event"});
    frame.headers.push_back(FeishuFrameHeader{"message_id", "om_123"});
    frame.payload_encoding = std::string("json");
    frame.payload_type = std::string("event");
    frame.payload = std::string("{\"schema\":\"2.0\"}");
    frame.log_id_new = std::string("log-new-1");
    const std::string bytes = EncodeFeishuFrame(frame);
    // 头四个字段的手算:08 ac 02 | 10 00 | 18 07 | 20 01(9 字节)。
    CHECK(Hex(bytes.substr(0, 9)) == "08ac02100018072001");
    std::string error;
    const auto decoded = DecodeFeishuFrame(bytes, &error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq_id == 300);
    CHECK(decoded->log_id == 0);
    CHECK(decoded->service == 7);
    CHECK(decoded->method == 1);
    REQUIRE(decoded->headers.size() == 2);
    CHECK(decoded->headers[1].key == "message_id");
    CHECK(decoded->headers[1].value == "om_123");
    REQUIRE(decoded->payload.has_value());
    CHECK(*decoded->payload == "{\"schema\":\"2.0\"}");
    REQUIRE(decoded->payload_encoding.has_value());
    CHECK(*decoded->payload_encoding == "json");
    REQUIRE(decoded->log_id_new.has_value());
    CHECK(*decoded->log_id_new == "log-new-1");
}

TEST_CASE("feishu_frame: 往返一致(编码确定性;含二进制 payload)") {
    FeishuFrame frame;
    frame.seq_id = 18446744073709551615ULL;  // uint64 上限(10 字节 varint)
    frame.log_id = 42;
    frame.service = 0;
    frame.method = kFrameMethodData;
    frame.headers.push_back(FeishuFrameHeader{"sum", "2"});
    frame.payload = std::string("\x00\x01\xff binary \xfe", 12);
    const std::string once = EncodeFeishuFrame(frame);
    const std::string twice = EncodeFeishuFrame(frame);
    CHECK(Hex(once) == Hex(twice));  // 确定性
    std::string error;
    const auto decoded = DecodeFeishuFrame(once, &error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq_id == 18446744073709551615ULL);
    REQUIRE(decoded->payload.has_value());
    CHECK(*decoded->payload == std::string("\x00\x01\xff binary \xfe", 12));
    CHECK(EncodeFeishuFrame(*decoded) == once);  // 再编码字节不变
}

TEST_CASE("feishu_frame: 坏帧——缺 1-4 任一字段即拒;全零帧合法") {
    // 手拼缺字段的样本(字段 1-4 各缺一次;含 \x00,须带显式长度)。
    const std::string missing_seq = std::string("\x10\x02\x18\x03\x20\x00", 6);   // 只有 2/3/4
    const std::string missing_log = std::string("\x08\x01\x18\x03\x20\x00", 6);   // 只有 1/3/4
    const std::string missing_service = std::string("\x08\x01\x10\x02\x20\x00", 6); // 只有 1/2/4
    const std::string missing_method = std::string("\x08\x01\x10\x02\x18\x03", 6);  // 只有 1/2/3
    std::string error;
    for (const std::string& bad : {missing_seq, missing_log, missing_service,
                                   missing_method}) {
        error.clear();
        const auto decoded = DecodeFeishuFrame(bad, &error);
        CHECK_FALSE(decoded.has_value());
        CHECK(error.find("missing required field") != std::string::npos);
    }
    // 对照:全零帧(编码器恒写 1-4,零值也占 tag+0x00)合法。
    error.clear();
    CHECK(DecodeFeishuFrame(EncodeFeishuFrame(FeishuFrame{}), &error).has_value());
    // 空输入。
    error.clear();
    CHECK_FALSE(DecodeFeishuFrame("", &error).has_value());
    CHECK(error == "frame empty");
}

TEST_CASE("feishu_frame: 坏帧——截断/wire 类型错/Header 缺字段") {
    std::string error;
    // 截断:varint 半截。
    CHECK_FALSE(DecodeFeishuFrame(std::string("\x08\x80", 2), &error).has_value());
    // 截断:length-delimited 长度超过剩余字节(字面量拼接断开十六进制转义)。
    CHECK_FALSE(DecodeFeishuFrame(std::string("\x42\x05" "ab", 3), &error).has_value());
    // wire 类型错:字段 1 用了 length-delimited(tag 0x0A)。
    CHECK_FALSE(DecodeFeishuFrame(std::string("\x0a\x01\x01", 3), &error).has_value());
    CHECK(error.find("wire type mismatch") != std::string::npos);
    // Header 子消息缺 value(只有 key,子消息 6 字节)。
    const std::string header_missing_value =
        std::string("\x2a\x06", 2) + std::string("\x0a\x04type", 6);
    CHECK_FALSE(DecodeFeishuFrame(header_missing_value, &error).has_value());
    CHECK(error.find("header missing key or value") != std::string::npos);
    // Header 子消息缺 key(只有 value,子消息 6 字节)。
    const std::string header_missing_key =
        std::string("\x2a\x06", 2) + std::string("\x12\x04ping", 6);
    CHECK_FALSE(DecodeFeishuFrame(header_missing_key, &error).has_value());
}

TEST_CASE("feishu_frame: 未知字段号跳过(前向兼容)") {
    // 字段 1-4 齐全,夹一个未知 varint 字段(tag 0x78 = field 15)与一个
    // 未知 length-delimited 字段(tag 0x82 0x01 = field 16)——解析不报错,
    // 已知字段照常解出。
    const std::string bytes =
        std::string("\x08\x01\x10\x02\x18\x19\x20\x00", 8) +  // 1-4
        std::string("\x78\x2a", 2) +                            // field 15 varint 42
        std::string("\x82\x01\x03xyz", 6);                      // field 16 bytes "xyz"(6 字节)
    std::string error;
    const auto decoded = DecodeFeishuFrame(bytes, &error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq_id == 1);
    CHECK(decoded->log_id == 2);
    CHECK(decoded->service == 25);
    CHECK(decoded->method == 0);
    CHECK(decoded->headers.empty());
}

TEST_CASE("feishu_frame: 头键取值便捷口") {
    FeishuFrame frame;
    frame.headers = {{"type", "event"}, {"message_id", "om_1"}, {"type", "later"}};
    CHECK(FeishuFrameHeaderValue(frame, "type") == "event");   // 首个命中
    CHECK(FeishuFrameHeaderValue(frame, "message_id") == "om_1");
    CHECK(FeishuFrameHeaderValue(frame, "absent").empty());
}

}  // namespace
}  // namespace lubancode::channel::feishu
