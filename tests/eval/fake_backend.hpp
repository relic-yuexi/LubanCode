// Q2 量化评测(工具与上下文治理量化评测单)P0 装置的通用假后端。
//
// 蓝本是 tests/unit/agent/test_loop.cpp 里那支脚本式 FakeBackend——全仓
// 33 处私有假后端中最有代表性的一支(脚本序列 + 请求捕获 + 取消注入,
// test_turn_harness.cpp 注释里写明"与 test_agent_tool.cpp 同款"的就是这
// 一族)。在其之上按单子 P0 段补三件通用化:
//   1. 可编程响应序列:scripts 每元素是一轮 send_stream 要吐的事件组,
//      逐请求取下一组,耗尽报错(与原实现一字不差的语义);
//   2. 可注错误/断流:error_on_request 让第 N 个请求整体报错(注网络错/
//      HTTP 错),abort_after_event 让某请求吐完第 M 个事件后断流(半截
//      脚本喂出去就返回错误——量 compaction/subagent 失败分布要用);
//   3. usage 记账:on_usage 在每个 MessageDone 上回调一笔,usage_log 留
//      全账;first_event_latency_ms 逐请求记"进 send_stream 到第一个
//      事件回调"的微秒数——实验 A 的首 token 延迟取的就是它。
//
// 只给 tests/eval/ 的驱动用,不进 lubancode_tests(默认构建零扰动)。
// 既有 33 处私有假后端不动:各有私有变体(成员名/取消语义/usage 落账
// 都有微差),强行统一必破"行为零变",其中 3 处还在 tests/unit/app/
// 手术禁区。够用就好,不造万能件。

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode_eval {

class FakeStreamingBackend : public lubancode::api::Backend {
public:
    // 每调一次 send_stream 按调用次序取下一组脚本吐出去(蓝本语义)。
    std::vector<std::vector<lubancode::api::StreamEvent>> scripts;
    // 收到的请求逐笔留底,方便对账(tools 数组有几枚、system 带没带
    // 索引段、历史里 tool_result 接没接上)。
    std::vector<lubancode::api::Request> captured_requests;
    // 每个 send_stream 从进入到第一个事件回调的微秒数;没有吐过事件
    // 的请求不记。下标与 captured_requests 对齐。
    std::vector<double> first_event_latency_us;
    // 蓝本语义原样保留:发完脚本里下标为该值的事件就返回 Cancelled,
    // 后续事件不再喂——模拟"传输层发现取消标志就地掐断"。
    std::optional<std::size_t> cancel_after_event_index;
    // 第 N 个请求(0-based)整体报这个错,脚本一个事件都不吐。nullopt =
    // 不注。注整体失败(连不上/HTTP 非 2xx)用它。
    struct RequestError {
        std::size_t request_index = 0;
        lubancode::api::ErrorKind kind = lubancode::api::ErrorKind::Network;
        std::string message = "FakeStreamingBackend: 注入的请求级错误";
        int http_status = 0;
    };
    std::optional<RequestError> error_on_request;
    // 第 N 个请求(0-based)吐完脚本里下标为 event_index 的事件后断流:
    // 返回 kind 指定的错误,后续事件不喂。与 cancel_after_event_index
    // 的差别只在错误类型与作用请求——注"半路断流"用它。
    struct AbortAfterEvent {
        std::size_t request_index = 0;
        std::size_t event_index = 0;
        lubancode::api::ErrorKind kind = lubancode::api::ErrorKind::Network;
        std::string message = "FakeStreamingBackend: 注入的半路断流";
    };
    std::optional<AbortAfterEvent> abort_after_event;
    // usage 记账回调:每个 MessageDone 一笔(不管 usage_reported 真假,
    // 全零也记——区分"明报全零"与"没报"是消费端拿 MessageDone 的
    // usage_reported 自己判的事)。
    std::function<void(std::size_t request_index, const lubancode::api::MessageDone&)> on_usage;
    // on_usage 之外的全账留底(不想挂回调时直接读它)。
    struct UsageEntry {
        std::size_t request_index = 0;
        lubancode::api::Usage usage{};
        bool usage_reported = false;
    };
    std::vector<UsageEntry> usage_log;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

    // 便捷拿数:第 i 个请求的首 token 延迟(毫秒,双精度)。请求不存在或
    // 没吐过事件返回 nullopt(调用方按零分母 unavailable 记账,不冒充 0)。
    std::optional<double> FirstEventLatencyMs(std::size_t request_index) const;
};

// ---- 脚本工厂(与 test_loop.cpp 的 TextOnlyScript/ToolUseScript 同形) ----

// 一轮纯文本直收:end_turn。
std::vector<lubancode::api::StreamEvent> TextScript(const std::string& text,
                                                    const lubancode::api::Usage& usage = {});
// 一轮单工具调用:tool_use 收口,入参是完整 JSON 字符串。
std::vector<lubancode::api::StreamEvent> ToolUseScript(const std::string& tool_id,
                                                       const std::string& tool_name,
                                                       const std::string& input_json,
                                                       const lubancode::api::Usage& usage = {});

// 编译期 + 运行期自检(eval_smoke 调;证明这个头在真实编译单元里可用)。
// 全过返回 true;失败把第一处不对写到 error。
bool RunFakeBackendSelfCheck(std::string* error);

}  // namespace lubancode_eval
