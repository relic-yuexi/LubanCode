#include "api/sse_framing.hpp"

namespace lubancode::api {

std::vector<SseFrame> SseFramer::feed(std::string_view chunk) {
    buffer_.append(chunk);

    std::vector<SseFrame> out;

    // 按 \n 把缓冲区里能凑成整行的部分都切出来处理;不管原始换行是 \r\n
    // 还是单纯 \n,行末的 \r 在切出单行之后统一去掉。凑不成整行的残句
    // (缓冲区里最后、没有 \n 收尾的那一截)留在 buffer_ 里,等下次 feed()。
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
        ProcessLine(line, out);
        start = newline_pos + 1;
    }
    buffer_.erase(0, start);

    return out;
}

void SseFramer::ProcessLine(std::string_view line, std::vector<SseFrame>& out) {
    if (line.empty()) {
        // 空行:一条事件到此结束,该派发了。
        Dispatch(out);
        return;
    }

    if (line.front() == ':') {
        // 以冒号开头的是注释行,直接跳过,不当作任何字段。
        return;
    }

    const std::size_t colon_pos = line.find(':');
    std::string_view field;
    std::string_view value;
    if (colon_pos == std::string_view::npos) {
        // 没有冒号:整行就是字段名,值是空串。
        field = line;
        value = std::string_view{};
    } else {
        field = line.substr(0, colon_pos);
        value = line.substr(colon_pos + 1);
        // SSE 规范:冒号后如果紧跟一个空格,这一个空格要去掉(只去一个,
        // 不是去掉所有前导空白)。
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }

    if (field == "data") {
        if (has_data_) {
            pending_data_.push_back('\n');
        }
        pending_data_.append(value);
        has_data_ = true;
    } else if (field == "event") {
        pending_event_.assign(value);
    }
    // 其余字段(id、retry……)M1 用不上,忽略。
}

void SseFramer::Dispatch(std::vector<SseFrame>& out) {
    if (has_data_) {
        SseFrame frame;
        frame.event = pending_event_.empty() ? std::string("message") : pending_event_;
        frame.data = pending_data_;
        out.push_back(std::move(frame));
    }
    // 不管有没有派发,都清空当前事件的累积状态,准备接收下一条事件。
    pending_event_.clear();
    pending_data_.clear();
    has_data_ = false;
}

}  // namespace lubancode::api
