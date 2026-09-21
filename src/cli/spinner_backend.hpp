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
//
// HC-08(包装层预算映射与出站能力转发):与 RebuildableBackend 同病——只
// override send_stream,预算/映射/出站能力五口吃基类默认,单发与终端链
// 包了这层壳就丢叶 client 合同。修法同款:逐项窄转发 inner_。这层握的
// 是 Backend&(装配方保内芯活过自己),转发零加锁零状态;动画职责一分
// 不变,spinner_enabled=false 时本就纯透传,转发不掺一个转轮字节。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <optional>
#include <string>

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

    // HC-08 五口窄转发:这层不改协议、不攒状态,内芯说什么就是什么。
    // 未装配/不可得的语义由内芯自己负责(它握引用,装配方保活)。
    std::string SerializeForDiagnostics(const lubancode::api::Request& request) const override;
    lubancode::api::PreparedWireRequest PrepareWireRequest(
        const lubancode::api::Request& request) const override;
    std::optional<lubancode::api::WireMessageMap> BuildWireMessageMap(
        const lubancode::api::Request& request) const override;
    EffectiveOutputLimit GetEffectiveOutputLimit(const lubancode::api::Request& request) const override;
    void ForceMaxOutputTokensOverride(lubancode::api::Request& request, int tokens) const override;

private:
    lubancode::api::Backend& inner_;
    const lubancode::cli::Theme& theme_;
    bool spinner_enabled_;
};

}  // namespace lubancode::cli
