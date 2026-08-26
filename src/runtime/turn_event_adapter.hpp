// TurnEventAdapter(显示系统剥离单第五步后半;骨架拆解批二余款升唯一
// 出水口:Callbacks 老路拔除,引擎经 agent::TurnWiring::events 直连这只,
// 回调装配的 MakeCallbacks/ComposeDisplayCallbacks 随老路退役)。
//
// 一轮 Run() 里模型给的正文/思考/工具起止/usage/step 与批次边界,逐枚翻
// 成带 seq/thread_id/turn_id/item_id 的 ServerEvent,交给 sink。终端(画
// 屏的那只 sink 住装配层,如 app::TerminalTurnSink)、app-server(桥式
// sink)、Web/Tauri 都从同一份事件流里取自己那份——"内核只吐结构化事件"
// 的单子边界,这只就是出水口。
//
// 两条水路:
//   - 主路(OnXxx 口):引擎直接调;条目状态机(正文/思考懒起条、工具
//     开账/销账)在这只上滚。
//   - 从路(ForwardFromSubordinate):子代理/嵌套回合的事件原样并入本流,
//     payload 标 subordinate=true——画屏侧(终端 sink)跳过,账面侧
//     (会话事件链)照收;不经条目状态机,嵌套条目各带各的 item_id。
//
// id 规矩(event.hpp 文件头):thread_id 会话级、turn_id 一轮、item_id
// 一条;全部从 IdAuthority 发,seq thread 内单调。正文/思考各占一枚
// item(多条 ItemDelta 并入同一枚,首次 delta 开 ItemStarted,收不到
// 显式边界就在下一枚 item 开始时收上一枚)。
//
// 依赖:合同头 + api/tools 的领域形状,零 cli/app 依赖,也不再倒挂
// agent/loop(那头的 TurnWiring 持本类的指针,方向是 agent -> runtime)。

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "tools/tool.hpp"

namespace lubancode::runtime {

class TurnEventAdapter {
public:
    // sink:事件落点(实现自管线程安全,见 event_sink.hpp 合同)。
    // thread_id:哪场会话;turn_id 不带 id_authority 的可让 Runtime 发
    // (Start 里现发一枚)。
    TurnEventAdapter(std::string thread_id, IdAuthority& ids)
        : thread_id_(std::move(thread_id)), ids_(ids) {}

    TurnEventAdapter(const TurnEventAdapter&) = delete;
    TurnEventAdapter& operator=(const TurnEventAdapter&) = delete;
    // 移动允许:MakeTurnAdapter 按值造好带回来(命名返回 NRVO 不是保证,
    // 拷贝禁着,移动是唯一路)。移动后的旧壳不许再用——闭包里的 this 已
    // 指向新家,旧壳只待析构。
    TurnEventAdapter(TurnEventAdapter&&) = default;
    TurnEventAdapter& operator=(TurnEventAdapter&&) = default;

    // 挂事件落点(可换;每次开轮之前挂好)。
    void Attach(std::function<void(const ServerEvent&)> sink) { sink_ = std::move(sink); }

    // 在既有落点旁边再挂一只(批二余款:终端渲染改吃事件流——SessionRuntime
    // 造的适配器已带着会话事件链,画屏的 sink 从这里补挂,两条吃同一份流,
    // 次序 = 原有在前、后挂在后)。
    void AttachAlongside(std::function<void(const ServerEvent&)> sink) {
        if (!sink_) {
            sink_ = std::move(sink);
            return;
        }
        std::function<void(const ServerEvent&)> front = std::move(sink_);
        sink_ = [front = std::move(front), back = std::move(sink)](const ServerEvent& event) {
            front(event);
            back(event);
        };
    }

