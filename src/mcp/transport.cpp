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
        // 每条凑齐的完整行都在吐出前核界:上限管的是行本身,不看它跟换行
        // 是不是同一批到的(旧实现只查残缓冲,超限行恰与换行同批到达就
        // 整个放行,准入随管道切片漂移)。
        if (line.size() > kMaxLineBytes) {
            overflowed_ = true;
            buffer_.clear();
            buffer_.shrink_to_fit();
            // 这一批先前已凑齐的合规行照常交出去,超限行起头的残句丢弃。
            return out;
        }
        out.emplace_back(line);
        start = newline_pos + 1;
    }
    buffer_.erase(0, start);

    // 残行迟迟不见换行、还越攒越大:按"最小可能行长"核界——末位 \r 若恰
    // 是行尾,凑齐时会被剥掉,先少算一字节,保证逐字节喂与整片喂同一结论。
    std::size_t min_pending = buffer_.size();
    if (min_pending > 0 && buffer_.back() == '\r') {
        --min_pending;
    }
    if (min_pending > kMaxLineBytes) {
        // 残行注定超限:协议不对劲,报废,别把内存吃光。这一批已经凑齐的
        // 完整行照常交出去,残行丢弃。
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
                                            std::function<void(std::string)> on_line,
                                            platform::EnvMode env_mode) {
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
        },
        /*cwd_utf8=*/std::string(), env_mode);

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
