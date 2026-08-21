#include "app/session_title.hpp"

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

#include "api/assembler.hpp"

namespace lubancode::app {
namespace {

// 首尾整串比对:中文引号(“「”」)在 UTF-8 里三个字节,拿 front()/
// back() 的单字节比字面量,MSVC 静默、gcc 警告、clang 报错,还全比不中。
bool StartsWithBytes(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWithBytes(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string SanitizeTitle(const std::string& raw, std::size_t max_chars) {
    std::string text = raw;
    // 剥代码围栏:模型偶尔会把标题包进 ``` ```。
    if (text.size() > 6 && text.rfind("```", 0) == 0) {
        const std::size_t close = text.rfind("```");
        if (close >= 3) {
            text = text.substr(3, close - 3);
        }
    }
    // 压空白:标题单行,换行/制表折成空格,连续空格并成一个。
    std::string squeezed;
    bool pending_space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = !squeezed.empty();
            continue;
        }
        if (pending_space) {
            squeezed += ' ';
            pending_space = false;
        }
        squeezed += c;
    }
    // 剥首尾引号(模型爱加的书名号/引号一并剥掉一层)。
    while (!squeezed.empty()) {
        bool stripped = false;
        for (const std::string_view q : {"\"", "'", "“", "「"}) {
            if (StartsWithBytes(squeezed, q)) {
                squeezed.erase(0, q.size());
                stripped = true;
                break;
            }
        }
        if (!stripped) {
            break;
        }
    }
    while (!squeezed.empty()) {
        bool stripped = false;
        for (const std::string_view q : {"\"", "'", "”", "」"}) {
            if (EndsWithBytes(squeezed, q)) {
                squeezed.resize(squeezed.size() - q.size());
                stripped = true;
                break;
            }
        }
        if (!stripped) {
            break;
        }
    }
    // 限长:按 UTF-8 码点截,不从码点中腰劈开。
    std::size_t count = 0;
    std::size_t cut = squeezed.size();
    for (std::size_t i = 0; i < squeezed.size();) {
        if (count == max_chars) {
            cut = i;
            break;
        }
        std::size_t width = 1;
        const unsigned char lead = static_cast<unsigned char>(squeezed[i]);
        if ((lead & 0x80) == 0x00) {
            width = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            width = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            width = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            width = 4;
        }
        i += width;
        ++count;
    }
    squeezed.resize(cut);
    // 剥两端空白(压空白后再来一道,防截断后留空尾巴)。
    while (!squeezed.empty() && (squeezed.back() == ' ')) {
        squeezed.pop_back();
    }
    return squeezed;
}

std::expected<std::string, std::string> GenerateSessionTitle(lubancode::api::Backend& backend,
                                                             const std::string& model,
                                                             const std::string& reasoning_effort,
                                                             const std::vector<lubancode::api::Message>& head,
                                                             int timeout_secs,
                                                             lubancode::agent::BackgroundCallAccounting* accounting) {
    if (head.empty()) {
        return std::unexpected("没有可起标题的对话内容");
    }
    // 转写:只取用户/助手正文,各截一小段——标题不需要全文。
    std::string transcript;
    for (const auto& message : head) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                std::string piece = text->text.substr(0, 600);
                if (text->text.size() > 600) {
                    // 截尾不劈半个字:退到完整码点边界。
                    while (!piece.empty() &&
                           (static_cast<unsigned char>(piece.back()) & 0xC0) == 0x80) {
                        piece.pop_back();
                    }
                }
                transcript += (message.role == lubancode::api::Role::User ? "用户: " : "助手: ") + piece + "\n";
            }
        }
        if (transcript.size() > 4000) {
            break;
        }
    }
    if (transcript.empty()) {
        return std::unexpected("对话里没有可用的文本");
    }

    lubancode::api::Request request;
    request.model = model;
    request.reasoning_effort = reasoning_effort;
    request.system = "给下面这段对话起一个会话标题。要求:中文不超过 16 个字(英文不超过 8 个词),"
                     "说清对话在做什么(比如\"修登录超时\"\"调研向量库选型\");"
                     "直接输出标题本身,不要引号、不要标点结尾、不要解释。";
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{transcript});
    request.messages.push_back(std::move(message));
    request.max_tokens = 100;
    const auto started = std::chrono::steady_clock::now();

    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::thread watchdog([&cancel, &done, timeout_secs]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load()) cancel = true;
    });

    lubancode::api::MessageAssembler assembler;
    bool stream_error = false;
    std::string stream_error_message;
    auto send_result = backend.send_stream(
        request,
        [&](const lubancode::api::StreamEvent& event) {
            assembler.Feed(event);
            if (const auto* error = std::get_if<lubancode::api::StreamError>(&event)) {
                stream_error = true;
                stream_error_message = error->message;
            }
        },
        &cancel);
    done = true;
    watchdog.join();

    if (accounting != nullptr) {
        const lubancode::api::Usage& usage = assembler.usage();
        accounting->usage.input_tokens += usage.input_tokens;
        accounting->usage.cache_read_tokens += usage.cache_read_tokens;
        accounting->usage.cache_creation_tokens += usage.cache_creation_tokens;
        accounting->usage.output_tokens += usage.output_tokens;
        accounting->usage.output_reasoning_tokens += usage.output_reasoning_tokens;
        accounting->usage_reported = usage.input_tokens > 0 || usage.output_tokens > 0 ||
                                     usage.cache_read_tokens > 0 || usage.cache_creation_tokens > 0;
        accounting->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
    }

    if (!send_result.has_value()) return std::unexpected(send_result.error().message);
    if (stream_error) return std::unexpected(stream_error_message);

    std::string reply;
    for (const auto& block : assembler.BuildMessage().content) {
        if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
            reply += text->text;
        }
    }
    const std::string title = SanitizeTitle(reply);
    if (title.empty()) {
        return std::unexpected("标题为空");
    }
    return title;
}

}  // namespace lubancode::app
