#include "agent/context.hpp"

#include <cstdlib>
#include <string>
#include <type_traits>
#include <variant>

#include "platform/text_encoding.hpp"  // Utf8PrefixBoundary:截短不劈半个字

namespace lubancode::agent {

namespace {

std::size_t BlockChars(const api::ContentBlock& block) {
    return std::visit(
        [](const auto& b) -> std::size_t {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                return b.text.size();
            } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                return b.media_type.size() + b.data.size() + b.filename.size();
            } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                return b.name.size() + b.id.size() + b.input.dump().size();
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                return b.tool_use_id.size() + b.content.size();
            } else if constexpr (std::is_same_v<T, api::ThinkingBlock>) {
                return b.text.size() + b.signature.size();
            } else {
                return 0;
            }
        },
        block);
}

std::size_t MessageChars(const api::Message& message) {
    std::size_t total = 0;
    for (const auto& block : message.content) {
        total += BlockChars(block);
    }
    return total;
}

// 一条"真正的用户输入"消息:role 是 User,并且内容里带着 TextBlock
// ——区别于同样顶着 User 角色、但内容全是 ToolResultBlock 的那种
// "把工具结果喂回去"的中间消息。
bool IsUserTurnStart(const api::Message& message) {
    if (message.role != api::Role::User) {
        return false;
    }
    if (message.content.empty()) {
        return true;  // 理论上不会出现,防御性地当成一轮开始
    }
    for (const auto& block : message.content) {
        if (std::holds_alternative<api::TextBlock>(block) || std::holds_alternative<api::ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

// 硬上限兜底:轮级裁剪之后仍旧超限(往往是单条工具结果就大得离谱),把
// 超大的 ToolResultBlock 内容从尾部截短、打上标注。只动 tool_result 的
// content 字符串,消息条数、tool_use/tool_result 的配对关系一概不碰。
// 每条至少留 kMinKeepChars,免得截成空壳,模型连是什么工具的结果都看不出。
// 截尾兜底带报告:截了哪些工具结果,给上层一句实话。
std::vector<api::Message> ShrinkOversizedToolResults(std::vector<api::Message> messages, std::size_t max_chars,
                                                     TrimReport* report) {
    constexpr std::size_t kMinKeepChars = 1024;
    const char kMark[] = "\n[内容过长已截断]";
    const std::size_t mark_size = sizeof(kMark) - 1;

    std::size_t total = EstimateHistoryBytes(messages);
    if (total <= max_chars) {
        return messages;
    }

    for (auto& message : messages) {
        for (auto& block : message.content) {
            if (total <= max_chars) {
                return messages;
            }
            if (!std::holds_alternative<api::ToolResultBlock>(block)) {
                continue;
            }
            auto& tool_result = std::get<api::ToolResultBlock>(block);
            if (tool_result.content.size() <= kMinKeepChars + mark_size) {
                continue;
            }
            const std::size_t overage = total - max_chars;
            const std::size_t reducible = tool_result.content.size() - kMinKeepChars - mark_size;
            const std::size_t cut = overage < reducible ? overage + mark_size : reducible + mark_size;
            // 刀口先对齐码点边界再砍:裸按字节 resize,砍进三字节汉字的腰上,
            // 末尾悬半个字,合法 UTF-8 也会被截成非法——请求体 dump 当场
            // type_error.316,整场会话每回合必挂(真机上掐死过)。
            const std::size_t keep =
                platform::Utf8PrefixBoundary(tool_result.content, tool_result.content.size() - cut);
            tool_result.content.resize(keep);
            tool_result.content += kMark;
            if (report != nullptr) {
                report->truncated_results = true;
            }
            total = EstimateHistoryBytes(messages);
        }
    }
    return messages;
}

}  // namespace

std::size_t EstimateHistoryBytes(const std::vector<api::Message>& history) {
    std::size_t total = 0;
    for (const auto& message : history) {
        total += MessageChars(message);
    }
    return total;
}

std::size_t EstimateUtf8Tokens(const std::string& text) {
    std::size_t ascii = 0;
    std::size_t codepoints = 0;  // 全部码点;非 ASCII 数 = codepoints - ascii
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) != 0x80) {
            ++codepoints;  // UTF-8 后续字节不算新码点
        }
        if (byte < 0x80) {
            ++ascii;
        }
    }
    const std::size_t non_ascii = codepoints >= ascii ? codepoints - ascii : 0;
    // ASCII 4 字符 1 token;非 ASCII 3 token 折 2 字。不引分词器,预算够用。
    return ascii / 4 + (non_ascii * 3) / 2;
}

