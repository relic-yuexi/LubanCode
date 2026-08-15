#include "agent/context_events.hpp"

#include <map>
#include <utility>
#include <variant>

namespace lubancode::agent {

namespace {

// 一条"真正的用户输入"消息:role 是 User,且内容里带着 TextBlock 或
// ImageBlock——区别于同样顶着 User 角色、内容全是 ToolResultBlock 的那种
// "把工具结果喂回去"的中间消息。(agent/context.cpp 同名私有函数的又一
// 份拷贝;三处各自独立演化,语义钉死在这一条注释里。)
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

std::vector<api::Message> CompressWorkingView(const std::vector<api::Message>& history,
                                              const StructuralCompressionOptions& options,
                                              StructuralCompressionStats& stats) {
    stats = StructuralCompressionStats{};
    const std::vector<NormalizedEvent> ledger = BuildEventLedger(history);
    stats.events_total = ledger.size();

    const std::size_t hot_start = options.protect_hot_zone ? HotZoneStartIndex(history)
                                                                    : history.size();

    // 决策账(只对冷区、只对带键的只读工具):
    //   dedup_state[key] = {first_kept_id, hash, count}——同键同 hash 只留
    //   第一份正文(它必须在冷区;热区本来就不动),后来者换引用。
    //   版本账:同键不同 hash = 文件改版,旧版(冷区)标 superseded。
    struct KeyState {
        std::string kept_event_id;   // 保留正文的那枚事件
        std::string kept_hash;       // 它的内容 hash
        std::size_t seen_count = 1;  // 同键同 hash 累计出现次数
        std::string latest_event_id; // 同键最新一枚事件的 id(不论 hash)
        std::string latest_hash;
    };
    std::map<std::string, KeyState> key_states;

    // 重写计划:message_index -> tool_use_id -> 替换正文。
    std::map<std::pair<std::size_t, std::string>, std::string> rewrites;

    for (const auto& event : ledger) {
        // 键的记账不管冷热都做(热区出现也计入"看过几遍"),但只对冷区落
        // 重写。
        const bool cold = event.message_index < hot_start;
        if (!event.dedup_key.empty()) {
            auto& state = key_states[event.dedup_key];
            if (state.kept_event_id.empty()) {
                state.kept_event_id = event.id;
                state.kept_hash = event.content_hash;
                state.latest_event_id = event.id;
                state.latest_hash = event.content_hash;
                continue;
            }
            state.latest_event_id = event.id;
            state.latest_hash = event.content_hash;
            if (event.content_hash == state.kept_hash) {
                state.seen_count += 1;
                if (cold && event.result_content.size() >= options.min_compressible_bytes &&
                    event.result_message_index != static_cast<std::size_t>(-1)) {
                    const std::string stub = "[已收敛:与事件 " + state.kept_event_id + " 的结果完全相同(" +
                                             event.tool_name + "),累计出现 " + std::to_string(state.seen_count) +
                                             " 次;全文在会话存档]";
                    stats.duplicate_groups += 1;
                    stats.duplicate_saved_bytes += event.result_content.size() - stub.size();
                    rewrites[{event.result_message_index, event.tool_use_id}] = stub;
                }
            } else {
                // 同键不同 hash:文件改版,这是新版本的一枚读取。它自己保
                // 正文(新版本是当下事实;超长的走第二遍的 artifact 外置)。
                // 旧版本的 superseded 标记也在第二遍统一落——那里能反查
                // "后面存在同键不同 hash"。
                state.kept_event_id = event.id;
                state.kept_hash = event.content_hash;
                state.seen_count = 1;
            }
        }
    }

    // 第二遍:supersession 与 artifact 外置。supersession 需要"同键的后一
    // 枚不同 hash 事件"的存在;重扫一遍账,把每个"后面存在同键不同 hash"
    // 的冷区事件标掉。
    for (std::size_t idx = 0; idx < ledger.size(); ++idx) {
        const NormalizedEvent& event = ledger[idx];
        if (event.dedup_key.empty() || event.message_index >= hot_start ||
            event.result_message_index == static_cast<std::size_t>(-1)) {
            continue;
        }
        if (event.result_content.size() < options.min_compressible_bytes) {
            continue;
        }
        if (rewrites.count({event.message_index, event.tool_use_id}) != 0) {
            continue;  // 已经是精确重复的引用,不再叠加
        }
        bool superseded = false;
        std::string newer_id;
        for (std::size_t j = idx + 1; j < ledger.size(); ++j) {
            if (ledger[j].dedup_key == event.dedup_key && ledger[j].content_hash != event.content_hash) {
                superseded = true;
                newer_id = ledger[j].id;
                break;
            }
        }
        const std::size_t head = options.preview_bytes < event.result_content.size() ? options.preview_bytes
                                                                                     : event.result_content.size();
        if (superseded) {
            const std::string stub = event.result_content.substr(0, head) +
                                     "\n[已收敛:此版本其后已改版,最新读取见事件 " + newer_id +
                                     ";头部预览止于此,全文在会话存档]";
            stats.superseded_observations += 1;
            stats.superseded_saved_bytes += event.result_content.size() - stub.size();
            rewrites[{event.result_message_index, event.tool_use_id}] = stub;
            continue;
        }
        if (event.result_content.size() > options.long_result_bytes) {
            const std::string tail_begin = event.result_content.substr(
                event.result_content.size() > options.preview_bytes
                    ? event.result_content.size() - options.preview_bytes
                    : 0);
            const std::string stub = "[artifact " + event.id + " · sha=" + event.content_hash + " · " +
                                     std::to_string(event.result_content.size()) +
                                     " 字节 · 头部:\n" + event.result_content.substr(0, head) +
                                     "\n…尾部:\n" + tail_begin + "\n全文在会话存档,可用 /export 查看]";
            stats.offloaded_results += 1;
            stats.offloaded_saved_bytes += event.result_content.size() - stub.size();
            rewrites[{event.result_message_index, event.tool_use_id}] = stub;
        }
    }

    // 落视图:只重写命中的 tool_result.content,消息条数与块序不动。
    std::vector<api::Message> view = history;
    for (const auto& [location, stub] : rewrites) {
        const auto [message_index, tool_use_id] = location;
        if (message_index >= view.size()) {
            continue;
        }
        for (auto& block : view[message_index].content) {
            if (std::holds_alternative<api::ToolResultBlock>(block)) {
                auto& result = std::get<api::ToolResultBlock>(block);
                if (result.tool_use_id == tool_use_id) {
                    result.content = stub;
                }
            }
        }
    }
    return view;
}

}  // namespace lubancode::agent