    // 一轮开始:发 turn_id、TurnStarted;返回这轮的 turn_id(调用方对账)。
    // 每轮调一次;上一轮没收尾的条目在这里统一按 Cancelled 收口(打断/
    // 异常路径不会再有自然终态)。turn_id 可显式给(宿主已为这轮发过号的,
    // 比如 trace 口径同一枚,不另 mint 一枚对不上账);空 = 现发。
    std::string Start(const std::string& turn_id = std::string()) {
        turn_id_ = !turn_id.empty() ? turn_id : ids_.NextTurnId();
        text_item_id_.clear();
        thinking_item_id_.clear();
        open_tools_.clear();
        // 复用同一只适配器开第二轮(Stop 钩子续跑、workflow 节点连轴):上一轮
        // 的收口旗翻回去,新一轮的 TurnCompleted 才发得出来。
        turn_finished_ = false;
        Emit(MakeEvent(ServerEventKind::TurnStarted));
        return turn_id_;
    }

    // 本轮 turn_id(Start 之前为空)。
    const std::string& turn_id() const { return turn_id_; }
    // 会话 id 与发号局(从路适配器对账用:同一场会话、同一本号,seq 才单
    // 调得起来)。
    const std::string& thread_id() const { return thread_id_; }
    IdAuthority& ids() const { return ids_; }

    // ------------------------------------------------------------------
    // 主路出水口(引擎直连;subordinate=true 时事件带从路标记——画屏侧
    // 跳过,账面侧照收。PTC 的 stub 调用走这条:16 枚同构调用逐枚有账,
    // 终端照旧只画一张外层卡)。
    // ------------------------------------------------------------------

    void OnTextDelta(const std::string& text, bool subordinate = false) {
        CloseThinking();
        if (text_item_id_.empty()) {
            text_item_id_ = StartItem(ItemKind::Text, std::string());
        }
        ServerEvent event = MakeEvent(ServerEventKind::ItemDelta, text_item_id_, ItemKind::Text);
        event.text = text;
        MarkSubordinate(event, subordinate);
        Emit(std::move(event));
    }

    void OnThinkingDelta(const std::string& text, bool subordinate = false) {
        CloseText();
        if (thinking_item_id_.empty()) {
            thinking_item_id_ = StartItem(ItemKind::Thinking, std::string());
        }
        ServerEvent event = MakeEvent(ServerEventKind::ItemDelta, thinking_item_id_, ItemKind::Thinking);
        event.text = text;
        MarkSubordinate(event, subordinate);
        Emit(std::move(event));
    }

    void OnToolStart(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                     bool subordinate = false) {
        CloseText();
        CloseThinking();
        const std::string item_id = StartItem(ItemKind::Tool, name, tool_use_id, input, /*builtin=*/false, subordinate);
        open_tools_.emplace(tool_use_id, item_id);
    }

    void OnToolDone(const std::string& tool_use_id, const std::string& name, const tools::Tool::Result& result,
                    bool subordinate = false) {
        (void)name;
        const std::string item_id = TakeToolItem(tool_use_id);
        if (item_id.empty()) {
            return;  // 迟到/陌生终态:丢弃不误伤(与 ToolDisplay 同规矩)
        }
        ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, item_id, ItemKind::Tool);
        event.outcome = result.is_error ? Outcome::Failed : Outcome::Succeeded;
        event.payload = nlohmann::json{{"result", result.content}, {"is_error", result.is_error}};
        MarkSubordinate(event, subordinate);
        Emit(std::move(event));
    }

    // 服务端内置工具(Responses 的 web_search_call 一类):与本地工具同一
    // 种条目,payload 加 builtin 标——画屏侧凭它走"只画一张卡、不走视图账"
    // 的老规矩(与拔除前的 on_builtin_tool_* 闭包逐字节同画面)。
    void OnBuiltinToolStart(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                            bool subordinate = false) {
        CloseText();
        CloseThinking();
        const std::string item_id = StartItem(ItemKind::Tool, name, tool_use_id, input, /*builtin=*/true, subordinate);
        open_tools_.emplace(tool_use_id, item_id);
    }

