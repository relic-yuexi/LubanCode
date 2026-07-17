// 纯函数式增量分帧器:把 HTTP 响应体里陆续到达的字节流,按 SSE
// (Server-Sent Events)规则切成一帧一帧的 SseFrame。
//
// 这一层只认得 SSE 的通用语法(event: / data: / 空行分隔 / : 开头注释),
// 不碰网络、不懂 Anthropic 或者 OpenAI 的事件语义——那是上一层的事。

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lubancode::api {

// 一帧完整的 SSE 事件:事件名 + 拼好的 data 串。
struct SseFrame {
    std::string event;  // 没写 event: 字段时,按 SSE 规范默认是 "message"
    std::string data;   // 多行 data: 已经用 \n 拼接好
};

// 增量分帧器。喂多少字节都行,可以在任意位置把一条事件劈成好几段喂进来,
// 也可以一次塞进好几条事件——都能正确切帧。残句(还没凑齐一整行/一整帧的部分)
// 留在内部缓冲里,下次 feed() 接着用。
class SseFramer {
public:
    // 喂一段新到达的字节。返回这次新凑齐的完整帧(可能是 0 帧、1 帧、多帧)。
    std::vector<SseFrame> feed(std::string_view chunk);

private:
    std::string buffer_;         // 还没凑成一整行的残句
    std::string pending_event_;  // 当前正在累积的事件,event 字段
    std::string pending_data_;   // 当前正在累积的事件,data 字段(已按 \n 拼接)
    bool has_data_ = false;      // data 字段是否出现过(哪怕值是空串)

    void ProcessLine(std::string_view line, std::vector<SseFrame>& out);
    void Dispatch(std::vector<SseFrame>& out);
};

}  // namespace lubancode::api
