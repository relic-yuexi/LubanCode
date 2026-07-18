#include "mcp/transport.hpp"

namespace lubancode::mcp {

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
        // 残行迟迟不见换行、还越攒越大:协议不对劲,报废,别把内存吃光。
        // 这一批已经凑齐的完整行照常交出去,残行丢弃。
        overflowed_ = true;
        buffer_.clear();
        buffer_.shrink_to_fit();
    }

    return out;
}

StdioTransport::~StdioTransport() {
    Shutdown(2000);
}

TransportStartResult StdioTransport::Start(const std::string& command, const std::vector<std::string>& args,
                                            const std::vector<std::pair<std::string, std::string>>& env,
                                            std::function<void(std::string)> on_line) {
    on_line_ = std::move(on_line);

    const platform::SpawnResult spawn = child_.Start(
        command, args, env,
        // stdout 读线程:分帧、逐行上交;单行超上限就宣布协议报废、杀进程
        // 断连(返回 false 让读线程收工)。进程一死,等待中的请求靠 IsAlive
        // 轮询很快就能失败返回。
        [this](std::string_view chunk) {
            const std::vector<std::string> lines = line_framer_.Feed(chunk);
            for (const auto& line : lines) {
                if (on_line_) {
                    on_line_(line);
                }
            }
            if (line_framer_.overflowed()) {
                {
                    std::lock_guard<std::mutex> lock(stderr_mutex_);
                    stderr_buffer_ += "[lubancode] 服务器单行输出超过上限(8MB),协议错误,已断开";
                }
                child_.Kill();
                return false;
            }
            return true;
        },
        // stderr 读线程:环形日志缓冲,只留最近 8KB,出错时给人看够用,
        // 不无限增长。
        [this](std::string_view chunk) {
            std::lock_guard<std::mutex> lock(stderr_mutex_);
            stderr_buffer_.append(chunk);
            constexpr std::size_t kMaxStderrBytes = 8192;
            if (stderr_buffer_.size() > kMaxStderrBytes) {
                stderr_buffer_.erase(0, stderr_buffer_.size() - kMaxStderrBytes);
            }
        });

    if (!spawn.success) {
        return TransportStartResult{false, spawn.error};
    }
    return TransportStartResult{true, std::string()};
}

bool StdioTransport::WriteLine(const std::string& message) {
    return child_.Write(message + "\n");
}

void StdioTransport::Shutdown(int wait_ms) {
    child_.Shutdown(wait_ms);
}

bool StdioTransport::IsAlive() const {
    return child_.IsAlive();
}

std::string StdioTransport::StderrTail() const {
    std::lock_guard<std::mutex> lock(stderr_mutex_);
    return stderr_buffer_;
}

}  // namespace lubancode::mcp
