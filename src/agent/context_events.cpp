#include "agent/context_events.hpp"

#include <map>
#include <utility>
#include <variant>

#include "agent/artifact_store.hpp"
#include "agent/context.hpp"  // IsUserTurnStart:公共 turn 判定(§二 唯一定义)
#include "platform/text_encoding.hpp"  // Utf8*Boundary:预览头尾不劈半个字

namespace lubancode::agent {

namespace {

// (IsUserTurnStart 的私有拷贝已删:判定收拢到 agent/context.hpp 的公共
// IsUserTurnStart——Compact 四分区单阶段 0,§二 的唯一定义,磁盘账与
// 内存路共用同一只。)

// 只读工具的规范键:同键才谈得上"同一件事再看一遍"。副作用工具
// (run_command / web_fetch / web_search / agent / ...)一律给空键——命令
// 文本相同不代表这次能不跑,结构压缩也绝不因此跳执行;这里只是不给它们
// 进判重的门。
std::string BuildDedupKey(const std::string& tool_name, const nlohmann::json& input) {
    const auto field = [&input](const char* name) -> std::string {
        if (const auto it = input.find(name); it != input.end() && it->is_string()) {
            return it->get<std::string>();
        }
        if (const auto it = input.find(name); it != input.end() && it->is_number_integer()) {
            return std::to_string(it->get<long long>());
        }
        return std::string();
    };
    if (tool_name == "read_file") {
        // path + 窗口(offset/limit)定一枚读取;正文 hash 另算。
        return "read|" + field("path") + "|" + field("offset") + "|" + field("limit");
    }
    if (tool_name == "search" || tool_name == "tool_search") {
        return "search|" + tool_name + "|" + field("mode") + "|" + field("pattern") + "|" + field("path") +
               "|" + field("glob") + "|" + field("query");
    }
    return std::string();  // 其余工具(含全部副作用工具)不判重
}

}  // namespace

std::string Fingerprint64(const std::string& content) {
    // FNV-1a 64:指纹只判"完全相同"与做引用锚,不做安全用途。
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : content) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

std::size_t HotZoneStartIndex(const std::vector<api::Message>& history) {
    for (std::size_t i = history.size(); i-- > 0;) {
        if (IsUserTurnStart(history[i])) {
            return i;
        }
    }
    return 0;
}

std::vector<NormalizedEvent> BuildEventLedger(const std::vector<api::Message>& history) {
    std::vector<NormalizedEvent> events;
    events.reserve(history.size());
    int sequence = 0;
    for (std::size_t i = 0; i < history.size(); ++i) {
        const api::Message& message = history[i];
        // 用户/助手文本:一枚事件一枚消息(简化:消息级,不拆块——判重只
        // 关心 ToolExchange,文本事件的粒度够用)。
        if (IsUserTurnStart(message)) {
            NormalizedEvent event;
            event.id = "e" + std::to_string(sequence++);
            event.kind = NormalizedEventKind::UserText;
            event.message_index = i;
            events.push_back(std::move(event));
            continue;
        }
        bool has_tool_use = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                has_tool_use = true;
                const auto& call = std::get<api::ToolUseBlock>(block);
                NormalizedEvent event;
                event.id = "e" + std::to_string(sequence++);
                event.kind = NormalizedEventKind::ToolExchange;
                event.message_index = i;
                event.tool_name = call.name;
                event.tool_use_id = call.id;
                event.tool_input = call.input;
                event.dedup_key = BuildDedupKey(call.name, call.input);
                // 原子配对:tool_result 紧跟在 assistant 消息之后的 user 消息
                // 里(本代码库的固定形状);找不到 = 孤儿,按无结果记账,
                // 不丢消息。错误结果也算配上(is_error 的空正文是真实答案)。
                bool paired = false;
                for (std::size_t j = i + 1; j < history.size() && j <= i + 1 && !paired; ++j) {
                    if (history[j].role != api::Role::User) {
                        break;
                    }
                    for (const auto& result_block : history[j].content) {
                        if (!std::holds_alternative<api::ToolResultBlock>(result_block)) {
                            continue;
                        }
                        const auto& result = std::get<api::ToolResultBlock>(result_block);
                        if (result.tool_use_id == call.id) {
                            event.result_content = result.content;
                            event.result_is_error = result.is_error;
                            event.result_message_index = j;
                            paired = true;
                            break;
                        }
                    }
                }
                event.content_hash = Fingerprint64(event.result_content);
                events.push_back(std::move(event));
            }
        }
        if (!has_tool_use && message.role == api::Role::Assistant) {
            NormalizedEvent event;
            event.id = "e" + std::to_string(sequence++);
            event.kind = NormalizedEventKind::AssistantText;
            event.message_index = i;
            events.push_back(std::move(event));
        }
    }
    return events;
}

