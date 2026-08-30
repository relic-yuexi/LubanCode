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

// ---------------------------------------------------------------------------
// 四分区·阶段 2-4:TurnGroupSummary map、双账 final reduce、新 history
// ---------------------------------------------------------------------------

namespace {

// "t7" -> 7;不是合法 turn 号给 nullopt(覆盖方向、证据排序都靠它比先后)。
std::optional<std::size_t> TurnOrdinal(const std::string& id) {
    if (id.size() < 2 || id[0] != 't') {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (std::size_t i = 1; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::size_t>(id[i] - '0');
        if (value > 100000000) {  // 防溢出的防御闸
            return std::nullopt;
        }
    }
    return value == 0 ? std::optional<std::size_t>() : value;
}

// map/reduce 输出的 JSON 提取:优先认单枚 ```json 围栏,认不着就当裸 JSON
// 整段解析。围栏只是宽容一道(模型手滑加围栏),不改变"只收 JSON"的规矩。
std::optional<nlohmann::json> ParseJsonObjectLoose(const std::string& text) {
    std::string body = TrimWhitespace(text);
    const std::size_t fence = body.find("```json");
    if (fence != std::string::npos) {
        const std::size_t body_begin = body.find('\n', fence);
        const std::size_t fence_end = body_begin == std::string::npos
                                          ? std::string::npos
                                          : body.find("```", body_begin + 1);
        if (body_begin == std::string::npos || fence_end == std::string::npos) {
            return std::nullopt;
        }
        body = TrimWhitespace(body.substr(body_begin + 1, fence_end - body_begin - 1));
    }
    if (body.empty() || body.front() != '{') {
        return std::nullopt;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (...) {
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

// 字符串数组读档:必须是数组、元素必须是字符串、剥空白非空。
bool JsonStringArray(const nlohmann::json& json, const char* key, std::vector<std::string>& out) {
    if (!json.contains(key) || !json[key].is_array()) {
        return false;
    }
    for (const auto& item : json[key]) {
        if (!item.is_string() || TrimWhitespace(item.get<std::string>()).empty()) {
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

// requirement 读档:{id?, text, source_turns:[...]};id 是否必填由调用方定
// (goal 不带 id,其余都带)。
bool ParseRequirement(const nlohmann::json& json, bool require_id, ContractRequirement& out) {
    if (!json.is_object() || !json.contains("text") || !json["text"].is_string()) {
        return false;
    }
    if (require_id && (!json.contains("id") || !json["id"].is_string())) {
        return false;
    }
    if (TrimWhitespace(json["text"].get<std::string>()).empty()) {
        return false;
    }
    if (require_id && TrimWhitespace(json["id"].get<std::string>()).empty()) {
        return false;
    }
    out.id = require_id ? json["id"].get<std::string>() : std::string();
    out.text = json["text"].get<std::string>();
    return JsonStringArray(json, "source_turns", out.source_turns) && !out.source_turns.empty();
}

}  // namespace

std::optional<TurnGroupSummary> ParseTurnGroupSummary(const std::string& text) {
    const auto parsed = ParseJsonObjectLoose(text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    TurnGroupSummary summary;
    if (!JsonStringArray(*parsed, "user_requirement_changes", summary.user_requirement_changes) ||
        !JsonStringArray(*parsed, "confirmed_facts", summary.confirmed_facts) ||
        !JsonStringArray(*parsed, "files", summary.files) ||
        !JsonStringArray(*parsed, "changes_made", summary.changes_made) ||
        !JsonStringArray(*parsed, "failed_attempts", summary.failed_attempts) ||
        !JsonStringArray(*parsed, "open_items", summary.open_items) ||
        !JsonStringArray(*parsed, "next_step_candidates", summary.next_step_candidates)) {
        return std::nullopt;
    }
    if (!parsed->contains("tool_results") || !(*parsed)["tool_results"].is_array()) {
        return std::nullopt;
    }
    for (const auto& item : (*parsed)["tool_results"]) {
        if (!item.is_object() || !item.contains("tool") || !item["tool"].is_string() ||
            !item.contains("result") || !item["result"].is_string()) {
            return std::nullopt;
        }
        TurnGroupToolResult note;
        note.tool = item["tool"].get<std::string>();
        note.result = item["result"].get<std::string>();
        if (TrimWhitespace(note.tool).empty() || TrimWhitespace(note.result).empty()) {
            return std::nullopt;
        }
        if (item.contains("evidence")) {
            if (!item["evidence"].is_string() || TrimWhitespace(item["evidence"].get<std::string>()).empty()) {
                return std::nullopt;
            }
            note.evidence = item["evidence"].get<std::string>();
        }
        summary.tool_results.push_back(std::move(note));
    }
    return summary;
}

nlohmann::json ToJson(const UserContract& contract) {
    nlohmann::json json;
    nlohmann::json goal;
    goal["text"] = contract.goal.text;
    goal["source_turns"] = contract.goal.source_turns;
    json["goal"] = std::move(goal);
    const auto dump_requirements = [](const std::vector<ContractRequirement>& requirements) {
        std::vector<nlohmann::json> items;
        items.reserve(requirements.size());
        for (const auto& requirement : requirements) {
            nlohmann::json item;
            item["id"] = requirement.id;
            item["text"] = requirement.text;
            item["source_turns"] = requirement.source_turns;
            items.push_back(std::move(item));
        }
        return items;
    };
    json["active_constraints"] = dump_requirements(contract.active_constraints);
    json["acceptance_criteria"] = dump_requirements(contract.acceptance_criteria);
    json["additions"] = dump_requirements(contract.additions);
    std::vector<nlohmann::json> superseded;
    superseded.reserve(contract.superseded_requirements.size());
    for (const auto& item : contract.superseded_requirements) {
        nlohmann::json entry;
        entry["id"] = item.id;
        entry["text"] = item.text;
        entry["source_turns"] = item.source_turns;
        entry["superseded_by"] = item.superseded_by;
        entry["superseded_at_turn"] = item.superseded_at_turn;
        superseded.push_back(std::move(entry));
    }
    json["superseded_requirements"] = std::move(superseded);
    std::vector<nlohmann::json> questions;
    questions.reserve(contract.open_questions.size());
    for (const auto& question : contract.open_questions) {
        nlohmann::json item;
        item["text"] = question.text;
        item["source_turns"] = question.source_turns;
        questions.push_back(std::move(item));
    }
    json["open_questions"] = std::move(questions);
    return json;
}

std::optional<UserContract> ParseUserContract(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("goal") || !json["goal"].is_object()) {
        return std::nullopt;
    }
    UserContract contract;
    if (!ParseRequirement(json["goal"], /*require_id=*/false, contract.goal)) {
        return std::nullopt;
    }
    const auto parse_list = [&](const char* key, std::vector<ContractRequirement>& out) {
        if (!json.contains(key) || !json[key].is_array()) {
            return false;
        }
        for (const auto& item : json[key]) {
            ContractRequirement requirement;
            if (!ParseRequirement(item, /*require_id=*/true, requirement)) {
                return false;
            }
            out.push_back(std::move(requirement));
        }
        return true;
    };
    if (!parse_list("active_constraints", contract.active_constraints) ||
        !parse_list("acceptance_criteria", contract.acceptance_criteria) ||
        !parse_list("additions", contract.additions)) {
        return std::nullopt;
    }
    if (!json.contains("superseded_requirements") || !json["superseded_requirements"].is_array()) {
        return std::nullopt;
    }
    for (const auto& item : json["superseded_requirements"]) {
        if (!item.is_object()) {
            return std::nullopt;
        }
        ContractRequirement base;
        if (!ParseRequirement(item, /*require_id=*/true, base)) {
            return std::nullopt;
        }
        if (!item.contains("superseded_by") || !item["superseded_by"].is_string() ||
            TrimWhitespace(item["superseded_by"].get<std::string>()).empty() ||
            !item.contains("superseded_at_turn") || !item["superseded_at_turn"].is_string() ||
            TrimWhitespace(item["superseded_at_turn"].get<std::string>()).empty()) {
            return std::nullopt;
        }
        SupersededRequirement requirement;
        requirement.id = base.id;
        requirement.text = base.text;
        requirement.source_turns = base.source_turns;
        requirement.superseded_by = item["superseded_by"].get<std::string>();
        requirement.superseded_at_turn = item["superseded_at_turn"].get<std::string>();
        contract.superseded_requirements.push_back(std::move(requirement));
    }
    if (!json.contains("open_questions") || !json["open_questions"].is_array()) {
        return std::nullopt;
    }
    for (const auto& item : json["open_questions"]) {
        if (!item.is_object()) {
            return std::nullopt;
        }
        ContractOpenQuestion question;
        if (!item.contains("text") || !item["text"].is_string() ||
            TrimWhitespace(item["text"].get<std::string>()).empty()) {
            return std::nullopt;
        }
        question.text = item["text"].get<std::string>();
        if (!JsonStringArray(item, "source_turns", question.source_turns) ||
            question.source_turns.empty()) {
            return std::nullopt;
        }
        contract.open_questions.push_back(std::move(question));
    }
    return contract;
}

nlohmann::json ToJson(const WorkState& state) {
    nlohmann::json json;
    const auto dump_notes = [](const std::vector<StateEvidenceNote>& notes) {
        std::vector<nlohmann::json> items;
        items.reserve(notes.size());
        for (const auto& note : notes) {
            nlohmann::json item;
            item["text"] = note.text;
            item["evidence_refs"] = note.evidence_refs;
            items.push_back(std::move(item));
        }
        return items;
    };
    json["confirmed_facts"] = dump_notes(state.confirmed_facts);
    std::vector<nlohmann::json> tool_results;
    tool_results.reserve(state.tool_results.size());
    for (const auto& note : state.tool_results) {
        nlohmann::json item;
        item["tool"] = note.tool;
        item["result"] = note.result;
        item["evidence_refs"] = note.evidence_refs;
        tool_results.push_back(std::move(item));
    }
    json["tool_results"] = std::move(tool_results);
    json["files"] = state.files;
    json["changes_made"] = dump_notes(state.changes_made);
    json["failed_attempts"] = dump_notes(state.failed_attempts);
    json["open_items"] = state.open_items;
    json["next_action"] = state.next_action;
    return json;
}

std::optional<WorkState> ParseWorkState(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("next_action") || !json["next_action"].is_string() ||
        TrimWhitespace(json["next_action"].get<std::string>()).empty()) {
        return std::nullopt;
    }
    WorkState state;
    state.next_action = json["next_action"].get<std::string>();
    const auto parse_notes = [&](const char* key, std::vector<StateEvidenceNote>& out) {
        if (!json.contains(key) || !json[key].is_array()) {
            return false;
        }
        for (const auto& item : json[key]) {
            if (!item.is_object() || !item.contains("text") || !item["text"].is_string()) {
                return false;
            }
            StateEvidenceNote note;
            note.text = item["text"].get<std::string>();
            if (TrimWhitespace(note.text).empty()) {
                return false;
            }
            if (!JsonStringArray(item, "evidence_refs", note.evidence_refs)) {
                return false;
            }
            out.push_back(std::move(note));
        }
        return true;
    };
    if (!parse_notes("confirmed_facts", state.confirmed_facts) ||
        !parse_notes("changes_made", state.changes_made) ||
        !parse_notes("failed_attempts", state.failed_attempts)) {
        return std::nullopt;
    }
    if (!json.contains("tool_results") || !json["tool_results"].is_array()) {
        return std::nullopt;
    }
    for (const auto& item : json["tool_results"]) {
        if (!item.is_object() || !item.contains("tool") || !item["tool"].is_string() ||
            !item.contains("result") || !item["result"].is_string()) {
            return std::nullopt;
        }
        StateToolResult note;
        note.tool = item["tool"].get<std::string>();
        note.result = item["result"].get<std::string>();
        if (TrimWhitespace(note.tool).empty() || TrimWhitespace(note.result).empty()) {
            return std::nullopt;
        }
        if (!JsonStringArray(item, "evidence_refs", note.evidence_refs)) {
            return std::nullopt;
        }
        state.tool_results.push_back(std::move(note));
    }
    if (!JsonStringArray(json, "files", state.files) ||
        !JsonStringArray(json, "open_items", state.open_items)) {
        return std::nullopt;
    }
    return state;
}

namespace {

// 一组 turn id 全部有效(存在且真是宿主钉的 turn 号)才收;invalid_out 非空
// 时顺手记下第一枚不合法的 id(拒收消息点名用)。
bool AllTurnIdsValid(const std::vector<std::string>& ids, const std::set<std::string>& valid_turn_ids,
                     std::string* invalid_out = nullptr) {
    for (const auto& id : ids) {
        if (valid_turn_ids.count(id) == 0) {
            if (invalid_out != nullptr) {
                *invalid_out = id;
            }
            return false;
        }
    }
    return true;
}

// 把一组 id 拼成 "t1,t2"(拒收消息点名用)。
std::string JoinTurnIds(const std::vector<std::string>& ids) {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += ids[i];
    }
    return out;
}

// 一枚 turn 的最晚序号(比覆盖先后用);任一 id 不合法给 nullopt。
std::optional<std::size_t> LatestTurnOrdinal(const std::vector<std::string>& ids) {
    std::optional<std::size_t> latest;
    for (const auto& id : ids) {
        const auto ordinal = TurnOrdinal(id);
        if (!ordinal.has_value()) {
            return std::nullopt;
        }
        if (!latest.has_value() || *ordinal > *latest) {
            latest = ordinal;
        }
    }
    return latest;
}

}  // namespace

std::vector<std::string> ValidateUserContract(const UserContract& contract,
                                              const std::set<std::string>& valid_turn_ids) {
    std::vector<std::string> failures;
    if (TrimWhitespace(contract.goal.text).empty()) {
        failures.push_back("用户契约缺目标(goal)");
    } else if (contract.goal.source_turns.empty()) {
        failures.push_back("目标缺少来源 turn(source_turns)");
    } else if (std::string bad_id; !AllTurnIdsValid(contract.goal.source_turns, valid_turn_ids, &bad_id)) {
        failures.push_back("目标的来源 turn 不存在: " + bad_id);
    }

    // requirement 总册:id 唯一、文本非空、来源合法。id -> 最晚来源序号,
    // 覆盖方向校验用。
    std::map<std::string, std::size_t> requirement_latest_turn;
    const auto collect = [&](const std::vector<ContractRequirement>& requirements, const char* what) {
        for (const auto& requirement : requirements) {
            const std::string label = std::string(what) + "「" + requirement.text.substr(0, 24) + "」";
            if (TrimWhitespace(requirement.id).empty()) {
                failures.push_back(std::string(what) + "有条目缺 id");
                continue;
            }
            if (requirement_latest_turn.count(requirement.id) > 0) {
                failures.push_back("requirement id 重复: " + requirement.id);
                continue;
            }
            if (TrimWhitespace(requirement.text).empty()) {
                failures.push_back(label + " 缺正文");
                continue;
            }
            if (requirement.source_turns.empty()) {
                failures.push_back(label + " 缺少来源 turn(source_turns)");
                continue;
            }
            if (std::string bad_id; !AllTurnIdsValid(requirement.source_turns, valid_turn_ids, &bad_id)) {
                failures.push_back(label + " 的来源 turn 不存在: " + bad_id + "(来源: " +
                                   JoinTurnIds(requirement.source_turns) + ")");
                continue;
            }
            if (const auto latest = LatestTurnOrdinal(requirement.source_turns)) {
                requirement_latest_turn[requirement.id] = *latest;
            }
        }
    };
    collect(contract.active_constraints, "活动约束");
    collect(contract.acceptance_criteria, "验收条件");
    collect(contract.additions, "补充要求");

    for (const auto& item : contract.superseded_requirements) {
        const std::string label = std::string("被废旧要求「") + item.text.substr(0, 24) + "」";
        if (TrimWhitespace(item.id).empty() || TrimWhitespace(item.text).empty()) {
            failures.push_back("superseded 列表有条目缺 id 或正文");
            continue;
        }
        if (item.source_turns.empty() || !AllTurnIdsValid(item.source_turns, valid_turn_ids)) {
            failures.push_back(label + " 缺少合法的来源 turn(source_turns)");
            continue;
        }
        const auto old_latest = LatestTurnOrdinal(item.source_turns);
        const auto at_turn = TurnOrdinal(item.superseded_at_turn);
        if (!at_turn.has_value() || valid_turn_ids.count(item.superseded_at_turn) == 0) {
            failures.push_back(label + " 的 superseded_at_turn 不是合法 turn");
            continue;
        }
        // 覆盖方向:废旧动作必须发生在旧要求成立之后(§四"覆盖方向从旧
        // turn 指向新 turn")——倒序覆盖整份拒收。
        if (old_latest.has_value() && *at_turn <= *old_latest) {
            failures.push_back(label + " 的覆盖方向倒序: superseded_at_turn " + item.superseded_at_turn +
                               " 不晚于旧要求的最晚来源");
            continue;
        }
        // 替代项必须存在,且不得比旧要求更老。
        const auto replacement = requirement_latest_turn.find(item.superseded_by);
        if (replacement == requirement_latest_turn.end()) {
            // 也允许指向另一条 superseded 项(链式覆盖)。
            bool chained = false;
            for (const auto& other : contract.superseded_requirements) {
                if (other.id == item.superseded_by) {
                    chained = true;
                    break;
                }
            }
            if (!chained) {
                failures.push_back(label + " 的 superseded_by 指向不存在的 requirement: " + item.superseded_by);
                continue;
            }
        } else if (old_latest.has_value() && replacement->second <= *old_latest) {
            failures.push_back(label + " 的替代项不比旧要求新(倒序覆盖)");
            continue;
        }
    }

    // 覆盖图无环:沿 superseded_by 在 superseded 列表里走,一步一记,走到
    // 已访问过的 id 即成环(指向活动 requirement 的链自然到头)。
    for (const auto& item : contract.superseded_requirements) {
        std::set<std::string> visited{item.id};
        std::string next = item.superseded_by;
        bool cycle = false;
        for (std::size_t steps = 0; steps <= contract.superseded_requirements.size(); ++steps) {
            const auto it = std::find_if(contract.superseded_requirements.begin(),
                                         contract.superseded_requirements.end(),
                                         [&next](const SupersededRequirement& candidate) {
                                             return candidate.id == next;
                                         });
            if (it == contract.superseded_requirements.end()) {
                break;  // 指向活动 requirement,链到头
            }
            if (!visited.insert(next).second) {
                cycle = true;
                break;
            }
            next = it->superseded_by;
        }
        if (cycle) {
            failures.push_back("覆盖关系成环(从 " + item.id + " 起)");
        }
    }

    for (const auto& question : contract.open_questions) {
        if (TrimWhitespace(question.text).empty()) {
            failures.push_back("open_questions 有空条目");
        } else if (question.source_turns.empty() ||
                   !AllTurnIdsValid(question.source_turns, valid_turn_ids)) {
            failures.push_back("待澄清问题「" + question.text.substr(0, 24) + "」缺少合法的来源 turn");
        }
    }
    return failures;
}

std::vector<std::string> ValidateWorkState(const WorkState& state,
                                           const std::set<std::string>& valid_turn_ids,
                                           const std::set<std::string>& valid_event_ids,
                                           const std::vector<std::string>& required_open_items) {
    std::vector<std::string> failures;
    const auto check_refs = [&](const std::vector<std::string>& refs, const std::string& label) {
        if (refs.empty()) {
            failures.push_back(label + " 没带任何 evidence_refs");
            return;
        }
        for (const auto& ref : refs) {
            // ref 形如 "t4" 或 "t4:e3":turn 部分必须存在;事件部分(若有)
            // 也必须在事件账里。
            const std::size_t colon = ref.find(':');
            const std::string turn_part = colon == std::string::npos ? ref : ref.substr(0, colon);
            if (valid_turn_ids.count(turn_part) == 0) {
                failures.push_back(label + " 的证据 turn 不存在: " + ref);
                continue;
            }
            if (colon != std::string::npos) {
                const std::string event_part = ref.substr(colon + 1);
                if (!valid_event_ids.empty() && valid_event_ids.count(event_part) == 0) {
                    failures.push_back(label + " 的证据事件不存在: " + ref);
                }
            }
        }
    };
    for (const auto& note : state.confirmed_facts) {
        check_refs(note.evidence_refs, "已证实事实「" + note.text.substr(0, 24) + "」");
    }
    for (const auto& note : state.tool_results) {
        check_refs(note.evidence_refs, "工具结果(" + note.tool + ")");
    }
    for (const auto& note : state.changes_made) {
        check_refs(note.evidence_refs, "已做修改「" + note.text.substr(0, 24) + "」");
    }
    for (const auto& note : state.failed_attempts) {
        check_refs(note.evidence_refs, "失败尝试「" + note.text.substr(0, 24) + "」");
    }
    // 活动待办逐字守恒(与 manifest 守恒同一把尺:空白归一后在 open_items
    // 里逐字在场)。
    for (const auto& required : required_open_items) {
        const std::string needle = NormalizeForCompare(required);
        if (needle.empty()) {
            continue;
        }
        bool found = false;
        for (const auto& item : state.open_items) {
            if (NormalizeForCompare(item).find(needle) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) {
            failures.push_back("工作状态丢了未完成事项: " + required);
        }
    }
    if (TrimWhitespace(state.next_action).empty()) {
        failures.push_back("工作状态缺下一步(next_action)");
    }
    return failures;
}

namespace {

// ---------------------------------------------------------------------------
// 双账压缩的指令与材料拼装
// ---------------------------------------------------------------------------

// map 指令:只收严格 JSON 的 TurnGroupSummary,turn/事件范围宿主已钉。
std::string BuildTurnGroupMapInstruction(const std::string& turn_range, const std::string& evidence_range,
                                         const CompactOptions& options) {
    std::string instruction =
        "以上是对话历史中的一段(来源 turn " + turn_range + ",来源事件 " +
        (evidence_range.empty() ? std::string("无") : evidence_range) +
        ")。请把这段收成一份局部小结。只输出一枚 JSON 对象——不要 Markdown、不要围栏、"
        "不要任何解释文字,键名与结构逐字照写:\n"
        "{\"user_requirement_changes\": [\"本段里用户提出/修改/撤销的要求\"], "
        "\"confirmed_facts\": [\"用户确认或工具证实的事实\"], "
        "\"tool_results\": [{\"tool\": \"工具名\", \"result\": \"关键结果(错误码/退出码/测试结论;不许只写处理过)\", "
        "\"evidence\": \"证据入口,如 t6:e2\"}], "
        "\"files\": [\"涉及文件路径\"], \"changes_made\": [\"已做修改\"], "
        "\"failed_attempts\": [\"失败尝试,保住失败原因与后来改用的路\"], "
        "\"open_items\": [\"本段结束时仍未完成的事\"], "
        "\"next_step_candidates\": [\"下一步候选\"]}\n"
        "每栏没有内容就给空数组;所有元素必须是非空字符串;只写这段里确证过的内容,不许猜补。"
        "user_requirement_changes 只收真正的用户输入说了什么,assistant 的猜测不算用户要求。";
    if (!options.focus.empty()) {
        instruction += "\n重点关注: " + options.focus;
    }
    return instruction;
}

// reduce 指令:产出 {"user_contract":...,"work_state":...} 严格双账 JSON。
std::string BuildDualLedgerReduceInstruction(std::size_t summary_count, bool has_prior, bool has_hot,
                                             const CompactOptions& options) {
    std::string instruction =
        "以上是一份任务的压缩材料:上一轮总账(若有)、" +
        std::to_string(summary_count) + " 份局部小结(各带来源 turn 与事件范围),以及最近热区原文(若有)。"
        "请把它们归并成两份总账。只输出一枚 JSON 对象——不要 Markdown、不要围栏、不要解释文字,"
        "键名与结构逐字照写:\n"
        "{\"user_contract\": {\"goal\": {\"text\": \"当前任务目标\", \"source_turns\": [\"t1\"]}, "
        "\"active_constraints\": [{\"id\": \"r1\", \"text\": \"当前有效约束\", \"source_turns\": [\"t3\"]}], "
        "\"acceptance_criteria\": [{\"id\": \"r2\", \"text\": \"验收条件\", \"source_turns\": [\"t7\"]}], "
        "\"additions\": [{\"id\": \"r3\", \"text\": \"用户后来补充的要求\", \"source_turns\": [\"t8\"]}], "
        "\"superseded_requirements\": [{\"id\": \"r0\", \"text\": \"已被废掉的旧要求\", \"source_turns\": [\"t2\"], "
        "\"superseded_by\": \"替代它的新要求 id\", \"superseded_at_turn\": \"覆盖发生在哪枚 turn\"}], "
        "\"open_questions\": [{\"text\": \"尚待澄清\", \"source_turns\": [\"t9\"]}]}, "
        "\"work_state\": {\"confirmed_facts\": [{\"text\": \"用户确认或工具证实的事实\", "
        "\"evidence_refs\": [\"t4:e3\"]}], "
        "\"tool_results\": [{\"tool\": \"ctest\", \"result\": \"关键结果\", \"evidence_refs\": [\"t6:e2\"]}], "
        "\"files\": [\"src/x.cpp\"], "
        "\"changes_made\": [{\"text\": \"已做修改\", \"evidence_refs\": [\"t10\"]}], "
        "\"failed_attempts\": [{\"text\": \"失败尝试\", \"evidence_refs\": [\"t11:e4\"]}], "
        "\"open_items\": [\"未完成事项\"], \"next_action\": \"下一步要执行的准确动作\"}}\n"
        "规矩(违反任一条整份会被拒收,旧历史不动):\n"
        "1. user_contract 只从真正的用户输入提取;assistant 与 tool_result 永不可成为来源"
        "(source_turns 只能引用材料里标注的 turn 号)。\n"
        "2. 每条 active_constraints/acceptance_criteria/additions 都至少有一枚 source_turns;"
        "id 用 r1、r2…各不相同。\n"
        "3. 后来的用户要求可覆盖早先用户要求:旧要求移进 superseded_requirements,写明 "
        "superseded_by(替代项 id)与 superseded_at_turn(不早于旧要求的来源 turn)。\n"
        "4. 两条要求冲突又找不到明确覆盖关系时,放进 open_questions,不许擅自删一条。\n"
        "5. confirmed_facts 只收用户确认或工具证实的;assistant 推测不算。每条至少一枚 "
        "evidence_refs(\"tN\" 或 \"tN:eM\")。\n"
        "6. 工具错误、退出码、关键路径、测试结果不得只写\"处理过\"。\n"
        "7. open_items 承接全部仍未完成的待办;next_action 是工作接力,不混进 user_contract。";
    if (has_prior) {
        instruction += "\n输入里的上一轮总账只当参考:与局部小结或热区原文冲突时,以后者为准"
                       "(它们来自原始材料);不许拿旧摘要复印新摘要。";
    }
    if (has_hot) {
        instruction += "\n最近热区原文没有总结过:最新一轮可能刚纠正早先要求,总契约必须吸收它——"
                       "被纠正的旧约束移进 superseded_requirements,新约束列 active。";
    }
    if (!options.required_open_items.empty()) {
        instruction += "\n\n以下是当前仍未完成的待办,每一项必须逐字(只许调整空白)出现在 work_state 的 "
                       "open_items 数组里,一项都不许丢、不许改写:\n";
        for (std::size_t i = 0; i < options.required_open_items.size(); ++i) {
            instruction += std::to_string(i + 1) + ". " + options.required_open_items[i] + "\n";
        }
    }
    if (!options.focus.empty()) {
        instruction += "\n重点关注: " + options.focus;
    }
    return instruction;
}

// 热区原文渲染成 reduce 能读的流水(工作视图:长 ToolResult 已是 artifact
// 预览)。turn 头标号,给 reduce 引 source_turns 用。
std::string RenderTranscript(const std::vector<api::Message>& messages,
                             const std::vector<std::pair<std::size_t, std::size_t>>& turn_ranges,
                             const TurnPartitionPlan& plan, std::size_t first_turn) {
    std::string body;
    for (std::size_t t = 0; t < turn_ranges.size(); ++t) {
        const std::string turn_id =
            first_turn + t < plan.turns.size() ? plan.turns[first_turn + t].id : std::string("t?");
        for (std::size_t i = turn_ranges[t].first; i < turn_ranges[t].second; ++i) {
            const std::string role = messages[i].role == api::Role::User ? "用户" : "助手";
            for (const auto& block : messages[i].content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                    body += "--- turn " + turn_id + " · " + role + " ---\n" + text->text + "\n";
                } else if (const auto* use = std::get_if<api::ToolUseBlock>(&block); use != nullptr) {
                    body += "--- turn " + turn_id + " · 助手调用工具 " + use->name + "(" + use->id +
                            ") ---\n" + use->input.dump() + "\n";
                } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                           result != nullptr) {
                    body += "--- turn " + turn_id + " · 工具结果(" + result->tool_use_id + ") ---\n" +
                            result->content + "\n";
                }
            }
        }
    }
    return body;
}

// 消息里的全部 tool_use/tool_result 配对完整性:map 请求的任何一块都不许
// 劈开工具原子组——这里给单测与防御性检查用。
bool ToolPairsCompleteIn(const std::vector<api::Message>& messages) {
    std::map<std::string, bool> use_seen;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block); use != nullptr) {
                use_seen[use->id] = false;
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                       result != nullptr) {
                const auto it = use_seen.find(result->tool_use_id);
                if (it == use_seen.end()) {
                    return false;  // result 比 use 先出现/没有 use
                }
                it->second = true;
            }
        }
    }
    for (const auto& [id, matched] : use_seen) {
        (void)id;
        if (!matched) {
            return false;
        }
    }
    return true;
}

// TurnGroupSummary -> json(reduce 材料与两两归并的输入共用一份拼装)。
nlohmann::json TurnGroupSummaryJson(const TurnGroupSummary& summary) {
    nlohmann::json json;
    json["turn_range"] = summary.turn_range;
    json["evidence_range"] = summary.evidence_range;
    json["user_requirement_changes"] = summary.user_requirement_changes;
    json["confirmed_facts"] = summary.confirmed_facts;
    std::vector<nlohmann::json> tool_results;
    tool_results.reserve(summary.tool_results.size());
    for (const auto& note : summary.tool_results) {
        nlohmann::json item;
        item["tool"] = note.tool;
        item["result"] = note.result;
        if (!note.evidence.empty()) {
            item["evidence"] = note.evidence;
        }
        tool_results.push_back(std::move(item));
    }
    json["tool_results"] = std::move(tool_results);
    json["files"] = summary.files;
    json["changes_made"] = summary.changes_made;
    json["failed_attempts"] = summary.failed_attempts;
    json["open_items"] = summary.open_items;
    json["next_step_candidates"] = summary.next_step_candidates;
    return json;
}

// 范围标签的首/尾 turn id:"t3-t7" -> "t3"/"t7";单枚 "t3" 两头都是它。
std::string RangeFirst(const std::string& range) {
    const std::size_t dash = range.find('-');
    return dash == std::string::npos ? range : range.substr(0, dash);
}

std::string RangeLast(const std::string& range) {
    const std::size_t dash = range.rfind('-');
    return dash == std::string::npos ? range : range.substr(dash + 1);
}

}  // namespace

