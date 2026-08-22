// framing.hpp 的实现:与 mcp/transport.cpp 的 LineFramer::Feed 逐字对齐
// (两平台行为必须一样,测试手法也照搬 test_mcp_transport.cpp)。
#include "app_server/framing.hpp"

namespace lubancode::app_server {

std::vector<std::string> LineFramer::Feed(std::string_view chunk) {
    if (overflowed_) {
        return {};
    }
    buffer_.append(chunk);

    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline_pos = buffer_.find('\n', start);
        if (newline_pos == std::string::npos) {
            break;
        }
        std::string_view line(buffer_.data() + start, newline_pos - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.emplace_back(line);
        start = newline_pos + 1;
    }
    buffer_.erase(0, start);

    if (buffer_.size() > kMaxLineBytes) {
        // 残行迟迟不见换行、还越攒越大:协议不对劲,报废。这批已凑齐的
        // 完整行照常交出去,残行丢弃。
        overflowed_ = true;
        buffer_.clear();
        buffer_.shrink_to_fit();
    }

    return out;
}

}  // namespace lubancode::app_server