// 按首次定形的决策渲染一枚结果的视图正文。
std::string RenderResultView(const NormalizedEvent& event, const ResultViewDecision& decision,
                             const StructuralCompressionOptions& options) {
    switch (decision.kind) {
        case ResultViewKind::Full:
            return event.result_content;
        case ResultViewKind::Artifact: {
            // 预览头尾都按码点边界对齐:preview_bytes 是字节数,裸 substr 砍进
            // 多字节序列的腰上,拼出来的视图就是非法 UTF-8,而这个视图会顶替
            // tool_result 进请求——dump 当场 type_error.316。
            const std::size_t head =
                platform::Utf8PrefixBoundary(event.result_content,
                                             options.preview_bytes < event.result_content.size()
                                                 ? options.preview_bytes
                                                 : event.result_content.size());
            const std::size_t tail_begin =
                platform::Utf8SuffixBoundary(event.result_content,
                                             event.result_content.size() > options.preview_bytes
                                                 ? event.result_content.size() - options.preview_bytes
                                                 : 0);
            // 第二期:落盘成功的 artifact 视图带稳定 id 与两把只读钥匙的
            // 指引——模型凭 id 能搜全文、能分块读,不再只是"存档里有"的
            // 一句空话(规格"给模型两把只读钥匙")。
            if (!decision.artifact_id.empty()) {
                return "[artifact " + decision.artifact_id + " · " + event.id + " · " + event.tool_name +
                       " · " + std::to_string(event.result_content.size()) + " 字节 · sha256=" +
                       decision.artifact_sha + " · 头部预览:\n" + event.result_content.substr(0, head) +
                       "\n…尾部预览:\n" + event.result_content.substr(tail_begin) +
                       "\n全文已落盘。预览不足时:先用 context_search(artifact_id=\"" + decision.artifact_id +
                       "\", query=...) 搜命中行,再用 context_read 按 chunk_id 或行窗读取;"
                       "省略号不是全文]";
            }
            return "[artifact " + event.id + " · sha=" + event.content_hash + " · " +
                   std::to_string(event.result_content.size()) + " 字节 · 头部:\n" +
                   event.result_content.substr(0, head) + "\n…尾部:\n" + event.result_content.substr(tail_begin) +
                   "\n全文在会话存档,可用 /export 查看]";
        }
        case ResultViewKind::DuplicateRef:
            return "[已收敛:与事件 " + decision.ref_event_id + " 的结果完全相同(" + event.tool_name +
                   "),累计出现 " + std::to_string(decision.seen_count) + " 次;全文在会话存档]";
        case ResultViewKind::NewVersion: {
            // 新版本自述"替代 eN",原文照发(超长再折 artifact);绝不回头
            // 给 eN 补 superseded——那枚事件的表示在它首次进视图时已定形。
            if (event.result_content.size() > options.long_result_bytes) {
                ResultViewDecision artifact;
                artifact.kind = ResultViewKind::Artifact;
                return "[此读取替代事件 " + decision.ref_event_id + " 的旧版本]\n" +
                       RenderResultView(event, artifact, options);
            }
            return "[此读取替代事件 " + decision.ref_event_id + " 的旧版本]\n" + event.result_content;
        }
    }
    return event.result_content;
}

std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats) {
    ResultViewMemo fresh_memo;
    return CompressWorkingView(history, options, stats, fresh_memo);
}

std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats, ResultViewMemo& memo) {
    return CompressWorkingView(history, options, stats, memo, /*store=*/nullptr);
}