std::vector<api::Message> BuildCompactedHistory(const std::vector<api::Message>& history,
                                                const api::Message& archive, const TurnPartitionPlan& plan,
                                                std::vector<std::size_t>* kept_indices_out) {
    if (history.empty() || plan.partitions.empty()) {
        if (kept_indices_out != nullptr) {
            kept_indices_out->clear();
        }
        return {archive};
    }
    // 热区 = 末分区的原文消息。分区盖住全部 turn,热区必是到尾的连续段。
    const TurnPartitionInfo& hot = plan.partitions.back();
    const std::size_t hot_from = plan.turns[hot.first_turn].from_message;

    std::string archive_text;
    for (const auto& block : archive.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
            archive_text += text->text;
        }
    }

    std::vector<api::Message> new_history;
    new_history.reserve(history.size() - hot_from);
    std::vector<std::size_t> kept_indices;
    for (std::size_t i = hot_from; i < history.size(); ++i) {
        api::Message message = history[i];
        if (kept_indices.empty()) {
            // 双账并入热区首条 user 消息开头:不单独成一条,相邻两条 user
            // 违反角色交替(与老 BuildCompactedHistory 同一招)。热区首条
            // 是 turn 头,必有 TextBlock(防御:没有就补一条)。
            bool merged = false;
            for (auto& block : message.content) {
                if (auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                    text->text = archive_text + "\n\n" + text->text;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                message.content.insert(message.content.begin(), api::TextBlock{archive_text});
            }
        }
        kept_indices.push_back(i);
        new_history.push_back(std::move(message));
    }
    if (kept_indices_out != nullptr) {
        *kept_indices_out = std::move(kept_indices);
    }
    return new_history;
}