    void OnBuiltinToolDone(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                           const std::string& summary, bool is_error, bool subordinate = false) {
        (void)name;
        const std::string item_id = TakeToolItem(tool_use_id);
        if (item_id.empty()) {
            return;
        }
        ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, item_id, ItemKind::Tool);
        event.outcome = is_error ? Outcome::Failed : Outcome::Succeeded;
        event.payload = nlohmann::json{{"result", summary}, {"is_error", is_error}, {"builtin", true}};
        // 终态参数原样带出:Responses 兼容端到 done 才补全 query,画屏侧要
        // 拿它回填条目标题(老路 on_builtin_tool_done 直传 e.input)。
        if (!input.is_null() && !input.empty()) {
            event.payload["input"] = input;
        }
        MarkSubordinate(event, subordinate);
        Emit(std::move(event));
    }

    // usage:身份齐着带(step 号/请求 id/缓存 epoch/追加律)——终端的逐步
    // 流水账(TurnUsageStats)从这份 payload 里还原 UsageReport,不必另开
    // 一条旁路回调。
    void OnUsage(const api::UsageReport& report, bool subordinate = false) {
        ServerEvent event = MakeEvent(ServerEventKind::UsageUpdated);
        event.payload = nlohmann::json{{"input_tokens", report.usage.input_tokens},
                                       {"output_tokens", report.usage.output_tokens},
                                       {"cache_read_tokens", report.usage.cache_read_tokens},
                                       {"cache_creation_tokens", report.usage.cache_creation_tokens},
                                       {"reasoning_tokens", report.usage.output_reasoning_tokens},
                                       {"model", report.model},
                                       {"reported", report.reported()},
                                       {"step_index", report.step_index},
                                       {"request_id", report.request_id},
                                       {"cache_epoch", report.cache_epoch},
                                       {"epoch_break_reason", report.epoch_break_reason},
                                       {"prefix_append_only", report.prefix_append_only}};
        MarkSubordinate(event, subordinate);
        Emit(std::move(event));
    }

    // ---- 回合边界(step/批次):turn 层事件,不带 item_id ----------------

    void OnModelStepStarted(int step_index) {
        ServerEvent event = MakeEvent(ServerEventKind::ModelStepStarted);
        event.payload = nlohmann::json{{"step_index", step_index}};
        Emit(std::move(event));
    }

    void OnToolBatchStarted(int step_index, int batch_index,
                            const std::vector<std::string>& ordered_tool_use_ids) {
        ServerEvent event = MakeEvent(ServerEventKind::ToolBatchStarted);
        event.payload = nlohmann::json{{"step_index", step_index},
                                       {"batch_index", batch_index},
                                       {"ordered_tool_use_ids", ordered_tool_use_ids}};
        Emit(std::move(event));
    }

    void OnToolBatchFinished(int batch_index, bool interrupted) {
        ServerEvent event = MakeEvent(ServerEventKind::ToolBatchFinished);
        event.payload = nlohmann::json{{"batch_index", batch_index}, {"interrupted", interrupted}};
        Emit(std::move(event));
    }

    // ------------------------------------------------------------------
    // 从路并入:嵌套回合(子代理/PTC 之外的宿主侧装配)把已翻好的事件
    // 原样递进来——本流只补 subordinate 标与转发,不动条目状态机。
    // ------------------------------------------------------------------
    void ForwardFromSubordinate(const ServerEvent& event) {
        ServerEvent forwarded = event;
        MarkSubordinate(forwarded, true);
        Emit(std::move(forwarded));
    }

    // 一轮结束:没收尾的条目按给定终态收口(正常跑完 = Succeeded,打断 =
    // Cancelled,出错 = Failed),发 TurnCompleted。幂等:重复调只发一次
    // TurnCompleted。
    void Finish(Outcome outcome, const std::string& error_message = std::string()) {
        CloseText();
        CloseThinking();
        for (auto& [tool_use_id, item_id] : open_tools_) {
            (void)tool_use_id;
            ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, item_id, ItemKind::Tool);
            event.outcome = Outcome::Cancelled;
            event.payload = nlohmann::json{{"result", std::string()}, {"is_error", false}};
            Emit(std::move(event));
        }
        open_tools_.clear();
        if (turn_finished_) {
            return;
        }
        turn_finished_ = true;
        ServerEvent event = MakeEvent(ServerEventKind::TurnCompleted);
        event.outcome = outcome;
        if (!error_message.empty()) {
            event.payload = nlohmann::json{{"error", error_message}};
        }
        Emit(std::move(event));
    }