std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats, ResultViewMemo& memo,
                                              ContextArtifactStore* store) {
    stats = StructuralCompressionStats{};
    const std::vector<NormalizedEvent> ledger = BuildEventLedger(history);
    stats.events_total = ledger.size();

    // 键的活账(判定"同键第几遍/新版本"用,原文 hash 口径,不受 memo 影响):
    //   key_states[key] = {kept_event_id, kept_hash, seen_count}
    //   kept 是同键头一份(它首次进视图时已定形,后来者只自述,不改它)。
    struct KeyState {
        std::string kept_event_id;
        std::string kept_hash;
        std::size_t seen_count = 1;
    };
    std::map<std::string, KeyState> key_states;

    // 渲染账:tool_use_id -> 视图正文。memo 命中的旧决策直接照抄;新事件
    // 现场定形并写进 memo——这份决策在本 epoch 内从此钉死。
    std::map<std::string, std::string> rendered;

    for (const auto& event : ledger) {
        if (event.result_message_index == static_cast<std::size_t>(-1)) {
            continue;  // 没配上结果的孤儿 tool_use,不动
        }
        // memo 命中:首次定形过的决策原样重放,stats 不再计——"追改"这条路
        // 从根上堵死。
        if (auto pinned = memo.decisions.find(event.tool_use_id); pinned != memo.decisions.end()) {
            rendered[event.tool_use_id] = RenderResultView(event, pinned->second, options);
            // 键账照记(后面的事件要靠它判断自己是第几遍/新版本)。
            if (!event.dedup_key.empty()) {
                auto& state = key_states[event.dedup_key];
                if (state.kept_event_id.empty()) {
                    state.kept_event_id = event.id;
                    state.kept_hash = event.content_hash;
                } else if (event.content_hash == state.kept_hash) {
                    state.seen_count += 1;
                } else {
                    // 新版本成了该键的最新事实:后来的同 hash 重复指它。
                    state.kept_event_id = event.id;
                    state.kept_hash = event.content_hash;
                    state.seen_count = 1;
                }
            }
            continue;
        }

        // 首次定形。副作用工具(空键)永远全文——判重不给它们开门。
        ResultViewDecision decision;
        if (event.dedup_key.empty()) {
            decision.kind = ResultViewKind::Full;
        } else {
            auto& state = key_states[event.dedup_key];
            if (state.kept_event_id.empty()) {
                // 同键头一份:短则全文钉死,超长首次即 artifact——要么首次
                // 就预览,要么这个 epoch 一直全文,不能半路变脸。
                decision.kind = event.result_content.size() > options.long_result_bytes
                                    ? ResultViewKind::Artifact
                                    : ResultViewKind::Full;
                state.kept_event_id = event.id;
                state.kept_hash = event.content_hash;
            } else if (event.content_hash == state.kept_hash) {
                // 精确重复:后来者自述指回 kept 那枚;太短不值得换标注。
                state.seen_count += 1;
                decision.kind = event.result_content.size() >= options.min_compressible_bytes
                                    ? ResultViewKind::DuplicateRef
                                    : ResultViewKind::Full;
                decision.ref_event_id = state.kept_event_id;
                decision.seen_count = state.seen_count;
            } else {
                // 文件改版:新版本原文照发(自述替代旧事件),键账换新事实。
                decision.kind = event.result_content.size() >= options.min_compressible_bytes
                                    ? ResultViewKind::NewVersion
                                    : ResultViewKind::Full;
                decision.ref_event_id = state.kept_event_id;
                state.kept_event_id = event.id;
                state.kept_hash = event.content_hash;
                state.seen_count = 1;
            }
        }

        // 第二期:判成 Artifact 的先落盘(原子次序与幂等见 artifact_store)。
        // 落盘失败(仓没开/磁盘错/任一步没成)决策退回 Full——内存全文照旧
        // 发送,磁盘失败绝不把全文换成空引用(规格"原文不丢")。
        if (decision.kind == ResultViewKind::Artifact && store != nullptr) {
            if (const auto offloaded =
                    store->Offload(event.tool_use_id, event.tool_name, event.result_content,
                                   event.result_message_index);
                offloaded.has_value()) {
                decision.artifact_id = offloaded->artifact_id;
                decision.artifact_sha = offloaded->sha256.substr(0, 12);
            } else {
                decision.kind = ResultViewKind::Full;
            }
        }

        // stats:只记本次新做的决策(不含 memo 命中的旧账)。
        const std::string view_text = RenderResultView(event, decision, options);
        switch (decision.kind) {
            case ResultViewKind::DuplicateRef:
                stats.duplicate_groups += 1;
                stats.duplicate_saved_bytes += event.result_content.size() > view_text.size()
                                                   ? event.result_content.size() - view_text.size()
                                                   : 0;
                break;
            case ResultViewKind::NewVersion:
                if (decision.ref_event_id != event.id) {
                    stats.superseded_observations += 1;
                }
                break;
            case ResultViewKind::Artifact:
                stats.offloaded_results += 1;
                stats.offloaded_saved_bytes += event.result_content.size() > view_text.size()
                                                   ? event.result_content.size() - view_text.size()
                                                   : 0;
                break;
            case ResultViewKind::Full:
                break;
        }

        memo.decisions[event.tool_use_id] = decision;
        rendered[event.tool_use_id] = view_text;
    }

    // 落视图:只重写命中的 tool_result.content,消息条数与块序不动。
    // MCP 富结果单:富块在身的结果换法——文本压成视图短句(一枚
    // TextContent 承载),图片/音频/资源引用与 structuredContent 原样
    // 留在 blocks 里(裁文本不删图、不删结构化结果);content 按新真账
    // 重算投影。纯文本结果照旧只动 content 字符串。
    std::vector<api::Message> view = history;
    for (auto& message : view) {
        for (auto& block : message.content) {
            if (!std::holds_alternative<api::ToolResultBlock>(block)) {
                continue;
            }
            auto& result = std::get<api::ToolResultBlock>(block);
            if (auto it = rendered.find(result.tool_use_id); it != rendered.end()) {
                if (result.blocks.empty()) {
                    result.content = it->second;
                    continue;
                }
                std::vector<tools::ToolContentBlock> kept;
                kept.push_back(tools::TextContent{it->second});
                for (auto& rich : result.blocks) {
                    if (std::holds_alternative<tools::TextContent>(rich)) {
                        continue;  // 文本已并进视图短句
                    }
                    kept.push_back(rich);
                }
                tools::ToolResultPayload payload;
                payload.content = std::move(kept);
                payload.structured_content = result.structured_content;
                result.blocks = payload.content;
                result.content = tools::TextProjection(payload);
            }
        }
    }
    return view;
}

}  // namespace lubancode::agent
