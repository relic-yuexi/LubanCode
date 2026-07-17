#include "agent/context.hpp"

#include <cstdlib>
#include <string>
#include <type_traits>
#include <variant>

namespace lubancode::agent {

namespace {

std::size_t BlockChars(const api::ContentBlock& block) {
    return std::visit(
        [](const auto& b) -> std::size_t {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                return b.text.size();
            } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                return b.name.size() + b.id.size() + b.input.dump().size();
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                return b.tool_use_id.size() + b.content.size();
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
        if (std::holds_alternative<api::TextBlock>(block)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::size_t EstimateChars(const std::vector<api::Message>& history) {
    std::size_t total = 0;
    for (const auto& message : history) {
        total += MessageChars(message);
    }
    return total;
}

std::size_t EstimateTokens(const std::vector<api::Message>& history) {
    return EstimateChars(history) / 3;
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
                                       std::size_t keep_recent_turns) {
    if (history.empty()) {
        return history;
    }
    if (EstimateChars(history) <= max_chars) {
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
        // 用户消息开始),没法安全地按轮裁剪,原样返回。
        return history;
    }

    if (turns.size() <= keep_recent_turns + 1) {
        // 第一轮 + 最近 N 轮已经盖住了全部历史,没有中间可丢的。
        return history;
    }

    const std::size_t first_turn_end = turns.front().second;
    const std::size_t recent_start_idx = turns.size() - keep_recent_turns;
    const std::size_t recent_start = turns[recent_start_idx].first;

    if (recent_start <= first_turn_end) {
        // 保留区间已经衔接甚至重叠,没有中间段可丢。
        return history;
    }

    std::vector<api::Message> trimmed;
    trimmed.reserve(first_turn_end + (history.size() - recent_start) + 1);

    for (std::size_t i = 0; i < first_turn_end; ++i) {
        trimmed.push_back(history[i]);
    }

    api::Message notice;
    notice.role = api::Role::User;
    notice.content.push_back(api::TextBlock{"[早前对话已裁剪]"});
    trimmed.push_back(std::move(notice));

    for (std::size_t i = recent_start; i < history.size(); ++i) {
        trimmed.push_back(history[i]);
    }

    return trimmed;
}

}  // namespace lubancode::agent
