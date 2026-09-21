#include "lsp/transport.hpp"

#include <cctype>

namespace lubancode::lsp {

namespace {

// 在一整块头部文本(不含结尾的 \r\n\r\n)里找 Content-Length 的值。
// 头名大小写不敏感,冒号后允许空格。找不到/不是数字返回 -1。
// 数字累积带上帽:一旦越过 kMaxBodyBytes 就钳到"上限+1"当哨兵接着扫
// (只为止住漏网的非数字)——几十位的十进制串也不会把 long long 乘爆
// (旧实现逐位乘十无核界,有符号溢出是 UB)。
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
                    if (out > static_cast<long long>(ContentLengthFramer::kMaxBodyBytes)) {
                        out = static_cast<long long>(ContentLengthFramer::kMaxBodyBytes) + 1;
                    }
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
    if (overflowed_) {
        return {};
    }
    buffer_.append(chunk);

    std::vector<std::string> out;
    while (true) {
        if (!in_body_) {
            // 正在等头:头部块以 \r\n\r\n 收尾。
            const std::size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                // 头还没到齐:攒头期间同样核界,无界定界的垃圾不许无界增长。
                // 恰在上限不误判;越帽即判死,这一批先前凑齐的消息照常交出。
                if (buffer_.size() > kMaxHeaderBytes) {
                    overflowed_ = true;
                    buffer_.clear();
                    buffer_.shrink_to_fit();
                }
                break;
            }
            if (header_end + 4 > kMaxHeaderBytes) {
                // 凑齐的头块本身越帽:整片到达也判死,跟逐字节喂同一个结论。
                overflowed_ = true;
                buffer_.clear();
                buffer_.shrink_to_fit();
                break;
            }
            const long long length = ParseContentLength(std::string_view(buffer_.data(), header_end));
            buffer_.erase(0, header_end + 4);
            if (length < 0) {
                // 坏头(没有 Content-Length/不是数字):丢掉这块头,继续找
                // 下一条,不把整条流搞死,也不算资源超限。
                continue;
            }
            if (static_cast<unsigned long long>(length) > kMaxBodyBytes) {
                // 声明长度越帽(含十进制超长被钳住的):协议不可信,头凑齐
                // 即判死,不等正文,内存不受声明值摆布。
                overflowed_ = true;
                buffer_.clear();
                buffer_.shrink_to_fit();
                break;
            }
            expected_ = static_cast<std::size_t>(length);
            in_body_ = true;
        }
        // 正在攒正文。
        if (buffer_.size() < expected_) {
            break;  // 正文还没到齐,残包留缓冲(上界 = 声明值 ≤ kMaxBodyBytes)
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
        // stdout 读线程:按 Content-Length 分帧,逐条上交;头部块或声明长度
        // 超上限就宣布协议报废、杀进程断连(返回 false 让读线程收工)。
        // 进程一死,等待中的请求靠 IsAlive 轮询很快就能失败返回,不悬挂。
        [this](std::string_view chunk) {
            std::vector<std::string> messages = framer_.Feed(chunk);
            for (auto& message : messages) {
                if (on_message_) {
                    on_message_(std::move(message));
                }
            }
            if (framer_.overflowed()) {
                {
                    std::lock_guard<std::mutex> lock(stderr_mutex_);
                    stderr_buffer_ +=
                        "[lubancode] LSP 服务器输出超过分帧上限(头部块 8KB/单条正文 8MB),协议错误,已断开";
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
