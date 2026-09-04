// FakeStreamingBackend 的实现与自检。声明见 fake_backend.hpp。

#include "fake_backend.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace lubancode_eval {

std::expected<void, lubancode::api::Error> FakeStreamingBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* /*cancel*/) {
    using clock = std::chrono::steady_clock;
    captured_requests.push_back(request);
    const std::size_t index = captured_requests.size() - 1;
    first_event_latency_us.push_back(-1.0);  // 占位:没吐事件就保持 -1
    if (error_on_request.has_value() && error_on_request->request_index == index) {
        return std::unexpected(lubancode::api::Error{error_on_request->kind, error_on_request->message,
                                                     error_on_request->http_status});
    }
    if (index >= scripts.size()) {
        return std::unexpected(lubancode::api::Error{
            lubancode::api::ErrorKind::Api, "FakeStreamingBackend: 脚本用完了", 0});
    }
    const auto entered_at = clock::now();
    const auto& script = scripts[index];
    for (std::size_t i = 0; i < script.size(); ++i) {
        if (first_event_latency_us[index] < 0) {
            first_event_latency_us[index] =
                std::chrono::duration<double, std::micro>(clock::now() - entered_at).count();
        }
        on_event(script[i]);
        if (const auto* done = std::get_if<lubancode::api::MessageDone>(&script[i])) {
            usage_log.push_back(UsageEntry{index, done->usage, done->usage_reported});
            if (on_usage) {
                on_usage(index, *done);
            }
        }
        if (abort_after_event.has_value() && abort_after_event->request_index == index &&
            abort_after_event->event_index == i) {
            return std::unexpected(lubancode::api::Error{abort_after_event->kind,
                                                         abort_after_event->message, 0});
        }
        if (cancel_after_event_index.has_value() && *cancel_after_event_index == i) {
            return std::unexpected(
                lubancode::api::Error{lubancode::api::ErrorKind::Cancelled,
                                      "FakeStreamingBackend: 模拟取消", 0});
        }
    }
    return {};
}

std::optional<double> FakeStreamingBackend::FirstEventLatencyMs(std::size_t request_index) const {
    if (request_index >= first_event_latency_us.size() ||
        first_event_latency_us[request_index] < 0) {
        return std::nullopt;
    }
    return first_event_latency_us[request_index] / 1000.0;
}

std::vector<lubancode::api::StreamEvent> TextScript(const std::string& text,
                                                    const lubancode::api::Usage& usage) {
    return {
        lubancode::api::MessageStart{"msg", "model"},
        lubancode::api::TextDelta{text},
        lubancode::api::ContentBlockDone{0},
        lubancode::api::MessageDone{"end_turn", usage, true},
    };
}

std::vector<lubancode::api::StreamEvent> ToolUseScript(const std::string& tool_id,
                                                       const std::string& tool_name,
                                                       const std::string& input_json,
                                                       const lubancode::api::Usage& usage) {
    return {
        lubancode::api::MessageStart{"msg", "model"},
        lubancode::api::ToolUseStart{0, tool_id, tool_name},
        lubancode::api::ToolUseInputDelta{0, input_json},
        lubancode::api::ContentBlockDone{0},
        lubancode::api::MessageDone{"tool_use", usage, true},
    };
}

bool RunFakeBackendSelfCheck(std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };

    // 1) 两轮脚本:text + tool_use,请求捕获与 usage 记账对齐。
    {
        FakeStreamingBackend backend;
        backend.scripts = {
            TextScript("你好", lubancode::api::Usage{100, 5, 0, 0, 0}),
            ToolUseScript("t1", "read_file", "{}", lubancode::api::Usage{200, 7, 0, 0, 0}),
        };
        int usage_calls = 0;
        backend.on_usage = [&](std::size_t, const lubancode::api::MessageDone&) { ++usage_calls; };
        std::size_t events = 0;
        const auto sink = [&](const lubancode::api::StreamEvent&) { ++events; };
        if (!backend.send_stream(lubancode::api::Request{}, sink).has_value()) {
            return fail("第一轮 text 脚本竟然失败了");
        }
        if (!backend.send_stream(lubancode::api::Request{}, sink).has_value()) {
            return fail("第二轮 tool_use 脚本竟然失败了");
        }
        if (backend.captured_requests.size() != 2) {
            return fail("请求捕获笔数不对");
        }
        if (events != 9) {
            return fail("事件总数不对(text 4 + tool_use 5 事件)");
        }
        if (usage_calls != 2 || backend.usage_log.size() != 2) {
            return fail("usage 记账笔数不对");
        }
        if (backend.usage_log[0].usage.input_tokens != 100 ||
            !backend.usage_log[0].usage_reported) {
            return fail("第一笔 usage 账不对");
        }
        if (!backend.FirstEventLatencyMs(0).has_value() ||
            *backend.FirstEventLatencyMs(0) < 0.0) {
            return fail("首 token 延迟没记上");
        }
    }

    // 2) 脚本耗尽:第三次请求报 Api 错。
    {
        FakeStreamingBackend backend;
        backend.scripts = {TextScript("一")};
        const auto sink = [](const lubancode::api::StreamEvent&) {};
        (void)backend.send_stream(lubancode::api::Request{}, sink);
        const auto exhausted = backend.send_stream(lubancode::api::Request{}, sink);
        if (exhausted.has_value() || exhausted.error().kind != lubancode::api::ErrorKind::Api) {
            return fail("脚本耗尽没有按 Api 错报出");
        }
    }

    // 3) 请求级错误注入:第 0 个请求整体 Network 错,一个事件不吐。
    {
        FakeStreamingBackend backend;
        backend.scripts = {TextScript("一")};
        backend.error_on_request = FakeStreamingBackend::RequestError{
            0, lubancode::api::ErrorKind::Network, "注:连不上", 0};
        std::size_t events = 0;
        const auto result = backend.send_stream(
            lubancode::api::Request{}, [&](const lubancode::api::StreamEvent&) { ++events; });
        if (result.has_value() || result.error().kind != lubancode::api::ErrorKind::Network ||
            events != 0) {
            return fail("请求级错误注入没生效或还是吐了事件");
        }
        if (backend.FirstEventLatencyMs(0).has_value()) {
            return fail("没吐事件的请求不该有首 token 延迟(unavailable 语义)");
        }
    }

    // 4) 半路断流:第 0 个请求吐完第 1 个事件后断,后续事件不喂。
    {
        FakeStreamingBackend backend;
        backend.scripts = {TextScript("一二三")};
        backend.abort_after_event = FakeStreamingBackend::AbortAfterEvent{
            0, 1, lubancode::api::ErrorKind::Network, "注:半路断流"};
        std::size_t events = 0;
        const auto result = backend.send_stream(
            lubancode::api::Request{}, [&](const lubancode::api::StreamEvent&) { ++events; });
        if (result.has_value() || events != 2) {
            return fail("半路断流没有在第 2 个事件后掐断");
        }
    }

    // 5) 取消注入(蓝本语义):发完下标 1 的事件返回 Cancelled。
    {
        FakeStreamingBackend backend;
        backend.scripts = {TextScript("一二三")};
        backend.cancel_after_event_index = 1;
        std::size_t events = 0;
        const auto result = backend.send_stream(
            lubancode::api::Request{}, [&](const lubancode::api::StreamEvent&) { ++events; });
        if (result.has_value() || result.error().kind != lubancode::api::ErrorKind::Cancelled ||
            events != 2) {
            return fail("取消注入没有按 Cancelled 返回");
        }
    }

    return true;
}

}  // namespace lubancode_eval
