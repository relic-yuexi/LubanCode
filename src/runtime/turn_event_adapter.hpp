// TurnEventAdapter(显示系统剥离单第五步后半)。
//
// agent::Callbacks -> ServerEvent 流的适配器:一轮 Run() 里模型给的正文/
// 思考/工具起止/usage,逐枚翻成带 seq/thread_id/turn_id/item_id 的
// ServerEvent,交给 EventSink。终端(TerminalEventSink)、app-server
// (JsonEventSink)、Web/Tauri 都从这只适配器手上拿同一份事件流——
// "内核只吐结构化事件"的单子边界,这只就是出水口。
//
// 与 app/turn_runner.cpp 的 BuildCallbacks 的分工(第六步接线后的格局):
//   - BuildCallbacks 住终端装配层,直接喂 ToolDisplay 画现有 TUI(行为
//     逐字不变的老路,过渡期两轨并行);
//   - 本适配器住 runtime 层,零 cli 依赖,给远端前端与同流验收测试;
//   - SessionRuntime(第六步)立起来后,终端的老路逐步改吃事件流,这只
//     适配器升正房。
//
// id 规矩(event.hpp 文件头):thread_id 会话级、turn_id 一轮、item_id
// 一条;全部从 IdAuthority 发,seq thread 内单调。正文/思考各占一枚
// item(多条 TextDelta 并入同一枚,首次 delta 开 ItemStarted,收不到
// 显式边界就在下一枚 item 开始时收上一枚)。
//
// 依赖:合同头 + agent/loop.hpp 的 Callbacks,零 cli/app 依赖。

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"

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

    // 挂事件落点(可换多次;每次 MakeCallbacks 之前挂好)。
    void Attach(std::function<void(const ServerEvent&)> sink) { sink_ = std::move(sink); }

    // 一轮开始:发 turn_id、TurnStarted;返回这轮的 turn_id(调用方对账)。
    // 每轮调一次;上一轮没收尾的条目在这里统一按 Cancelled 收口(打断/
    // 异常路径不会再有自然终态)。
    std::string Start() {
        turn_id_ = ids_.NextTurnId();
        text_item_id_.clear();
        thinking_item_id_.clear();
        open_tools_.clear();
        Emit(MakeEvent(ServerEventKind::TurnStarted));
        return turn_id_;
    }

    // 本轮 turn_id(Start 之前为空)。
    const std::string& turn_id() const { return turn_id_; }

    // 装配这一轮的回调:正文/思考/工具起止/审批/usage 全翻译,零终端依赖。
    agent::Callbacks MakeCallbacks() {
        agent::Callbacks callbacks;

        callbacks.on_text_delta = [this](const std::string& text) {
            CloseThinking();
            if (text_item_id_.empty()) {
                text_item_id_ = StartItem(ItemKind::Text, std::string());
            }
            ServerEvent event = MakeEvent(ServerEventKind::ItemDelta, text_item_id_, ItemKind::Text);
            event.text = text;
            Emit(std::move(event));
        };

        callbacks.on_thinking_delta = [this](const std::string& text) {
            CloseText();
            if (thinking_item_id_.empty()) {
                thinking_item_id_ = StartItem(ItemKind::Thinking, std::string());
            }
            ServerEvent event = MakeEvent(ServerEventKind::ItemDelta, thinking_item_id_, ItemKind::Thinking);
            event.text = text;
            Emit(std::move(event));
        };

        callbacks.on_tool_start = [this](const std::string& tool_use_id, const std::string& name,
                                         const nlohmann::json& input) {
            CloseText();
            CloseThinking();
            const std::string item_id = StartItem(ItemKind::Tool, name, tool_use_id, input);
            open_tools_.emplace(tool_use_id, item_id);
        };

        callbacks.on_tool_done = [this](const std::string& tool_use_id, const std::string& name,
                                        const tools::Tool::Result& result) {
            (void)name;
            const std::string item_id = TakeToolItem(tool_use_id);
            if (item_id.empty()) {
                return;  // 迟到/陌生终态:丢弃不误伤(与 ToolDisplay 同规矩)
            }
            ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, item_id, ItemKind::Tool);
            event.outcome = result.is_error ? Outcome::Failed : Outcome::Succeeded;
            event.payload = nlohmann::json{{"result", result.content}, {"is_error", result.is_error}};
            Emit(std::move(event));
        };

        callbacks.on_builtin_tool_start = [this](const std::string& tool_use_id, const std::string& name,
                                                 const nlohmann::json& input) {
            CloseText();
            CloseThinking();
            const std::string item_id = StartItem(ItemKind::Tool, name, tool_use_id, input);
            open_tools_.emplace(tool_use_id, item_id);
        };

        callbacks.on_builtin_tool_done = [this](const std::string& tool_use_id, const std::string& name,
                                                const nlohmann::json& input, const std::string& summary,
                                                bool is_error) {
            (void)name;
            (void)input;
            const std::string item_id = TakeToolItem(tool_use_id);
            if (item_id.empty()) {
                return;
            }
            ServerEvent event = MakeEvent(ServerEventKind::ItemCompleted, item_id, ItemKind::Tool);
            event.outcome = is_error ? Outcome::Failed : Outcome::Succeeded;
            event.payload = nlohmann::json{{"result", summary}, {"is_error", is_error}};
            Emit(std::move(event));
        };

        callbacks.on_usage = [this](const api::UsageReport& report) {
            ServerEvent event = MakeEvent(ServerEventKind::UsageUpdated);
            event.payload = nlohmann::json{{"input_tokens", report.usage.input_tokens},
                                           {"output_tokens", report.usage.output_tokens},
                                           {"cache_read_tokens", report.usage.cache_read_tokens},
                                           {"cache_creation_tokens", report.usage.cache_creation_tokens},
                                           {"reasoning_tokens", report.usage.output_reasoning_tokens},
                                           {"model", report.model},
                                           {"reported", report.reported()}};
            Emit(std::move(event));
        };

        return callbacks;
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
                          const nlohmann::json& input = nlohmann::json::object()) {
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
        }
        Emit(std::move(event));
        return item_id;
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
