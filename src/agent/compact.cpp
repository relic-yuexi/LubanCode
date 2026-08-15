#include "agent/compact.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "agent/context.hpp"  // 统一 token 估算口径
#include "api/assembler.hpp"

namespace lubancode::agent {

namespace {

// 跟 agent/context.cpp 里的同名私有 helper 语义一模一样(角色是 user、
// 且至少带一个 TextBlock 或 ImageBlock 才算"一轮的开头"),那边是匿名命名
// 空间里的私有函数、没导出,所以这里原样再写一份。
bool IsUserTurnStart(const api::Message& message) {
    if (message.role != api::Role::User) {
        return false;
    }
    for (const auto& block : message.content) {
        if (std::holds_alternative<api::TextBlock>(block) || std::holds_alternative<api::ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

// 剥两端空白(空格/制表/回车/换行)。
std::string TrimWhitespace(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 剥掉全部空白再比对——守恒校验用,模型改了个换行、多敲个空格不算丢。
std::string NormalizeForCompare(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        out += c;
    }
    return out;
}

// 数 UTF-8 字符数(码点)。
std::size_t CountUtf8Chars(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// 空/短摘要拒收的门槛:剥空白后不足 40 字。踩过的坑:模型把压缩请求当
// "续写"处理,只回一两个字,这种残次摘要一旦顶替历史,记住的事实就全丢
// 了——宁可失败保历史,不吞残次品。这门槛只是最外层防呆;真正的验收在
// manifest 解析与守恒校验。
constexpr std::size_t kMinSummaryChars = 40;

// 压缩指令(固定栏目头 + manifest 要求)。required_open_items 非空时追加
// "逐字收编"一节。
std::string BuildCompactInstruction(const CompactOptions& options) {
    std::string instruction =
        "以上是到目前为止的对话历史。请把它压缩成一份存档,按以下栏目输出,栏目头逐字照写:\n"
        "## 任务目标\n"
        "## 已证实的事实\n"
        "## 关键决策\n"
        "## 涉及文件与符号\n"
        "## 关键命令与结果\n"
        "## 未完成事项\n"
        "只写对话里确证过的内容,不许猜补;某栏没有内容就写\"(无)\"。闲聊和过程细节"
        "(工具调用的中间试错、无关寒暄)可以舍弃。\n"
        "存档正文之后,另起一行输出一枚 JSON 代码块(```json 围栏),键名逐字照写:\n"
        "```json\n"
        "{\"goal\": \"当前任务目标一句话\", \"constraints\": [\"用户明示的约束或禁止\"], "
        "\"open_items\": [\"未完成事项\"], \"next_action\": \"下一步该做的具体动作\"}\n"
        "```\n"
        "goal 与 open_items 不许为空数组/空串;constraints 没有就给空数组。"
        "JSON 必须能直接解析,不要加注释。";
    if (!options.required_open_items.empty()) {
        instruction += "\n\n以下是当前仍未完成的待办,每一项必须逐字(只许调整空白)出现在 open_items 数组里,"
                       "一项都不许丢、不许改写:\n";
        for (std::size_t i = 0; i < options.required_open_items.size(); ++i) {
            instruction += std::to_string(i + 1) + ". " + options.required_open_items[i] + "\n";
        }
    }
    if (!options.focus.empty()) {
        instruction += "\n另加一栏\"## 重点保留\",重点保留:" + options.focus;
    }
    return instruction;
}

}  // namespace

std::optional<std::size_t> CompactInputBudget(const CompactBudget& budget) {
    if (!budget.window_tokens.has_value()) {
        return std::nullopt;
    }
    const std::size_t reserved = budget.output_reserve_tokens + budget.protocol_headroom_tokens;
    if (*budget.window_tokens <= reserved) {
        return std::size_t{0};  // 窗口连预留都盖不住:预算为零,任何输入都装不下
    }
    return *budget.window_tokens - reserved;
}

std::optional<CompactManifest> ParseCompactManifest(const std::string& summary_text) {
    // 取最后一个 ```json 围栏块。模型偶尔会在前后添话,围栏块必须认末尾
    // 那一枚(那是它按指令补的 manifest,前面若引用了代码块不算)。
    const std::size_t fence_begin = summary_text.rfind("```json");
    if (fence_begin == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t body_begin = summary_text.find('\n', fence_begin);
    if (body_begin == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t fence_end = summary_text.find("```", body_begin);
    if (fence_end == std::string::npos || fence_end <= body_begin) {
        return std::nullopt;
    }
    const std::string body = summary_text.substr(body_begin + 1, fence_end - body_begin - 1);
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (...) {
        return std::nullopt;
    }
    if (!parsed.is_object() || !parsed.contains("goal") || !parsed["goal"].is_string() ||
        !parsed.contains("open_items") || !parsed["open_items"].is_array()) {
        return std::nullopt;
    }
    CompactManifest manifest;
    manifest.goal = parsed["goal"].get<std::string>();
    for (const auto& item : parsed["open_items"]) {
        if (item.is_string()) {
            manifest.open_items.push_back(item.get<std::string>());
        }
    }
    if (parsed.contains("constraints") && parsed["constraints"].is_array()) {
        for (const auto& item : parsed["constraints"]) {
            if (item.is_string()) {
                manifest.constraints.push_back(item.get<std::string>());
            }
        }
    }
    if (parsed.contains("next_action") && parsed["next_action"].is_string()) {
        manifest.next_action = parsed["next_action"].get<std::string>();
    }
    return manifest;
}

CompactValidation ValidateCompactManifest(const CompactManifest& manifest,
                                          const std::vector<std::string>& required_open_items) {
    CompactValidation result;
    if (TrimWhitespace(manifest.goal).empty()) {
        result.failures.push_back("manifest 缺任务目标(goal)");
    }
    // 待办守恒:活动 plan 的每一条未完成事项必须逐字(空白归一后)还在。
    for (const auto& required : required_open_items) {
        const std::string needle = NormalizeForCompare(required);
        if (needle.empty()) {
            continue;
        }
        bool found = false;
        for (const auto& item : manifest.open_items) {
            if (NormalizeForCompare(item).find(needle) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) {
            result.failures.push_back("摘要丢了未完成事项: " + required);
        }
    }
    result.ok = result.failures.empty();
    return result;
}

std::vector<api::Message> BuildCompactedHistory(const std::vector<api::Message>& history,
                                                const api::Message& archive, std::size_t hot_zone_tokens) {
    std::vector<api::Message> new_history;

    // 按轮切:turns[i] = [start, end),一条用户文本输入领起,直到下一条
    // 用户文本输入之前。
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
        // 没有可保留的用户轮,存档只能自己单独成一条。
        new_history.push_back(archive);
        return new_history;
    }

    // 热区:从最后一轮整轮保留起,往前按轮收,收满 token 预算为止。
    // 最后一轮(最新用户消息所在)无论多大都保——它是必须钉住的内容,
    // 压不得;再往前的轮只有整个装得下预算才进来,装不下就停在轮边界,
    // tool_use/tool_result 的配对天然不被切开。
    std::vector<std::size_t> message_tokens(history.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
        // 每条至少记 1 token:估算为零的空壳消息不该白占热区预算(也让
        // "预算掐到最小只剩最后一轮"这条边界可测)。
        message_tokens[i] = std::max<std::size_t>(1, EstimateMessageTokens(history[i]));
    }
    const auto turn_tokens = [&turns, &message_tokens](std::size_t index) {
        std::size_t total = 0;
        for (std::size_t i = turns[index].first; i < turns[index].second; ++i) {
            total += message_tokens[i];
        }
        return total;
    };
    std::size_t keep_from = turns.back().first;
    std::size_t used = 0;
    for (std::size_t i = keep_from; i < history.size(); ++i) {
        used += message_tokens[i];
    }
    for (std::size_t t = turns.size() - 1; t-- > 0;) {
        const std::size_t tokens = turn_tokens(t);
        if (used + tokens > hot_zone_tokens) {
            break;
        }
        used += tokens;
        keep_from = turns[t].first;
    }

    // 存档正文并入热区第一条 user 消息开头,不单独成一条消息——独立的
    // 存档 user 消息紧跟热区的 user 输入,就是相邻两条 user,违反 Anthropic
    // 的角色交替要求(标准端点 400;MiniMax 宽容,才一直没暴露)。
    std::string archive_text;
    for (const auto& block : archive.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            archive_text += std::get<api::TextBlock>(block).text;
        }
    }

    api::Message merged = history[keep_from];
    bool merged_into_text = false;
    for (auto& block : merged.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            auto& text_block = std::get<api::TextBlock>(block);
            text_block.text = archive_text + "\n\n" + text_block.text;
            merged_into_text = true;
            break;
        }
    }
    if (!merged_into_text) {
        // IsUserTurnStart 保证有 TextBlock,这里纯防御。
        merged.content.push_back(api::TextBlock{archive_text});
    }
    new_history.push_back(std::move(merged));
    new_history.insert(new_history.end(), history.begin() + static_cast<std::ptrdiff_t>(keep_from) + 1,
                       history.end());

    return new_history;
}

std::expected<CompactSummary, api::Error> Compact(api::Backend& backend, const std::string& model,
                                                  const std::vector<api::Message>& history,
                                                  const CompactOptions& options) {
    const std::string instruction = BuildCompactInstruction(options);

    // 窗口预算:估算输入(指令 + 全份 history,统一 token 口径)超过压缩
    // 模型自己的可用预算时,明确拒绝——不发这个注定装不下的请求,更不静
    // 默截史。分块压缩是分层化下一期的活,这期把拒绝说清楚。
    if (const auto input_budget = CompactInputBudget(options.budget); input_budget.has_value()) {
        const std::size_t input_estimate = EstimateUtf8Tokens(instruction) + EstimateHistoryTokens(history) +
                                           options.budget.protocol_headroom_tokens;
        if (input_estimate > *input_budget) {
            return std::unexpected(api::Error{
                api::ErrorKind::Api,
                "压缩模型窗口 " + std::to_string(*options.budget.window_tokens) + " tokens 的可用输入预算 " +
                    std::to_string(*input_budget) + " tokens,装不下当前历史(估 " +
                    std::to_string(input_estimate) + " tokens)。已拒绝,历史一字未动,未做任何截断。",
                0});
        }
    }

    api::Request request;
    request.model = model;
    request.system = instruction;
    request.messages = history;

    // 踩过的坑:history 最后一条常常是 assistant 消息,原样发出去会被
    // Anthropic/Responses 两边当成"续写最后这条 assistant 消息"(prefill
    // continuation)处理,模型不理会 system 里的总结指令,只顺着最后一条
    // 内容随手接几个字——实测出过压缩正文只有孤零零一个"2"字的情况。
    // 补一条 user 消息收尾,让模型明确进入"该我说话、按 system 指令办"
    // 的状态。末条本来就是 user(比如工具结果)时天然该轮到它开口,不补。
    if (!request.messages.empty() && request.messages.back().role == api::Role::Assistant) {
        api::Message trailer;
        trailer.role = api::Role::User;
        trailer.content.push_back(api::TextBlock{"请开始压缩,直接给出存档正文与末尾的 JSON manifest,不要重复原对话内容。"});
        request.messages.push_back(trailer);
    }
    request.max_tokens = static_cast<int>(options.budget.output_reserve_tokens);

    api::MessageAssembler assembler;
    bool stream_error = false;
    std::string stream_error_message;

    const auto send_result = backend.send_stream(request, [&](const api::StreamEvent& event) {
        assembler.Feed(event);
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, api::StreamError>) {
                    stream_error = true;
                    stream_error_message = e.message;
                }
            },
            event);
    });

    if (!send_result.has_value()) {
        return std::unexpected(send_result.error());
    }
    if (stream_error) {
        return std::unexpected(api::Error{api::ErrorKind::Api, stream_error_message, 0});
    }

    const api::Message summary_message = assembler.BuildMessage();
    std::string summary_text;
    for (const auto& block : summary_message.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            summary_text += std::get<api::TextBlock>(block).text;
        }
    }

