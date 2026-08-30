#include "agent/compact.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "agent/context.hpp"       // 统一 token 估算口径
#include "agent/context_events.hpp"  // 事件账:evidence_refs 的来源区间
#include "agent/sample_model.hpp"  // SampleModel 原语:两处采样的公共路(批一·病四)
#include "platform/text_encoding.hpp"  // SanitizeExternalText:摘要文本进历史前的编码关口

namespace lubancode::agent {

namespace {

// (IsUserTurnStart 的私有拷贝已删:判定收拢到 agent/context.hpp 的公共
// IsUserTurnStart——Compact 四分区单阶段 0,§二 的唯一定义。本文件原先
// 匿名命名空间里还有一份 IsUserTurnStartMsg,同语义同实现,一并删了。)

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
        "goal 与 open_items 不许为空数组/空串;constraints 没有就给空数组,元素必须是字符串;"
        "next_action 不许缺席或为空串。"
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
// 失败(请求失败/流内错误)原样透传;正文交调用方再验。reasoning_effort
// 非空时带上(cheap 路由的 effort 档);accounting 非空时把这次子请求的
// usage 累进去。采样走 SampleModel 原语(批一·病四):攒流/usage/兜错
// 的路只有一份,这里只剩提示拼装;无看门狗无取消、usage 累加、时长
// 不记,都是本处旧口径。
std::expected<std::string, api::Error> RequestSummaryText(api::Backend& backend, const std::string& model,
                                                          const std::string& system,
                                                          const std::vector<api::Message>& messages,
                                                          int max_tokens,
                                                          const std::string& reasoning_effort = std::string(),
                                                          BackgroundCallAccounting* accounting = nullptr) {
    SampleRequest sample;
    sample.model = model;
    sample.system = system;
    sample.messages = messages;
    sample.reasoning_effort = reasoning_effort;
    // 老坑同前:末条 assistant 会被当 prefill continuation,补一条 user 收尾。
    if (!sample.messages.empty() && sample.messages.back().role == api::Role::Assistant) {
        api::Message trailer;
        trailer.role = api::Role::User;
        trailer.content.push_back(api::TextBlock{"请开始,直接给出要求的正文。"});
        sample.messages.push_back(trailer);
    }
    sample.max_tokens = max_tokens;

    const SampleResult result = SampleModel(backend, sample);
    AddSampleAccounting(accounting, result);
    if (!result.ok) {
        return std::unexpected(result.error);
    }
    return result.text;
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
        "goal 不许为空;没有未完成事项 open_items 给空数组;next_action 不许缺席或为空串。";
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
        "goal 与 open_items 不许为空数组/空串;constraints 没有就给空数组,元素必须是字符串;"
        "next_action 不许缺席或为空串。";
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
        if (IsUserTurnStart(history[i]) || HasTodoWrite(history[i])) {
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
                                                                     const CompactOptions& options,
                                                                     const std::string& reasoning_effort,
                                                                     BackgroundCallAccounting* accounting) {
    LayeredCompactResult result;
    // 计时守卫:任何 return 路径(含错误)都把墙钟记进账,调用方拿去按
    // 角色记账。Compact() 自己也写 duration(单次路),这里最后覆盖成整场
    // 分层的总时——语义就是"这次压缩任务一共花了多久"。
    struct AccountingGuard {
        BackgroundCallAccounting* accounting;
        std::chrono::steady_clock::time_point started;
        ~AccountingGuard() {
            if (accounting == nullptr) {
                return;
            }
            accounting->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count();
        }
    } accounting_guard{accounting, std::chrono::steady_clock::now()};
    // 分层路内部各次子请求都从这一份出账(单次路交给 Compact() 自己记)。
    const auto map_accounting = accounting;
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
        const auto single = Compact(backend, model, history, options, reasoning_effort, accounting);
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
        const auto single = Compact(backend, model, history, options, reasoning_effort, accounting);
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
        const auto single = Compact(backend, model, history, options, reasoning_effort, accounting);
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
                if (IsUserTurnStart(cold[i])) {
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
                                             static_cast<int>(options.budget.output_reserve_tokens),
                                             reasoning_effort, map_accounting);
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
                                                 static_cast<int>(options.budget.output_reserve_tokens),
                                                 reasoning_effort, map_accounting);
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
                           build_reduce_input(), static_cast<int>(options.budget.output_reserve_tokens),
                           reasoning_effort, map_accounting);
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
    // schema/type 收紧(Compact 四分区单阶段 0,照 §四 UserContract 的字段
    // 规矩):早先 constraints 混进非字符串、next_action 缺席或空串时,解析
    // 一律静默吞掉——摘要看着守恒,字段其实残了。现在类型不对、键缺席、
    // 字符串剥空白后为空,整枚 manifest 判坏(nullopt),调用方拒收、旧
    // history 不动;坏值不许半截收编。
    if (!parsed.is_object() || !parsed.contains("goal") || !parsed["goal"].is_string() ||
        !parsed.contains("open_items") || !parsed["open_items"].is_array() ||
        !parsed.contains("next_action") || !parsed["next_action"].is_string()) {
        return std::nullopt;
    }
    if (TrimWhitespace(parsed["goal"].get<std::string>()).empty() ||
        TrimWhitespace(parsed["next_action"].get<std::string>()).empty()) {
        return std::nullopt;
    }
    // 字符串数组的三道同规矩:必须是数组、元素必须是字符串、元素剥空白后
    // 非空。constraints 没写合法(等价空数组),写了就不许坏。
    if (parsed.contains("constraints")) {
        if (!parsed["constraints"].is_array()) {
            return std::nullopt;
        }
        for (const auto& item : parsed["constraints"]) {
            if (!item.is_string() || TrimWhitespace(item.get<std::string>()).empty()) {
                return std::nullopt;
            }
        }
    }
    for (const auto& item : parsed["open_items"]) {
        if (!item.is_string() || TrimWhitespace(item.get<std::string>()).empty()) {
            return std::nullopt;
        }
    }
    CompactManifest manifest;
    manifest.goal = parsed["goal"].get<std::string>();
    for (const auto& item : parsed["open_items"]) {
        manifest.open_items.push_back(item.get<std::string>());
    }
    if (parsed.contains("constraints")) {
        for (const auto& item : parsed["constraints"]) {
            manifest.constraints.push_back(item.get<std::string>());
        }
    }
    manifest.next_action = parsed["next_action"].get<std::string>();
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
                                                const api::Message& archive, std::size_t hot_zone_tokens,
                                                std::vector<std::size_t>* kept_indices_out) {
    std::vector<api::Message> new_history;

    // 按轮切:turns[i] = [start, end),切法收拢在 SplitIntoTurns(§二 唯一
    // 定义,磁盘账与内存路共用),不再自带一份轮界循环。
    const std::vector<std::pair<std::size_t, std::size_t>> turns = SplitIntoTurns(history);
    if (turns.empty()) {
        // 没有可保留的用户轮,存档只能自己单独成一条。
        new_history.push_back(archive);
        return new_history;
    }

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
    if (used <= hot_zone_tokens) {
        // 末轮装得下:从最后一轮整轮保留起,往前按轮收,收满 token 预算
        // 为止。再往前的轮只有整个装得下预算才进来,装不下就停在轮边界,
        // tool_use/tool_result 的配对天然不被切开。
        for (std::size_t t = turns.size() - 1; t-- > 0;) {
            const std::size_t tokens = turn_tokens(t);
            if (used + tokens > hot_zone_tokens) {
                break;
            }
            used += tokens;
            keep_from = turns[t].first;
        }
    }
    // 末轮超预算(P1-1 反涨的真机形状):mid-turn 压缩时,从最新用户消息
    // 到尾全在一个"轮"里——长工具循环能攒几十 k 的工具来回,整轮保留等
    // 于没压,archive 添在头上反而更长(实测 70.8k -> 73.7k)。这时热区
    // 预算当真:轮头那条 user 消息(最新用户输入)必保,其后按"消息组"
    // 收——一组 = 一条 assistant 消息(可能带 tool_use)加紧随的
    // user(tool_result) 消息,组内天然配对不劈;收满预算为止,没收进的
    // 中段交给存档概括。保留集在下面 kept_indices 段就地构造。

    // 保留区的消息下标(升序):末轮装得下时是 [keep_from, end) 连续段;
    // 末轮超预算走组收法时是"轮头 + 收进的若干尾部组",没收进的中段组
    // 直接不在列——那部分交给存档概括,不占热区(这正是 mid-turn 巨轮能
    // 收窄的那一刀)。
    std::vector<std::size_t> kept_indices;
    if (used > hot_zone_tokens) {
        const std::size_t turn_begin = turns.back().first;
        const std::size_t turn_end = history.size();
        // 先把末轮切成消息组:一组 = 一条 assistant 消息(可能带 tool_use)
        // 加紧随的 user(tool_result) 消息,组内天然配对不劈。
        std::vector<std::pair<std::size_t, std::size_t>> groups;
        for (std::size_t i = turn_begin + 1; i < turn_end;) {
            if (history[i].role != api::Role::Assistant) {
                ++i;  // 轮头后的零散 user 片段(理论少见),不进热区
                continue;
            }
            std::size_t to = i + 1;
            while (to < turn_end && history[to].role == api::Role::User && !IsUserTurnStart(history[to])) {
                ++to;
            }
            groups.emplace_back(i, to);
            i = to;
        }
        // 从尾往前收:最近的工具来回最相关,装得下就保;中段装不下的交给
        // 存档概括。
        std::vector<std::pair<std::size_t, std::size_t>> kept_groups;
        std::size_t tail_used = message_tokens[turn_begin];
        for (std::size_t g = groups.size(); g-- > 0;) {
            std::size_t group_tokens = 0;
            for (std::size_t j = groups[g].first; j < groups[g].second; ++j) {
                group_tokens += message_tokens[j];
            }
            if (tail_used + group_tokens > hot_zone_tokens) {
                continue;
            }
            tail_used += group_tokens;
            kept_groups.push_back(groups[g]);
        }
        kept_indices.push_back(turn_begin);  // 最新用户输入绝不丢
        for (std::size_t g = kept_groups.size(); g-- > 0;) {
            for (std::size_t j = kept_groups[g].first; j < kept_groups[g].second; ++j) {
                kept_indices.push_back(j);
            }
        }
    } else {
        for (std::size_t i = keep_from; i < history.size(); ++i) {
            kept_indices.push_back(i);
        }
    }

    // 存档正文并入保留区第一条 user 消息开头,不单独成一条消息——独立的
    // 存档 user 消息紧跟热区的 user 输入,就是相邻两条 user,违反 Anthropic
    // 的角色交替要求(标准端点 400;MiniMax 宽容,才一直没暴露)。两条路
    // 的第一条都是 user 文本消息(整轮路是某轮轮头;组收路是末轮轮头)。
    std::string archive_text;
    for (const auto& block : archive.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            archive_text += std::get<api::TextBlock>(block).text;
        }
    }