private:
    ServerEvent MakeEvent(ServerEventKind kind, const std::string& item_id = std::string(),
                          ItemKind item_kind = ItemKind::Tool) {
        ServerEvent event;
        event.envelope.thread_id = thread_id_;
        event.envelope.seq = ids_.NextSeq();
        event.envelope.timestamp_ms = NowMs();
        event.kind = kind;
        event.turn_id = turn_id_;
        event.item_id = item_id;
        event.item_kind = item_kind;
        return event;
    }

    std::string StartItem(ItemKind kind, const std::string& tool_name, const std::string& tool_use_id = std::string(),
                          const nlohmann::json& input = nlohmann::json::object(), bool builtin = false,
                          bool subordinate = false) {
        const std::string item_id = ids_.NextItemId();
        ServerEvent event = MakeEvent(ServerEventKind::ItemStarted, item_id, kind);
        if (!tool_name.empty()) {
            event.payload = nlohmann::json{{"tool_name", tool_name}};
            if (!tool_use_id.empty()) {
                event.payload["tool_use_id"] = tool_use_id;
            }
            if (!input.is_null() && !input.empty()) {
                event.payload["input"] = input;
            }
            if (builtin) {
                event.payload["builtin"] = true;
            }
        }
        MarkSubordinate(event, subordinate);
        Emit(std::move(event));
        return item_id;
    }

    // 从路标记:payload 里落一枚稳定键。画屏侧(终端 sink)凭它跳过嵌套
    // 回合的条目(画面规矩:子代理只画外层卡,不刷主屏),账面侧不认它,
    // 照单全收。主路(subordinate=false)一个字节不加,payload 与老路
    // 逐字节一致。
    static void MarkSubordinate(ServerEvent& event, bool subordinate) {
        if (subordinate) {
            event.payload["subordinate"] = true;
        }
    }

    // 正文/思考没有显式的"结束"回调:下一枚 item 开始时收上一枚(终态唯一,
    // 已收过的不再发)。终态四分里"顺利说完"算 Succeeded。
    void CloseText() {
        if (text_item_id_.empty()) {
            return;
        }
        ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, text_item_id_, ItemKind::Text);
        event.outcome = Outcome::Succeeded;
        Emit(std::move(event));
        text_item_id_.clear();
    }

    void CloseThinking() {
        if (thinking_item_id_.empty()) {
            return;
        }
        ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, thinking_item_id_, ItemKind::Thinking);
        event.outcome = Outcome::Succeeded;
        Emit(std::move(event));
        thinking_item_id_.clear();
    }

    // 工具终态:id 查表,查到即摘;空 id 兜底"最早一个进行中的"(与 ToolDisplay
    // 的旧路同款)。查不到 = 迟到/陌生,返回空。
    std::string TakeToolItem(const std::string& tool_use_id) {
        if (!tool_use_id.empty()) {
            const auto it = open_tools_.find(tool_use_id);
            if (it == open_tools_.end()) {
                return std::string();
            }
            const std::string item_id = it->second;
            open_tools_.erase(it);
            return item_id;
        }
        if (open_tools_.empty()) {
            return std::string();
        }
        const std::string item_id = open_tools_.begin()->second;
        open_tools_.erase(open_tools_.begin());
        return item_id;
    }

    static std::int64_t NowMs() {
        // 两枚 now 的差 + 显式 duration_cast(chrono 跨 clock 铁律,不用
        // to_sys/clock_cast):系统钟现值与 Unix epoch 的差。
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    void Emit(ServerEvent event) {
        if (sink_) {
            sink_(event);
        }
    }

    std::string thread_id_;
    IdAuthority& ids_;
    std::function<void(const ServerEvent&)> sink_;
    std::string turn_id_;
    std::string text_item_id_;
    std::string thinking_item_id_;
    std::map<std::string, std::string> open_tools_;  // tool_use_id -> item_id
    bool turn_finished_ = false;
};

}  // namespace lubancode::runtime
