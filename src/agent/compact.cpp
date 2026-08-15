#include "agent/compact.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "agent/context.hpp"       // 统一 token 估算口径
#include "agent/context_events.hpp"  // 事件账:evidence_refs 的来源区间
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

// ---------------------------------------------------------------------------
// 第三期:分阶段、分层摘要
// ---------------------------------------------------------------------------

namespace {

// 一枚"用户文本输入"消息(与各处同名私有 helper 同语义)。
bool IsUserTurnStartMsg(const api::Message& message) {
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

// 消息里是否带 todo_write 调用(plan 变化的显式信号)。
bool HasTodoWrite(const api::Message& message) {
    for (const auto& block : message.content) {
        if (std::holds_alternative<api::ToolUseBlock>(block) &&
            std::get<api::ToolUseBlock>(block).name == "todo_write") {
            return true;
        }
    }
    return false;
}

// 发一次"给我摘要"请求:system 指令 + 消息列表,收一段纯文本回来。
// 失败(请求失败/流内错误)原样透传;正文交调用方再验。
std::expected<std::string, api::Error> RequestSummaryText(api::Backend& backend, const std::string& model,
                                                          const std::string& system,
                                                          const std::vector<api::Message>& messages,
                                                          int max_tokens) {
    api::Request request;
    request.model = model;
    request.system = system;
    request.messages = messages;
    // 老坑同前:末条 assistant 会被当 prefill continuation,补一条 user 收尾。
    if (!request.messages.empty() && request.messages.back().role == api::Role::Assistant) {
        api::Message trailer;
        trailer.role = api::Role::User;
        trailer.content.push_back(api::TextBlock{"请开始,直接给出要求的正文。"});
        request.messages.push_back(trailer);
    }
    request.max_tokens = max_tokens;

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
    std::string text;
    for (const auto& block : assembler.BuildMessage().content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            text += std::get<api::TextBlock>(block).text;
        }
    }
    return text;
}

// 消息区间的事件号区间("e12-e45"):从事件账里找该消息区间覆盖的 ToolExchange/
// 文本事件,取首尾 id。evidence_refs 由程序钉,不由模型编。
std::string EventRangeForMessages(const std::vector<NormalizedEvent>& ledger, std::size_t from_message,
                                  std::size_t to_message) {
    std::string first;
    std::string last;
    for (const auto& event : ledger) {
        const bool in_range =
            event.message_index >= from_message && event.message_index < to_message;
        if (!in_range) {
            continue;
        }
        if (first.empty()) {
            first = event.id;
        }
        last = event.id;
    }
    if (first.empty()) {
        return std::string();
    }
    return first == last ? first : first + "-" + last;
}

// 从(压缩后历史的)首条 user 文本里剥出上一轮存档:存档以固定前缀起头、以
// ```json manifest 的闭合围栏收尾,其后才是被并入保留下来的原始输入。
// 剥得出给 {prior, rest};剥不出给 nullopt(整条当普通原始消息)。
std::optional<std::pair<std::string, std::string>> SplitPriorArchive(const std::string& text) {
    static const std::string kPrefix = "[对话存档,此前内容已压缩]";
    if (text.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    const std::size_t json_fence = text.find("```json");
    if (json_fence == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t close = text.find("```", json_fence + 7);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    std::size_t split = close + 3;
    if (text.compare(split, 2, "\n\n") == 0) {
        split += 2;
    }
    std::string rest = text.substr((std::min)(split, text.size()));
    if (TrimWhitespace(rest).empty()) {
        rest = std::string();
    }
    return std::make_pair(text.substr(0, (std::min)(split, text.size())), rest);
}

// map 阶段指令:一段探索的局部小结,末尾同样要 manifest(供 reduce 归并)。
std::string BuildEpisodeInstruction(const CompactOptions& options) {
    std::string instruction =
        "以上是对话历史中的一段(一个任务阶段)。请把它收成一份局部小结,按以下栏目输出,栏目头逐字照写:\n"
        "## 阶段目标\n"
        "## 已证实的事实\n"
        "## 关键决策\n"
        "## 涉及文件与符号\n"
        "## 未完成事项\n"
        "只写这段里确证过的内容,不许猜补;某栏没有内容就写\"(无)\"。闲聊与中间试错可以舍弃。\n"
        "小结末尾另起一行输出一枚 JSON 代码块(```json 围栏),键名逐字照写:\n"
        "```json\n"
        "{\"goal\": \"本阶段目标一句话\", \"constraints\": [\"本阶段出现的约束\"], "
        "\"open_items\": [\"本阶段结束时仍未完成的事\"], \"next_action\": \"下步动作\"}\n"
        "```\n"
        "goal 不许为空;没有未完成事项 open_items 给空数组。";
    if (!options.focus.empty()) {
        instruction += "\n重点关注:" + options.focus;
    }
    return instruction;
}

// reduce 阶段指令:各段局部小结 + 既有工作状态 → 最终存档。终稿的验收
// (manifest/守恒)与单次压缩同一套。
std::string BuildReduceInstruction(const CompactOptions& options, bool has_prior_state) {
    std::string instruction =
        "以上是一份任务的多份**局部小结**(每份带来源事件区间),"
        "请把它们归并成一份完整的交接存档,按以下栏目输出,栏目头逐字照写:\n"
        "## 任务目标\n"
        "## 已证实的事实\n"
        "## 关键决策\n"
        "## 涉及文件与符号\n"
        "## 关键命令与结果\n"
        "## 未完成事项\n"
        "只写局部小结里确证过的内容,不许猜补;相互矛盾时以后发生的为准并注明。\n"
        "存档正文之后,另起一行输出一枚 JSON 代码块(```json 围栏),键名逐字照写:\n"
        "```json\n"
        "{\"goal\": \"当前任务目标一句话\", \"constraints\": [\"用户明示的约束或禁止\"], "
        "\"open_items\": [\"未完成事项\"], \"next_action\": \"下一步该做的具体动作\"}\n"
        "```\n"
        "goal 与 open_items 不许为空数组/空串;constraints 没有就给空数组。";
    if (has_prior_state) {
        instruction += "\n输入里另有一份**上一轮压缩的存档**:只当参考,凡与局部小结冲突的,"
                       "以局部小结(来自原始事件)为准——不许拿旧摘要复印新摘要。";
    }
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

std::vector<std::pair<std::size_t, std::size_t>> SplitEpisodes(const std::vector<api::Message>& history) {
    std::vector<std::pair<std::size_t, std::size_t>> episodes;
    std::size_t start = 0;
    for (std::size_t i = 1; i < history.size(); ++i) {
        // 显式信号:新一条外层用户输入(新要求/纠正)、todo_write(plan 变化)。
        if (IsUserTurnStartMsg(history[i]) || HasTodoWrite(history[i])) {
            episodes.emplace_back(start, i);
            start = i;
        }
    }
    if (start < history.size()) {
        episodes.emplace_back(start, history.size());
    }
    return episodes;
}

std::expected<LayeredCompactResult, api::Error> CompactHierarchical(api::Backend& backend,
                                                                     const std::string& model,
                                                                     const std::vector<api::Message>& history,
                                                                     const CompactOptions& options) {
    LayeredCompactResult result;
    // 观测钩子(第四期):本次压缩输入的内容指纹。将来"episode 关闭后台
    // 预计算局部摘要、正式触发时按 digest 复用"靠它判失效;现在只记不用。
    {
        std::string buffer;
        for (const auto& message : history) {
            for (const auto& block : message.content) {
                if (std::holds_alternative<api::TextBlock>(block)) {
                    buffer += std::get<api::TextBlock>(block).text;
                } else if (std::holds_alternative<api::ToolResultBlock>(block)) {
                    buffer += std::get<api::ToolResultBlock>(block).content;
                }
            }
        }
        result.metrics.source_digest = Fingerprint64(buffer);
    }

    // 预算未知 → 分不了块(没法判定块上限),退化为单次压缩(老行为)。
    const auto input_budget = CompactInputBudget(options.budget);
    if (!input_budget.has_value()) {
        const auto single = Compact(backend, model, history, options);
        if (!single.has_value()) {
            return std::unexpected(single.error());
        }
        result.archive = single->archive;
        result.manifest = single->manifest;
        return result;
    }

    // 装得下 → 单次压缩,一条路同校验。
    const std::size_t single_input =
        EstimateUtf8Tokens(BuildCompactInstruction(options)) + EstimateHistoryTokens(history) +
        options.budget.protocol_headroom_tokens;
    if (single_input <= *input_budget) {
        const auto single = Compact(backend, model, history, options);
        if (!single.has_value()) {
            return std::unexpected(single.error());
        }
        result.archive = single->archive;
        result.manifest = single->manifest;
        return result;
    }

    // ---- 分层:冷区按 episode 切块 map,再 reduce ----
    const std::size_t hot_start = HotZoneStartIndex(history);
    if (hot_start == 0) {
        // 整份历史都在最后一轮里(单轮巨型):没有冷区可分层,交给单次压缩
        // 按窗口预算明确拒绝——绝不静默截史。
        const auto single = Compact(backend, model, history, options);
        if (!single.has_value()) {
            return std::unexpected(single.error());
        }
        result.archive = single->archive;
        result.manifest = single->manifest;
        return result;
    }
    const std::vector<NormalizedEvent> ledger = BuildEventLedger(history);

    // 上一轮存档剥出来:它只进 reduce 当参考,不进任何 map 块——局部摘要
    // 永远从原始消息来,阻断"摘要复印摘要"。
    std::string prior_state;
    std::vector<api::Message> cold;
    cold.reserve(hot_start);
    for (std::size_t i = 0; i < hot_start; ++i) {
        api::Message message = history[i];
        if (prior_state.empty() && i == 0) {
            for (auto& block : message.content) {
                if (std::holds_alternative<api::TextBlock>(block)) {
                    auto& text_block = std::get<api::TextBlock>(block);
                    if (auto split = SplitPriorArchive(text_block.text)) {
                        prior_state = split->first;
                        text_block.text = split->second;
                    }
                    break;
                }
            }
        }
        cold.push_back(std::move(message));
    }
    // 剥掉存档后剩下的空壳首条消息也不丢——账要齐。

    // 切块:episode 为首选边界;单段超预算就在段内按轮(用户输入边界)再切。
    const std::size_t chunk_budget = *input_budget > options.budget.protocol_headroom_tokens + 1024
                                         ? *input_budget - options.budget.protocol_headroom_tokens
                                         : *input_budget;
    struct Chunk {
        std::size_t from;
        std::size_t to;  // 消息区间 [from, to)
    };
    std::vector<Chunk> chunks;
    for (const auto& [ep_from, ep_to] : SplitEpisodes(cold)) {
        // 段本身装得下 → 整段一块;装不下 → 段内按轮再切,仍超(单轮巨型)
        // 就硬按轮界序列切——轮界保证 tool use/result 不被劈开。
        std::vector<std::size_t> boundaries{ep_from};
        std::size_t ep_tokens = EstimateHistoryTokens(
            std::vector<api::Message>(cold.begin() + static_cast<std::ptrdiff_t>(ep_from),
                                      cold.begin() + static_cast<std::ptrdiff_t>(ep_to)));
        if (ep_tokens > chunk_budget) {
            for (std::size_t i = ep_from + 1; i < ep_to; ++i) {
                if (IsUserTurnStartMsg(cold[i])) {
                    boundaries.push_back(i);
                }
            }
        }
        boundaries.push_back(ep_to);
        // 相邻边界合并到不超预算为止(贪心),单轮超预算也自成一块(尽量装)。
        for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
            const std::size_t from = boundaries[b];
            const std::size_t to = boundaries[b + 1];
            if (!chunks.empty()) {
                const Chunk& last = chunks.back();
                const std::size_t merged_tokens = EstimateHistoryTokens(
                    std::vector<api::Message>(cold.begin() + static_cast<std::ptrdiff_t>(last.from),
                                              cold.begin() + static_cast<std::ptrdiff_t>(to)));
                const bool same_episode = from >= ep_from && last.from >= ep_from;
                if (same_episode && merged_tokens <= chunk_budget) {
                    chunks.back().to = to;
                    continue;
                }
            }
            chunks.push_back({from, to});
        }
    }

    // map:各块局部小结。
    const std::string map_instruction = BuildEpisodeInstruction(options);
    std::vector<EpisodeSummary> summaries;
    summaries.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        std::vector<api::Message> messages(cold.begin() + static_cast<std::ptrdiff_t>(chunk.from),
                                           cold.begin() + static_cast<std::ptrdiff_t>(chunk.to));
        const auto text = RequestSummaryText(backend, model, map_instruction, messages,
                                             static_cast<int>(options.budget.output_reserve_tokens));
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        EpisodeSummary summary;
        summary.markdown = TrimWhitespace(*text);
        if (CountUtf8Chars(summary.markdown) < kMinSummaryChars) {
            return std::unexpected(api::Error{
                api::ErrorKind::Api, "第 " + std::to_string(summaries.size() + 1) +
                                         " 块的局部小结为空/过短,历史未动", 0});
        }
        if (const auto manifest = ParseCompactManifest(summary.markdown)) {
            summary.manifest = *manifest;
        }
        summary.evidence_refs = EventRangeForMessages(ledger, chunk.from, chunk.to);
        summary.from_message = chunk.from;
        summary.to_message = chunk.to;
        summaries.push_back(std::move(summary));
    }

    // 归并输入:先拼一份"reduce 材料消息"。
    const auto build_reduce_input = [&]() {
        std::string body;
        if (!prior_state.empty()) {
            body += "[上一轮压缩的存档,仅供参考,与局部小结冲突时以局部小结为准]\n" + prior_state + "\n\n";
        }
        for (std::size_t i = 0; i < summaries.size(); ++i) {
            body += "=== 局部小结 " + std::to_string(i + 1) + "/" + std::to_string(summaries.size()) +
                    " · 来源事件 " + (summaries[i].evidence_refs.empty() ? "?" : summaries[i].evidence_refs) +
                    " ===\n" + summaries[i].markdown + "\n\n";
        }
        api::Message input;
        input.role = api::Role::User;
        input.content.push_back(api::TextBlock{body});
        return std::vector<api::Message>{input};
    };

    // reduce 材料仍超预算 → 两两归并(每轮把相邻两份小结合成一份),压到
    // 装得下为止;有次数护栏,不无限归并。
    const int kMaxReducePasses = 4;
    int passes = 0;
    while (passes < kMaxReducePasses) {
        const std::vector<api::Message> reduce_input = build_reduce_input();
        const std::size_t reduce_tokens = EstimateUtf8Tokens(BuildReduceInstruction(options, !prior_state.empty())) +
                                          EstimateHistoryTokens(reduce_input) +
                                          options.budget.protocol_headroom_tokens;
        if (reduce_tokens <= *input_budget || summaries.size() <= 1) {
            break;
        }
        // 两两归并:相邻两份并成一份(map 的 map,每层仍带来源区间)。
        std::vector<EpisodeSummary> merged;
        const std::string merge_instruction =
            "以上是同一任务相邻两段的局部小结。请把它们归并成一份小结,保留两段的目标、"
            "事实、决策、文件、未完成事项(去重,冲突以后段为准),输出格式与输入相同:"
            "五栏 Markdown + 末尾 ```json manifest。";
        for (std::size_t i = 0; i < summaries.size(); i += 2) {
            if (i + 1 >= summaries.size()) {
                merged.push_back(summaries[i]);
                continue;
            }
            api::Message pair_message;
            pair_message.role = api::Role::User;
            pair_message.content.push_back(api::TextBlock{
                "=== 小结 A · 来源事件 " + summaries[i].evidence_refs + " ===\n" + summaries[i].markdown +
                "\n\n=== 小结 B · 来源事件 " + summaries[i + 1].evidence_refs + " ===\n" +
                summaries[i + 1].markdown});
            const auto text = RequestSummaryText(backend, model, merge_instruction,
                                                 std::vector<api::Message>{pair_message},
                                                 static_cast<int>(options.budget.output_reserve_tokens));
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            EpisodeSummary combined;
            combined.markdown = TrimWhitespace(*text);
            combined.evidence_refs = summaries[i].evidence_refs + "," + summaries[i + 1].evidence_refs;
            combined.from_message = summaries[i].from_message;
            combined.to_message = summaries[i + 1].to_message;
            if (const auto manifest = ParseCompactManifest(combined.markdown)) {
                combined.manifest = *manifest;
            }
            merged.push_back(std::move(combined));
        }
        summaries = std::move(merged);
        ++passes;
    }

    // 终稿 reduce。
    const auto final_text =
        RequestSummaryText(backend, model, BuildReduceInstruction(options, !prior_state.empty()),
                           build_reduce_input(), static_cast<int>(options.budget.output_reserve_tokens));
    if (!final_text.has_value()) {
        return std::unexpected(final_text.error());
    }
    if (CountUtf8Chars(TrimWhitespace(*final_text)) < kMinSummaryChars) {
        return std::unexpected(api::Error{api::ErrorKind::Api, "归并存档为空/过短,历史未动", 0});
    }
    const auto final_manifest = ParseCompactManifest(*final_text);
    if (!final_manifest.has_value()) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api, "归并存档没有可解析的 JSON manifest,历史未动", 0});
    }
    const auto validation = ValidateCompactManifest(*final_manifest, options.required_open_items);
    if (!validation.ok) {
        std::string reasons;
        for (std::size_t i = 0; i < validation.failures.size(); ++i) {
            if (i > 0) {
                reasons += ";";
            }
            reasons += validation.failures[i];
        }
        return std::unexpected(api::Error{api::ErrorKind::Api, "归并存档守恒校验未过,历史未动: " + reasons, 0});
    }

    api::Message archive;
    archive.role = api::Role::User;
    archive.content.push_back(api::TextBlock{"[对话存档,此前内容已压缩] " + TrimWhitespace(*final_text)});
    result.archive = std::move(archive);
    result.manifest = std::move(*final_manifest);
    result.metrics.hierarchical = true;
    result.metrics.implementation = "local-hierarchical";
    result.metrics.chunks = static_cast<int>(chunks.size());
    result.metrics.reduce_passes = passes;
    return result;
}

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

    // 第四道:压了反而更长就别换——省下的空间比摘要本身还小,替换不值。
    // 短历史(估 X token)不比摘要(估 Y token)大时拒收,旧 history 原样。
    if (const std::size_t history_tokens = EstimateHistoryTokens(history);
        history_tokens <= EstimateUtf8Tokens(summary_text)) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api,
            "历史(估 " + std::to_string(history_tokens) + " token)不比摘要本身大,压了反而更长,历史未动", 0});
    }

    api::Message archive;
    archive.role = api::Role::User;
    archive.content.push_back(api::TextBlock{"[对话存档,此前内容已压缩] " + summary_text});
    return CompactSummary{std::move(archive), *manifest};
}

}  // namespace lubancode::agent