std::optional<PriorLedgers> ParsePriorLedgers(const std::string& prior_archive_text) {
    // 剥出 json 围栏里的对象(BuildTurnPartitionPlan 剥出的 prior 文本一定
    // 带围栏;这里不依赖这一点,裸对象也认)。
    const auto parsed = ParseJsonObjectLoose(prior_archive_text);
    if (parsed.has_value()) {
        if (parsed->contains("user_contract") && parsed->contains("work_state")) {
            const auto contract = ParseUserContract((*parsed)["user_contract"]);
            const auto state = ParseWorkState((*parsed)["work_state"]);
            if (contract.has_value() && state.has_value()) {
                PriorLedgers ledgers;
                ledgers.dual_ledger = true;
                ledgers.contract = std::move(*contract);
                ledgers.state = std::move(*state);
                ledgers.raw_text = prior_archive_text;
                return ledgers;
            }
            return std::nullopt;  // 声称双账却解析不动:不许当基线蒙混
        }
        // 旧 flat manifest:goal/open_items 折成只有 goal 的契约当参考。
        if (parsed->contains("goal") && parsed->contains("open_items")) {
            PriorLedgers ledgers;
            ledgers.contract.goal.text = (*parsed)["goal"].is_string()
                                             ? (*parsed)["goal"].get<std::string>()
                                             : std::string();
            if ((*parsed)["open_items"].is_array()) {
                for (const auto& item : (*parsed)["open_items"]) {
                    if (item.is_string()) {
                        ledgers.state.open_items.push_back(item.get<std::string>());
                    }
                }
            }
            if ((*parsed).contains("next_action") && (*parsed)["next_action"].is_string()) {
                ledgers.state.next_action = (*parsed)["next_action"].get<std::string>();
            }
            ledgers.raw_text = prior_archive_text;
            return ledgers;
        }
    }
    // 纯文本旧档(更老的存档形状):只当参考文本,不折结构。
    if (TrimWhitespace(prior_archive_text).empty()) {
        return std::nullopt;
    }
    PriorLedgers ledgers;
    ledgers.raw_text = prior_archive_text;
    return ledgers;
}

