// 测试共用的回合事件录音器(骨架拆解批二余款:Callbacks 显示回调退役,
// 测试里"看模型说了什么/跑了什么工具/报了多少 usage"的观察点改吃事件流)。
//
// 用法:
//   lubancode::test::RecordedTurn turn;
//   agent::TurnWiring wiring;
//   wiring.events = &turn.adapter;          // 控制口照旧各配各的
//   loop.Run("问一句", wiring);
//   CHECK(turn.recorder.text == "你好");
//
// 一只 RecordedTurn 一轮:构造时 Start(现发 turn id),析构即弃。要断言
// 事件原文的,直接读 recorder.events。

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_event_adapter.hpp"

namespace lubancode::test {

// ServerEvent 流 -> 测试友好的账:正文/思考累计、工具起止(名 + 入参 +
// 结果)、usage 逐笔(UsageReport 身份齐)、step/批次边界。
class TurnEventRecorder final : public runtime::EventSink {
public:
    struct ToolStart {
        std::string tool_use_id;
        std::string name;
        nlohmann::json input;
    };
    struct ToolDone {
        std::string tool_use_id;
        std::string name;  // ItemStarted 对账还原;找不到(陌生终态)为空
        std::string content;
        bool is_error = false;
    };

    void Emit(const runtime::ServerEvent& event) override {
        events.push_back(event);
        switch (event.kind) {
            case runtime::ServerEventKind::ItemDelta:
                if (event.item_kind == runtime::ItemKind::Text) {
                    text += event.text;
                } else if (event.item_kind == runtime::ItemKind::Thinking) {
                    thinking += event.text;
                }
                break;
            case runtime::ServerEventKind::ItemStarted:
                if (event.item_kind == runtime::ItemKind::Tool) {
                    const std::string tool_use_id = event.payload.value("tool_use_id", std::string());
                    const std::string name = event.payload.value("tool_name", std::string());
                    started_tools.push_back({tool_use_id, name,
                                             event.payload.contains("input") ? event.payload["input"]
                                                                             : nlohmann::json::object()});
                    open_tools_[event.item_id] = {tool_use_id, name};
                }
                break;
            case runtime::ServerEventKind::ItemCompleted:
                if (event.item_kind == runtime::ItemKind::Tool) {
                    const auto it = open_tools_.find(event.item_id);
                    const std::string tool_use_id = it != open_tools_.end() ? it->second.first : std::string();
                    const std::string name = it != open_tools_.end() ? it->second.second : std::string();
                    if (it != open_tools_.end()) {
                        open_tools_.erase(it);
                    }
                    if (event.outcome != runtime::Outcome::Cancelled) {
                        done_tools.push_back({tool_use_id, name, event.payload.value("result", std::string()),
                                              event.payload.value("is_error", false)});
                    }
                }
                break;
            case runtime::ServerEventKind::UsageUpdated: {
                api::UsageReport report;
                report.usage.input_tokens = event.payload.value("input_tokens", std::int64_t{0});
                report.usage.output_tokens = event.payload.value("output_tokens", std::int64_t{0});
                report.usage.cache_read_tokens = event.payload.value("cache_read_tokens", std::int64_t{0});
                report.usage.cache_creation_tokens = event.payload.value("cache_creation_tokens", std::int64_t{0});
                report.usage.output_reasoning_tokens = event.payload.value("reasoning_tokens", std::int64_t{0});
                report.step_index = event.payload.value("step_index", 0);
                report.provider_response_id = event.payload.value("provider_response_id", std::string());
                report.reported_by_provider = event.payload.value("reported_by_provider", false);
                report.model = event.payload.value("model", std::string());
                report.cache_epoch = event.payload.value("cache_epoch", 1);
                report.epoch_break_reason = event.payload.value("epoch_break_reason", std::string());
                report.prefix_append_only = event.payload.value("prefix_append_only", true);
                reported_usage = event.payload.value("reported", report.reported());
                usage_reports.push_back(report);
                break;
            }
            case runtime::ServerEventKind::ModelStepStarted:
                steps.push_back(event.payload.value("step_index", 0));
                break;
            case runtime::ServerEventKind::ToolBatchStarted: {
                std::vector<std::string> ordered;
                if (event.payload.contains("ordered_tool_use_ids") &&
                    event.payload["ordered_tool_use_ids"].is_array()) {
                    ordered = event.payload["ordered_tool_use_ids"].get<std::vector<std::string>>();
                }
                batches_started.push_back({event.payload.value("step_index", 0),
                                           event.payload.value("batch_index", 0), std::move(ordered)});
                break;
            }
            case runtime::ServerEventKind::ToolBatchFinished:
                batches_finished.push_back(
                    {event.payload.value("batch_index", 0), event.payload.value("interrupted", false)});
                break;
            default:
                break;
        }
    }

    std::vector<runtime::ServerEvent> events;  // 原始流(次序/载荷断言用)
    std::string text;                          // 正文累计
    std::string thinking;                      // 思考累计
    std::vector<ToolStart> started_tools;
    std::vector<ToolDone> done_tools;
    std::vector<api::UsageReport> usage_reports;
    bool reported_usage = false;
    std::vector<int> steps;
    struct BatchStarted {
        int step_index = 0;
        int batch_index = 0;
        std::vector<std::string> ordered_tool_use_ids;
    };
    std::vector<BatchStarted> batches_started;
    struct BatchFinished {
        int batch_index = 0;
        bool interrupted = false;
    };
    std::vector<BatchFinished> batches_finished;

private:
    std::map<std::string, std::pair<std::string, std::string>> open_tools_;  // item_id -> {id, name}
};

// 一轮录音装配:本地发号局 + 适配器 + 录音 sink。
struct RecordedTurn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    TurnEventRecorder recorder;
    RecordedTurn() : adapter("test", ids) {
        adapter.Attach([this](const runtime::ServerEvent& event) { recorder.Emit(event); });
        adapter.Start();
    }
};

}  // namespace lubancode::test