    new_history.reserve(kept_indices.size());
    bool archive_merged = false;
    for (const std::size_t index : kept_indices) {
        api::Message message = history[index];
        if (!archive_merged) {
            bool merged_into_text = false;
            for (auto& block : message.content) {
                if (std::holds_alternative<api::TextBlock>(block)) {
                    auto& text_block = std::get<api::TextBlock>(block);
                    text_block.text = archive_text + "\n\n" + text_block.text;
                    merged_into_text = true;
                    break;
                }
            }
            if (!merged_into_text) {
                // IsUserTurnStart 保证有 TextBlock,这里纯防御。
                message.content.push_back(api::TextBlock{archive_text});
            }
            archive_merged = true;
        }
        new_history.push_back(std::move(message));
    }
    if (kept_indices_out != nullptr) {
        *kept_indices_out = std::move(kept_indices);
    }

    return new_history;
}

std::expected<CompactSummary, api::Error> Compact(api::Backend& backend, const std::string& model,
                                                  const std::vector<api::Message>& history,
                                                  const CompactOptions& options, const std::string& reasoning_effort,
                                                  BackgroundCallAccounting* accounting) {
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

    // 采样走 SampleModel 原语(批一·病四):无看门狗无取消、usage 累加、
    // 时长首包覆盖,全是本处旧口径。
    SampleRequest sample;
    sample.model = model;
    sample.system = instruction;
    sample.messages = history;
    sample.reasoning_effort = reasoning_effort;

    // 踩过的坑:history 最后一条常常是 assistant 消息,原样发出去会被
    // Anthropic/Responses 两边当成"续写最后这条 assistant 消息"(prefill
    // continuation)处理,模型不理会 system 里的总结指令,只顺着最后一条
    // 内容随手接几个字——实测出过压缩正文只有孤零零一个"2"字的情况。
    // 补一条 user 消息收尾,让模型明确进入"该我说话、按 system 指令办"
    // 的状态。末条本来就是 user(比如工具结果)时天然该轮到它开口,不补。
    if (!sample.messages.empty() && sample.messages.back().role == api::Role::Assistant) {
        api::Message trailer;
        trailer.role = api::Role::User;
        trailer.content.push_back(api::TextBlock{"请开始压缩,直接给出存档正文与末尾的 JSON manifest,不要重复原对话内容。"});
        sample.messages.push_back(trailer);
    }
    sample.max_tokens = static_cast<int>(options.budget.output_reserve_tokens);

    const SampleResult sampled = SampleModel(backend, sample);

    // usage 出账(分角色记账):压缩额外花的这轮采样不混进普通 turn 的账,
    // 交给调用方记进 ModelUsageLedger。
    AddSampleAccounting(accounting, sampled);
    if (accounting != nullptr) {
        accounting->duration_ms = sampled.duration_ms;
    }

    if (!sampled.ok) {
        return std::unexpected(sampled.error);
    }
    const std::string& summary_text = sampled.text;

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
    // 摘要文本是模型输出的,可能带坏串,进历史前清洗(和 assembler 收块
    // 同一道关口,防止下一轮 wire 序列化 316)。
    archive.content.push_back(api::TextBlock{
        "[对话存档,此前内容已压缩] " + platform::SanitizeExternalText(summary_text)});
    return CompactSummary{std::move(archive), *manifest};
}