std::expected<DualLedgerCompactResult, api::Error> CompactTurnPartitioned(
    api::Backend& backend, const std::string& model, const std::vector<api::Message>& history,
    const CompactOptions& options, const StructuralCompressionOptions& structural,
    const std::string& reasoning_effort, BackgroundCallAccounting* accounting) {
    DualLedgerCompactResult result;
    // 计时守卫:任何 return 路径都把墙钟记进账(与 CompactHierarchical 同
    // 一口径:duration = 整场压缩的总时)。
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

    if (history.empty()) {
        return std::unexpected(api::Error{api::ErrorKind::Api, "没有对话历史,无需压缩", 0});
    }

    TurnPartitionBudgets budgets;
    budgets.structural = structural;
    budgets.compact_model = options.budget;
    result.plan = BuildTurnPartitionPlan(history, options.partition_count, budgets);

    // 明确拒绝门(§3.4 第 3 条/§9.3):一次模型都不调,账先说清。
    if (result.plan.any_turn_over_map_budget) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api,
            "单枚 turn 超过压缩模型输入预算(估 " +
                std::to_string(result.plan.compact_input_budget.value_or(0)) +
                " tokens):该次 compact 明确拒绝——不截半条用户输入、不拆工具原子组,历史一字未动。",
            0});
    }
    if (!result.plan.WorthCompacting()) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api,
            "没有冷区" + std::string(result.plan.has_prior_archive ? "" : "也没有旧存档") +
                ":压缩榨不出收益,拒绝空跑,历史未动。",
            0});
    }
    if (result.plan.has_incomplete_tool_exchange) {
        // 分区边界只落 turn 之间,工具原子组天然不跨区;不完整组只可能住在
        // 热区尾(mid-turn 安全点),热区保原文,天然不拆。这里不拦——
        // plan.has_incomplete_tool_exchange 带在结果里给调用方展示。
    }

    // ---- 材料准备:map 源(剥旧档)、工作视图、事件账 ----
    // 旧档不进任何 map 块(阻断摘要复印摘要);它只进 final reduce 当基线。
    std::vector<api::Message> map_source = history;
    if (result.plan.has_prior_archive) {
        for (auto& block : map_source[0].content) {
            if (auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                if (auto split = SplitPriorArchive(text->text)) {
                    text->text = split->second.empty() ? std::string("[旧档已剥出,此消息无原始正文]")
                                                       : split->second;
                }
                break;
            }
        }
    }
    // 工作视图一把算(与会话请求同口径:长 ToolResult 已外置成 artifact 预览)。
    StructuralCompressionStats working_stats;
    ResultViewMemo working_memo;
    const std::vector<api::Message> working =
        CompressWorkingView(map_source, structural, working_stats, working_memo, /*store=*/nullptr);
    // 事件账(evidence 范围与 WorkState 证据校验的来源):按原 history 算,
    // id 只随消息结构走,与剥档后的 map_source 逐条对得上。
    const std::vector<NormalizedEvent> ledger = BuildEventLedger(history);
    std::set<std::string> event_ids;
    for (const auto& event : ledger) {
        event_ids.insert(event.id);
    }

    const auto input_budget = CompactInputBudget(options.budget);
    const std::size_t chunk_budget =
        input_budget.has_value() && *input_budget > kTurnPlanPromptOverheadTokens
            ? *input_budget - kTurnPlanPromptOverheadTokens
            : input_budget.value_or(std::numeric_limits<std::size_t>::max());

    // ---- map:冷分区各一次;超预算只递归拆该分区(§3.4 第 2 条) ----
    struct MapChunk {
        std::size_t first_turn;
        std::size_t last_turn;  // [first, last) turn 区间
    };
    std::vector<MapChunk> map_chunks;
    for (std::size_t p = 0; p + 1 < result.plan.partitions.size(); ++p) {
        const TurnPartitionInfo& partition = result.plan.partitions[p];
        const bool over_budget =
            input_budget.has_value() &&
            partition.working_tokens + kTurnPlanPromptOverheadTokens > *input_budget;
        if (!over_budget) {
            map_chunks.push_back({partition.first_turn, partition.last_turn});
            continue;
        }
        // 沿 turn 边界贪心装块:装满一块切一块;单 turn 仍超(理论上前面
        // 的门已拦)给明确拒绝。
        std::size_t begin = partition.first_turn;
        std::size_t used = 0;
        for (std::size_t t = partition.first_turn; t < partition.last_turn; ++t) {
            const std::size_t turn_tokens = result.plan.turns[t].working_tokens;
            if (turn_tokens > chunk_budget) {
                return std::unexpected(api::Error{
                    api::ErrorKind::Api,
                    "turn " + result.plan.turns[t].id + " 超过压缩模型单块输入预算,该次 compact 明确拒绝,历史未动。",
                    0});
            }
            if (used > 0 && used + turn_tokens > chunk_budget) {
                map_chunks.push_back({begin, t});
                begin = t;
                used = 0;
            }
            used += turn_tokens;
        }
        if (begin < partition.last_turn) {
            map_chunks.push_back({begin, partition.last_turn});
        }
    }

    auto turn_range_label = [&plan = result.plan](std::size_t first, std::size_t last) {
        if (first + 1 == last) {
            return plan.turns[first].id;
        }
        return plan.turns[first].id + "-" + plan.turns[last - 1].id;
    };

    std::vector<TurnGroupSummary> summaries;
    summaries.reserve(map_chunks.size());
    for (std::size_t c = 0; c < map_chunks.size(); ++c) {
        const MapChunk& chunk = map_chunks[c];
        const std::size_t from_message = result.plan.turns[chunk.first_turn].from_message;
        const std::size_t to_message = result.plan.turns[chunk.last_turn - 1].to_message;
        std::vector<api::Message> messages(working.begin() + static_cast<std::ptrdiff_t>(from_message),
                                           working.begin() + static_cast<std::ptrdiff_t>(to_message));
        const std::string turn_range = turn_range_label(chunk.first_turn, chunk.last_turn);
        const std::string evidence_range = EventRangeForMessages(ledger, from_message, to_message);
        // 防御:块内工具原子组必须完整(分区/再切都只落 turn 边界,这里
        // 不该拦得到;拦到了就是切分 bug,明确失败好过静默劈开)。
        if (!ToolPairsCompleteIn(messages)) {
            return std::unexpected(api::Error{
                api::ErrorKind::Api,
                "map 块 " + turn_range + " 内工具原子组不完整(切分缺陷),该次 compact 拒绝,历史未动。", 0});
        }
        const auto text =
            RequestSummaryText(backend, model, BuildTurnGroupMapInstruction(turn_range, evidence_range, options),
                               messages, static_cast<int>(options.budget.output_reserve_tokens),
                               reasoning_effort, accounting);
        if (!text.has_value()) {
            return std::unexpected(text.error());  // map 任一块失败,整次失败(§9.5)
        }
        auto summary = ParseTurnGroupSummary(*text);
        if (!summary.has_value()) {
            return std::unexpected(api::Error{
                api::ErrorKind::Api,
                "turn " + turn_range + " 的局部小结不是合法的严格 JSON TurnGroupSummary,该次 compact 失败,历史未动。",
                0});
        }
        // 宿主钉死来源范围:模型写什么都覆盖。
        summary->turn_range = turn_range;
        summary->evidence_range = evidence_range;
        summaries.push_back(std::move(*summary));
    }
    result.group_summaries = summaries;

    // ---- final reduce 输入:prior + summaries + 热区原文 ----
    const TurnPartitionInfo& hot = result.plan.partitions.back();
    const std::size_t hot_from_message = result.plan.turns[hot.first_turn].from_message;
    std::vector<api::Message> hot_working(working.begin() + static_cast<std::ptrdiff_t>(hot_from_message),
                                          working.end());
    std::vector<std::pair<std::size_t, std::size_t>> hot_turn_ranges;
    for (std::size_t t = hot.first_turn; t < hot.last_turn; ++t) {
        hot_turn_ranges.emplace_back(result.plan.turns[t].from_message - hot_from_message,
                                     result.plan.turns[t].to_message - hot_from_message);
    }
    const std::string hot_transcript = RenderTranscript(hot_working, hot_turn_ranges, result.plan, hot.first_turn);
    const bool has_hot = !hot_transcript.empty();

    const auto build_reduce_input = [&]() {
        std::string body;
        if (result.plan.has_prior_archive) {
            body += "=== 上一轮压缩的总账(仅供参考;与新材料冲突以新材料为准) ===\n" +
                    result.plan.prior_archive_text + "\n\n";
        }
        for (std::size_t i = 0; i < summaries.size(); ++i) {
            body += "=== 局部小结 " + std::to_string(i + 1) + "/" + std::to_string(summaries.size()) +
                    " · 来源 turn " + summaries[i].turn_range + " · 事件 " +
                    (summaries[i].evidence_range.empty() ? std::string("无") : summaries[i].evidence_range) +
                    " ===\n" + TurnGroupSummaryJson(summaries[i]).dump() + "\n\n";
        }
        if (has_hot) {
            body += "=== 最近热区原文(未总结;最新用户纠正以此为准) ===\n" + hot_transcript;
        }
        api::Message input;
        input.role = api::Role::User;
        input.content.push_back(api::TextBlock{body});
        return std::vector<api::Message>{input};
    };

    // §9.4:reduce 材料超预算 → 两两归并相邻 summaries(来源范围保住),
    // 压到装得下为止;次数护栏 4 轮。
    const int kMaxReducePasses = 4;
    int passes = 0;
    while (passes < kMaxReducePasses && summaries.size() > 1) {
        const std::vector<api::Message> reduce_input = build_reduce_input();
        const std::size_t reduce_tokens =
            EstimateUtf8Tokens(BuildDualLedgerReduceInstruction(
                                   summaries.size(), result.plan.has_prior_archive, has_hot, options)) +
            EstimateHistoryTokens(reduce_input) + options.budget.protocol_headroom_tokens;
        if (!input_budget.has_value() || reduce_tokens <= *input_budget) {
            break;
        }
        const std::string merge_instruction =
            "以上是同一任务相邻两段的局部小结(JSON)。请把它们归并成一份局部小结,只输出一枚 JSON 对象"
            "(键名结构与输入相同:八栏数组 + tool_results 对象数组),保留两段全部要点(去重,冲突以后段"
            "为准),不许丢 open_items。不要 Markdown、不要围栏。";
        std::vector<TurnGroupSummary> merged;
        for (std::size_t i = 0; i < summaries.size(); i += 2) {
            if (i + 1 >= summaries.size()) {
                merged.push_back(summaries[i]);
                continue;
            }
            api::Message pair_message;
            pair_message.role = api::Role::User;
            pair_message.content.push_back(api::TextBlock{
                "=== 小结 A · 来源 turn " + summaries[i].turn_range + " ===\n" +
                TurnGroupSummaryJson(summaries[i]).dump() + "\n\n=== 小结 B · 来源 turn " +
                summaries[i + 1].turn_range + " ===\n" + TurnGroupSummaryJson(summaries[i + 1]).dump()});
            const auto text = RequestSummaryText(backend, model, merge_instruction,
                                                 std::vector<api::Message>{pair_message},
                                                 static_cast<int>(options.budget.output_reserve_tokens),
                                                 reasoning_effort, accounting);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            auto combined = ParseTurnGroupSummary(*text);
            if (!combined.has_value()) {
                return std::unexpected(api::Error{
                    api::ErrorKind::Api,
                    "归并 turn " + summaries[i].turn_range + "+" + summaries[i + 1].turn_range +
                        " 的局部小结不是合法严格 JSON,该次 compact 失败,历史未动。",
                    0});
            }
            // 宿主钉归并后的范围:两份的并集(首段开头-末段结尾);证据范围
            // 同样取两份的拼接。
            combined->turn_range = RangeFirst(summaries[i].turn_range) + "-" +
                                   RangeLast(summaries[i + 1].turn_range);
            combined->evidence_range = summaries[i].evidence_range + "," + summaries[i + 1].evidence_range;
            merged.push_back(std::move(*combined));
        }
        summaries = std::move(merged);
        ++passes;
    }
    result.group_summaries = summaries;

    // ---- final reduce:严格双账 JSON ----
    const auto final_text =
        RequestSummaryText(backend, model,
                           BuildDualLedgerReduceInstruction(summaries.size(), result.plan.has_prior_archive,
                                                            has_hot, options),
                           build_reduce_input(), static_cast<int>(options.budget.output_reserve_tokens),
                           reasoning_effort, accounting);
    if (!final_text.has_value()) {
        return std::unexpected(final_text.error());
    }
    const auto final_json = ParseJsonObjectLoose(*final_text);
    if (!final_json.has_value() || !final_json->contains("user_contract") ||
        !final_json->contains("work_state")) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api,
            "final reduce 输出不是合法的 {\"user_contract\":...,\"work_state\":...} 严格 JSON,该次 compact 失败,历史未动。",
            0});
    }
    auto contract = ParseUserContract((*final_json)["user_contract"]);
    auto state = ParseWorkState((*final_json)["work_state"]);
    if (!contract.has_value() || !state.has_value()) {
        return std::unexpected(api::Error{
            api::ErrorKind::Api, "双账 schema 解析未过(user_contract/work_state 键型不合),该次 compact 失败,历史未动。",
            0});
    }

    // 宿主校验:来源、覆盖方向、环、证据、待办守恒(§四/§五)。任一失败
    // 整次 compact 失败,旧 history 不动(§9.5)。
    std::set<std::string> valid_turn_ids;
    for (const auto& turn : result.plan.turns) {
        valid_turn_ids.insert(turn.id);
    }
    std::vector<std::string> failures = ValidateUserContract(*contract, valid_turn_ids);
    for (const auto& failure :
         ValidateWorkState(*state, valid_turn_ids, event_ids, options.required_open_items)) {
        failures.push_back(failure);
    }
    if (!failures.empty()) {
        std::string reasons;
        for (std::size_t i = 0; i < failures.size(); ++i) {
            if (i > 0) {
                reasons += ";";
            }
            reasons += failures[i];
        }
        return std::unexpected(api::Error{api::ErrorKind::Api, "双账校验未过,历史未动: " + reasons, 0});
    }

    result.contract = std::move(*contract);
    result.state = std::move(*state);

    // 兼容面:双账折算成旧 manifest(展示/compact_v2 旧读者都不用改)。
    result.manifest.goal = result.contract.goal.text;
    for (const auto& requirement : result.contract.active_constraints) {
        result.manifest.constraints.push_back(requirement.text);
    }
    result.manifest.open_items = result.state.open_items;
    result.manifest.next_action = result.state.next_action;

    // ---- 新 history:[双账][热区原文](§3.5/阶段 4) ----
    // 双账落进单枚 ```json 围栏:SplitPriorArchive(下一次压缩剥旧档)与
    // ParsePriorLedgers(下一次压缩认基线)都不用改形状。围栏安全:json
    // dump 里把反引号换成普通引号,防正文里冒出 ``` 弄破围栏。
    std::string ledger_dump = nlohmann::json{{"user_contract", ToJson(result.contract)},
                                             {"work_state", ToJson(result.state)}}
                                  .dump();
    for (char& c : ledger_dump) {
        if (c == '`') {
            c = '\'';
        }
    }
    api::Message archive;
    archive.role = api::Role::User;
    archive.content.push_back(api::TextBlock{
        platform::SanitizeExternalText("[对话存档,此前内容已压缩] 此前对话已收进两份总账:"
                                       "用户契约(用户要什么)与工作状态(事情做到哪);此后上下文以此为准。\n"
                                       "```json\n" +
                                       ledger_dump + "\n```")});
    result.archive = archive;
    result.new_history =
        BuildCompactedHistory(history, result.archive, result.plan, &result.kept_indices);

    // 指标:map 调用数(再切也全算)、reduce 轮次、分区账。
    result.metrics.chunks = static_cast<int>(map_chunks.size());
    result.metrics.reduce_passes = passes;
    result.metrics.hierarchical = map_chunks.size() > 1;
    result.metrics.implementation = "turn-partition";
    result.metrics.schema = "dual-ledger-v1";
    result.metrics.partition_count = result.plan.partitions.size();
    result.metrics.total_turns = result.plan.turns.size();
    result.metrics.hot_turns = hot.last_turn - hot.first_turn;
    {
        std::string buffer;
        for (const auto& message : history) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                    buffer += text->text;
                } else if (const auto* tool_result = std::get_if<api::ToolResultBlock>(&block);
                           tool_result != nullptr) {
                    buffer += tool_result->content;
                }
            }
        }
        result.metrics.source_digest = Fingerprint64(buffer);
    }
    return result;
}

}  // namespace lubancode::agent
