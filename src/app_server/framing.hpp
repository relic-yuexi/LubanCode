// app-server 的 stdio 逐行分帧:跟 mcp/transport.hpp 的 LineFramer 一个
// 路数(劈包/挤包/残行/超长报废),照搬不改样——MCP 那份是给客户端连子进
// 程用的,app-server 是服务端读自己的 stdin,两头各自持有一份纯函数分帧
// 器,不互相牵扯依赖。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::app_server {

// 纯函数式增量分帧器:把陆续到达的字节流按换行切成一行一行,统一去掉
// 行尾 \r。单行累积超过 kMaxLineBytes 置 overflowed、清空缓冲、后续 Feed
// 不再产出任何行——一个不换行狂写的坏客户端不能把内存吃光,协议已不可
// 信,调用方应当回一条 parse error(尽力而为)然后退线。
class LineFramer {
public:
    static constexpr std::size_t kMaxLineBytes = 8 * 1024 * 1024;

    // 喂一段新到达的字节,返回这次新凑齐的完整行(可能 0 行、1 行、多行)。
    std::vector<std::string> Feed(std::string_view chunk);

    // 溢出标记:置位后分帧器报废。
    bool overflowed() const { return overflowed_; }

private:
    std::string buffer_;
    bool overflowed_ = false;
};

}  // namespace lubancode::app_server