    // 第一道防呆:空/过短整份拒收。
    if (CountUtf8Chars(TrimWhitespace(summary_text)) < kMinSummaryChars) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api, "摘要为空/过短(不足 40 字),历史未动", 0});
    }

    // 第二道:可解析 manifest。
    const auto manifest = ParseCompactManifest(summary_text);
    if (!manifest.has_value()) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api, "摘要末尾没有可解析的 JSON manifest(缺 ```json 围栏块或键不全),历史未动", 0});
    }

    // 第三道:守恒校验(目标非空、待办一条不丢)。不过,旧 history 不动。
    const auto validation = ValidateCompactManifest(*manifest, options.required_open_items);
    if (!validation.ok) {
        std::string reasons;
        for (std::size_t i = 0; i < validation.failures.size(); ++i) {
            if (i > 0) {
                reasons += ";";
            }
            reasons += validation.failures[i];
        }
        return std::unexpected(api::Error{
            api::ErrorKind::Api, "摘要守恒校验未过,历史未动: " + reasons, 0});
    }

    api::Message archive;
    archive.role = api::Role::User;
    archive.content.push_back(api::TextBlock{"[对话存档,此前内容已压缩] " + summary_text});
    return CompactSummary{std::move(archive), *manifest};
}

}  // namespace lubancode::agent
