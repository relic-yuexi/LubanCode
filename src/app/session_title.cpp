#include "app/session_title.hpp"

#include <string_view>
#include <variant>

#include "agent/sample_model.hpp"  // SampleModel 原语:采样的公共路(批一·病四)
#include "platform/text_encoding.hpp"

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
                // 刀口必须在原串上算。旧写法先 substr(0, 600)，再从成品
                // 尾部删续字节；若刀口落进三字节汉字，删完会剩一枚孤立
                // 首字节，随后拼入的 '\n' 就成了 nlohmann 报出的 0x0A。
                constexpr std::size_t kExcerptMaxBytes = 600;
                const std::size_t cut = lubancode::platform::Utf8PrefixBoundary(text->text, kExcerptMaxBytes);
                std::string piece = text->text.substr(0, cut);
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

    // 采样走 SampleModel 原语(批一·病四):攒流/usage/兜错/看门狗的路只有
    // 一份,这里只剩提示拼装与标题清洗;错误只回 message(旧口径)。
    lubancode::agent::SampleRequest sample;
    sample.model = model;
    sample.reasoning_effort = reasoning_effort;
    sample.system = "给下面这段对话起一个会话标题。要求:中文不超过 16 个字(英文不超过 8 个词),"
                    "说清对话在做什么(比如\"修登录超时\"\"调研向量库选型\");"
                    "直接输出标题本身,不要引号、不要标点结尾、不要解释。";
    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{transcript});
    sample.messages.push_back(std::move(message));
    sample.max_tokens = 100;

    lubancode::agent::SampleOptions sample_options;
    sample_options.timeout_secs = timeout_secs;
    const lubancode::agent::SampleResult sampled =
        lubancode::agent::SampleModel(backend, sample, sample_options);

    if (accounting != nullptr) {
        accounting->usage.input_tokens += sampled.usage.input_tokens;
        accounting->usage.cache_read_tokens += sampled.usage.cache_read_tokens;
        accounting->usage.cache_creation_tokens += sampled.usage.cache_creation_tokens;
        accounting->usage.output_tokens += sampled.usage.output_tokens;
        accounting->usage.output_reasoning_tokens += sampled.usage.output_reasoning_tokens;
        accounting->usage_reported = sampled.usage_reported;
        accounting->duration_ms = sampled.duration_ms;
    }

    if (!sampled.ok) return std::unexpected(sampled.error.message);

    const std::string title = SanitizeTitle(sampled.text);
    if (title.empty()) {
        return std::unexpected("标题为空");
    }
    return title;
}

}  // namespace lubancode::app