std::size_t EstimateMessageTokens(const api::Message& message) {
    std::size_t total = 0;
    for (const auto& block : message.content) {
        total += std::visit(
            [](const auto& b) -> std::size_t {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, api::TextBlock>) {
                    return EstimateUtf8Tokens(b.text);
                } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                    // base64 体积粗折:约 4/3 字符 3 字节,再按字节 4 折 1。
                    return (b.media_type.size() + b.data.size() + b.filename.size()) / 4;
                } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                    return EstimateUtf8Tokens(b.name) + EstimateUtf8Tokens(b.id) +
                           EstimateUtf8Tokens(b.input.dump());
                } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                    return EstimateUtf8Tokens(b.tool_use_id) + EstimateUtf8Tokens(b.content);
                } else if constexpr (std::is_same_v<T, api::ThinkingBlock>) {
                    return EstimateUtf8Tokens(b.text) + EstimateUtf8Tokens(b.signature);
                } else {
                    return 0;
                }
            },
            block);
    }
    return total;
}

std::size_t EstimateHistoryTokens(const std::vector<api::Message>& history) {
    std::size_t total = 0;
    for (const auto& message : history) {
        total += EstimateMessageTokens(message);
    }
    return total;
}

std::size_t MaxContextCharsFromEnv() {
    std::string value;
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, "LUBANCODE_MAX_CONTEXT");
    if (err != 0 || buffer == nullptr) {
        return kDefaultMaxContextChars;
    }
    value = buffer;
    std::free(buffer);
#else
    const char* raw = std::getenv("LUBANCODE_MAX_CONTEXT");
    if (raw == nullptr || raw[0] == '\0') {
        return kDefaultMaxContextChars;
    }
    value = raw;
#endif
    if (value.empty()) {
        return kDefaultMaxContextChars;
    }
    try {
        const long long parsed = std::stoll(value);
        if (parsed <= 0) {
            return kDefaultMaxContextChars;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return kDefaultMaxContextChars;
    }
}

std::vector<api::Message> TrimHistory(const std::vector<api::Message>& history, std::size_t max_chars,
                                       std::size_t keep_recent_turns, TrimReport* report) {
    if (history.empty()) {
        return history;
    }
    if (EstimateHistoryBytes(history) <= max_chars) {
        return history;
    }

    // 把 history 切成"轮":turns[i] = [start, end),含一条 user 输入消息
    // 和紧跟其后的所有消息,直到下一条 user 输入消息之前。
    std::vector<std::pair<std::size_t, std::size_t>> turns;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (IsUserTurnStart(history[i])) {
            if (!turns.empty()) {
                turns.back().second = i;
            }
            turns.emplace_back(i, history.size());
        }
    }

    if (turns.empty()) {
        // 找不到任何一条"真正的用户输入"消息(不应该发生,history 总是从
        // 用户消息开始),没法安全地按轮裁剪,只做工具结果截断兜底。
        return ShrinkOversizedToolResults(history, max_chars, report);
    }

    if (turns.size() <= keep_recent_turns + 1) {
        // 第一轮 + 最近 N 轮已经盖住了全部历史,没有中间可丢的。
        return ShrinkOversizedToolResults(history, max_chars, report);
    }

    const std::size_t first_turn_end = turns.front().second;
    const std::size_t recent_start_idx = turns.size() - keep_recent_turns;
    const std::size_t recent_start = turns[recent_start_idx].first;

    if (recent_start <= first_turn_end) {
        // 保留区间已经衔接甚至重叠,没有中间段可丢。
        return ShrinkOversizedToolResults(history, max_chars, report);
    }

    std::vector<api::Message> trimmed;
    trimmed.reserve(first_turn_end + (history.size() - recent_start));

    for (std::size_t i = 0; i < first_turn_end; ++i) {
        trimmed.push_back(history[i]);
    }

    // 裁剪说明并入保留区间第一条 user 消息的开头,不单独插一条 user 消息——
    // 独立插会跟紧随其后的 user 输入连成相邻两条 user,违反 Anthropic 的
    // 角色交替要求(标准端点直接 400;MiniMax 宽容,才一直没炸)。
    api::Message merged = history[recent_start];
    bool merged_into_text = false;
    for (auto& block : merged.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            auto& text_block = std::get<api::TextBlock>(block);
            text_block.text = "[早前对话已裁剪]\n\n" + text_block.text;
            merged_into_text = true;
            break;
        }
    }
    if (!merged_into_text) {
        merged.content.push_back(api::TextBlock{"[早前对话已裁剪]"});
    }
    trimmed.push_back(std::move(merged));

    for (std::size_t i = recent_start + 1; i < history.size(); ++i) {
        trimmed.push_back(history[i]);
    }

    if (report != nullptr) {
        report->trimmed_turns = true;
        report->dropped_messages = recent_start - first_turn_end;
    }
    return ShrinkOversizedToolResults(std::move(trimmed), max_chars, report);
}

}  // namespace lubancode::agent
