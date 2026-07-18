#include "lsp/transport.hpp"

#include <cctype>

namespace lubancode::lsp {

namespace {

// 在一整块头部文本(不含结尾的 \r\n\r\n)里找 Content-Length 的值。
// 头名大小写不敏感,冒号后允许空格。找不到/不是数字返回 -1。
long long ParseContentLength(std::string_view header_block) {
    std::size_t pos = 0;
    while (pos <= header_block.size()) {
        std::size_t line_end = header_block.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            line_end = header_block.size();
        }
        std::string_view line = header_block.substr(pos, line_end - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string name(line.substr(0, colon));
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (name == "content-length") {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                    value.remove_suffix(1);
                }
                if (value.empty()) {
                    return -1;
                }
                long long out = 0;
                for (const char c : value) {
                    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                        return -1;
                    }
                    out = out * 10 + (c - '0');
                }
                return out;
            }
        }
        if (line_end >= header_block.size()) {
            break;
        }
        pos = line_end + 2;
    }
    return -1;
}

}  // namespace

std::vector<std::string> ContentLengthFramer::Feed(std::string_view chunk) {
    buffer_.append(chunk);

    std::vector<std::string> out;
    while (true) {
        if (!in_body_) {
            // 正在等头:头部块以 \r\n\r\n 收尾。
            const std::size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                break;  // 头还没到齐,残包留缓冲
            }
            const long long length = ParseContentLength(std::string_view(buffer_.data(), header_end));
            buffer_.erase(0, header_end + 4);
            if (length < 0) {
                // 坏头(没有 Content-Length/不是数字):丢掉这块头,继续找
                // 下一条,不把整条流搞死。
                continue;
            }
            expected_ = static_cast<std::size_t>(length);
            in_body_ = true;
        }
        // 正在攒正文。
        if (buffer_.size() < expected_) {
            break;  // 正文还没到齐,残包留缓冲
        }
        out.push_back(buffer_.substr(0, expected_));
        buffer_.erase(0, expected_);
        expected_ = 0;
        in_body_ = false;
    }
    return out;
}

StdioTransport::~StdioTransport() {
    Shutdown(2000);
}

TransportStartResult StdioTransport::Start(const std::string& command, const std::vector<std::string>& args,
                                            std::function<void(std::string)> on_message) {
    on_message_ = std::move(on_message);

    const platform::SpawnResult spawn = child_.Start(
        command, args, {},
        // stdout 读线程:按 Content-Length 分帧,逐条上交。
        [this](std::string_view chunk) {
            std::vector<std::string> messages = framer_.Feed(chunk);
            for (auto& message : messages) {
                if (on_message_) {
                    on_message_(std::move(message));
                }
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
        // 可执行文件不存在单独给一句人话(用户十有八九是没装/没配 PATH),
        // 跟老版本 ERROR_FILE_NOT_FOUND/ERROR_PATH_NOT_FOUND 分支同语义。
        if (spawn.command_not_found) {
            return TransportStartResult{false, "未找到命令 " + command + "(" + spawn.error + ")"};
        }
        return TransportStartResult{false, spawn.error};
    }
    return TransportStartResult{true, std::string()};
}

bool StdioTransport::WriteMessage(const std::string& body) {
    const std::string payload = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return child_.Write(payload);
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

}  // namespace lubancode::lsp