// ---------------------------------------------------------------------------
// 四分区(阶段 1):TurnPartitionPlan 纯计算
// ---------------------------------------------------------------------------

TurnPartitionPlan BuildTurnPartitionPlan(const std::vector<api::Message>& history,
                                         std::size_t partition_count,
                                         const TurnPartitionBudgets& budgets) {
    TurnPartitionPlan plan;
    plan.requested_partition_count = partition_count;
    plan.compact_input_budget = CompactInputBudget(budgets.compact_model);
    if (history.empty()) {
        return plan;
    }

    // L1 工作视图(§3.3):分区按结构压缩后的稳定视图计量,长 ToolResult 按
    // artifact 外置后的重量算,不拿 durable 里的全文虚算。CompressWorkingView
    // 只重写 tool_result 的 content,消息条数与块序不动,视图与原 history
    // 逐条对得上;临时 memo/stats/store——不落盘、不定形、不碰会话真账。
    StructuralCompressionStats stats;
    ResultViewMemo memo;
    const std::vector<api::Message> working =
        CompressWorkingView(history, budgets.structural, stats, memo, /*store=*/nullptr);

    // 旧 archive 剥离(§3.2):只在首条消息的第一枚文本块上找,与分层压缩
    // 同一只。剥出的文本不算 turn、不占分区账,只作 final reduce 的基线。
    for (const auto& block : history[0].content) {
        if (!std::holds_alternative<api::TextBlock>(block)) {
            continue;
        }
        if (auto split = SplitPriorArchive(std::get<api::TextBlock>(block).text)) {
            plan.has_prior_archive = true;
            plan.prior_archive_text = split->first;
            plan.prior_archive_tokens = EstimateUtf8Tokens(plan.prior_archive_text);
        }
        break;
    }

    // 逐条 token:工作视图一把(分区用)、全量一把(对照外置收益)。每条至少
    // 记 1,空壳消息不白占预算(与 BuildCompactedHistory 同一口径)。
    std::vector<std::size_t> working_message_tokens(history.size());
    std::vector<std::size_t> raw_message_tokens(history.size());
    std::vector<std::size_t> externalized_message(history.size(), 0);
    for (std::size_t i = 0; i < history.size(); ++i) {
        working_message_tokens[i] = std::max<std::size_t>(1, EstimateMessageTokens(working[i]));
        raw_message_tokens[i] = std::max<std::size_t>(1, EstimateMessageTokens(history[i]));
        for (const auto& block : history[i].content) {
            if (!std::holds_alternative<api::ToolResultBlock>(block)) {
                continue;
            }
            // 已外置 = 首次定形成 artifact 视图(头尾预览 + 可追回引用)。
            const std::string& use_id = std::get<api::ToolResultBlock>(block).tool_use_id;
            if (const auto it = memo.decisions.find(use_id);
                it != memo.decisions.end() && it->second.kind == ResultViewKind::Artifact) {
                externalized_message[i] += 1;
            }
        }
    }

    // 按 §二 切 turn。首枚 turn 头之前若有零散消息(旧档外壳、异常形状),
    // 并入首 turn 记账——plan 的账要盖住整份 history,零散头没有自己的去处。
    const std::vector<std::pair<std::size_t, std::size_t>> raw_ranges = SplitIntoTurns(history);
    if (raw_ranges.empty()) {
        return plan;  // 一条真正用户输入都没有:没有可分区的 turn
    }
    std::vector<std::pair<std::size_t, std::size_t>> turn_ranges = raw_ranges;
    turn_ranges.front().first = 0;

    // 工具原子组(§6.1):按 tool_use_id 收齐本 assistant 消息发出的全部
    // 调用,不按"下一条 user 消息"猜配对;use 无 result = incomplete,
    // result 配不上 use = 悬空。todo_write 照样成组,天然随组走不劈开。
    std::map<std::string, std::size_t> result_message_of;
    for (std::size_t i = 0; i < history.size(); ++i) {
        for (const auto& block : history[i].content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result != nullptr) {
                result_message_of.emplace(result->tool_use_id, i);  // 首次出现为准
            }
        }
    }
    std::set<std::string> matched_uses;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (history[i].role != api::Role::Assistant) {
            continue;
        }
        std::vector<std::string> ids;
        for (const auto& block : history[i].content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block); use != nullptr) {
                ids.push_back(use->id);
            }
        }
        if (ids.empty()) {
            continue;
        }
        ToolExchangeGroupInfo group;
        group.assistant_message = i;
        group.from_message = i;
        group.to_message = i + 1;
        group.tool_use_ids = ids;
        for (const auto& id : ids) {
            const auto it = result_message_of.find(id);
            if (it == result_message_of.end() || it->second < i) {
                group.complete = false;  // result 缺失或跑到 use 前头:orphan
                continue;
            }
            matched_uses.insert(id);
            group.to_message = std::max(group.to_message, it->second + 1);
        }
        // 所属 turn:assistant 消息落在哪枚 turn 的区间里。
        for (std::size_t t = 0; t < turn_ranges.size(); ++t) {
            if (i >= turn_ranges[t].first && i < turn_ranges[t].second) {
                group.turn = t;
                break;
            }
        }
        plan.tool_groups.push_back(std::move(group));
    }
    for (const auto& [id, message_index] : result_message_of) {
        (void)message_index;
        if (matched_uses.count(id) == 0) {
            plan.dangling_results += 1;
        }
    }
    plan.has_incomplete_tool_exchange =
        plan.dangling_results > 0 ||
        std::any_of(plan.tool_groups.begin(), plan.tool_groups.end(),
                    [](const ToolExchangeGroupInfo& group) { return !group.complete; });

    // turn 画像:token 按区间累加,首 turn 扣掉已剥离的旧 archive 账。
    plan.turns.reserve(turn_ranges.size());
    for (std::size_t t = 0; t < turn_ranges.size(); ++t) {
        TurnInfo info;
        info.number = t + 1;
        info.id = "t" + std::to_string(t + 1);
        info.from_message = turn_ranges[t].first;
        info.to_message = turn_ranges[t].second;
        for (std::size_t i = info.from_message; i < info.to_message; ++i) {
            info.working_tokens += working_message_tokens[i];
            info.raw_tokens += raw_message_tokens[i];
            info.externalized_results += externalized_message[i];
        }
        if (t == 0) {
            info.working_tokens = info.working_tokens > plan.prior_archive_tokens
                                      ? info.working_tokens - plan.prior_archive_tokens
                                      : 0;
            info.raw_tokens = info.raw_tokens > plan.prior_archive_tokens
                                  ? info.raw_tokens - plan.prior_archive_tokens
                                  : 0;
        }
        info.tool_groups = static_cast<std::size_t>(
            std::count_if(plan.tool_groups.begin(), plan.tool_groups.end(),
                          [t](const ToolExchangeGroupInfo& group) { return group.turn == t; }));
        plan.total_working_tokens += info.working_tokens;
        plan.total_raw_tokens += info.raw_tokens;
        plan.externalized_results += info.externalized_results;
        plan.turns.push_back(std::move(info));
    }

    // 分区(§3.3):把有序 turns 切成 min(turn 数, partition_count) 份连续
    // 分区,目标 token 大致相等。切口只落 turn 边界;每份至少一枚 turn。理
    // 想切点取 total*k/parts,实际边界取前缀和最靠近理想点的那一枚 turn 边
    // 界(整数账,不引浮点);并列取更早的边界,保证确定性。末份固定热区,
    // 巨型末 turn 情形(§9.1)天然退成"热区只剩最后一枚 turn,较老的落进
    // 前一份被总结"。
    const std::size_t turn_count = plan.turns.size();
    const std::size_t wanted = partition_count == 0 ? 1 : partition_count;
    const std::size_t parts = std::min(turn_count, wanted);
    plan.map_calls = parts > 0 ? parts - 1 : 0;

    std::vector<std::size_t> prefix(turn_count + 1, 0);
    for (std::size_t t = 0; t < turn_count; ++t) {
        prefix[t + 1] = prefix[t] + plan.turns[t].working_tokens;
    }
    const std::size_t total = prefix[turn_count];
    std::vector<std::size_t> bounds;
    bounds.push_back(0);
    std::size_t previous = 0;
    for (std::size_t k = 1; k < parts; ++k) {
        // 理想切点的第 k 份边界:prefix[b]*parts 最接近 total*k 的 b。
        // 搜索域 [previous+1, turn_count-(parts-k)]:边界严格递增,且给后面
        // 每份至少留一枚 turn。
        const std::size_t ideal = total * k;
        std::size_t best = previous + 1;
        std::size_t best_diff = std::numeric_limits<std::size_t>::max();
        for (std::size_t b = previous + 1; b + (parts - k) <= turn_count; ++b) {
            const std::size_t scaled = prefix[b] * parts;
            const std::size_t diff = scaled > ideal ? scaled - ideal : ideal - scaled;
            if (diff < best_diff) {
                best = b;
                best_diff = diff;
            }
        }
        bounds.push_back(best);
        previous = best;
    }
    bounds.push_back(turn_count);

    const auto map_budget = plan.compact_input_budget;
    for (std::size_t p = 0; p + 1 < bounds.size(); ++p) {
        TurnPartitionInfo partition;
        partition.label = "P" + std::to_string(p + 1);
        partition.first_turn = bounds[p];
        partition.last_turn = bounds[p + 1];
        partition.is_hot = p + 1 == bounds.size() - 1;
        for (std::size_t t = partition.first_turn; t < partition.last_turn; ++t) {
            partition.working_tokens += plan.turns[t].working_tokens;
            partition.externalized_results += plan.turns[t].externalized_results;
        }
        if (map_budget.has_value() &&
            partition.working_tokens + kTurnPlanPromptOverheadTokens > *map_budget) {
            partition.over_map_budget = true;
            plan.any_partition_over_map_budget = true;
        }
        plan.partitions.push_back(std::move(partition));
    }
    if (map_budget.has_value()) {
        for (const auto& turn : plan.turns) {
            if (turn.working_tokens + kTurnPlanPromptOverheadTokens > *map_budget) {
                plan.any_turn_over_map_budget = true;
                break;
            }
        }
    }
    return plan;
}

}  // namespace lubancode::agent
