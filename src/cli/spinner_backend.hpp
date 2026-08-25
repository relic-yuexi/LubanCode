// SpinnerBackend(骨架拆解批二自 app/backend_stack 挪来):"思考中"转轮的
// Backend 包装。它是 UI 件,不是传输件——本就该住终端显示层(cli),由
// 终端装配层(one_shot/interactive_session)搭画面时包上;backend_stack
// 那串请求改写器(传输层)不再掺 UI(单子病十一:"SpinnerBackend 是 UI
// 混进传输层,挪去 sink 侧")。
//
// 包一层 Backend:发起真正的网络请求前起一个"思考中"转轮(cli::Spinner),
// 收到第一个流事件就停。转轮跟着 send_stream 这一次调用走——AgentLoop 一次
// Run() 里可能因为工具调用来回好几趟,每趟各自单独调一次 send_stream,
// 工具执行发生在两次 send_stream 之间(loop.cpp 里,不在这层包装范围内),
// 天然满足"工具执行期间不转,发下一轮请求再转"这条要求,不用改
// agent/loop.cpp 一个字。spinner_enabled 由调用方按"stdout 是不是真控制台"
// 算好传进来——管道模式下这层直接透传,不起线程、不输出任何转轮字符。
//
// 留一句实话:它至今仍是 Backend 包装而非 EventSink——引擎只在流事件里
// "说话",没有"请求已发出、还没第一个字节"的事件可挂;等批四把请求
// 管道收进 RequestProfile、事件流补上请求级起止,这只转轮再改吃事件。

#pragma once

#include <atomic>
#include <expected>
#include <functional>

#include "api/backend.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

class SpinnerBackend : public lubancode::api::Backend {
public:
    SpinnerBackend(lubancode::api::Backend& inner, const lubancode::cli::Theme& theme, bool spinner_enabled);

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

private:
    lubancode::api::Backend& inner_;
    const lubancode::cli::Theme& theme_;
    bool spinner_enabled_;
};

}  // namespace lubancode::cli
