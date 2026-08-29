// api 层对上暴露的抽象接口。agent 层只认这一个接口,不关心背后是
// Anthropic Messages 还是 OpenAI Responses 在干活。

#pragma once

#include <atomic>
#include <expected>
#include <functional>

#include "api/types.hpp"

namespace lubancode::api {

class Backend {
public:
    virtual ~Backend() = default;

    // 发一轮消息,流式拿结果。每收到一个语义事件就回调一次 on_event。
    // 失败(网络错、HTTP 非 200……)时返回 Error,调用方自己判断、自己处理。
    // cancel 非空且流式过程中被置位:两个具体后端在写回调里发现就地掐断
    // 传输,返回 Error{Kind::Cancelled,...}(不是网络错,调用方——agent 层——
    // 得把这种情况跟真出错分开处理)。cancel 为空指针等于永不取消,维持
    // 老行为。
    virtual std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) = 0;

    // 诊断模式专用(真实实测问题单问题 9):按本 backend 的 wire 把请求
    // 序列化成 JSON 文本,只用于"与上一份请求的公共前缀字节"对账——
    // 不发送、不落盘,只在 LUBANCODE_DEBUG_PREFIX 打开时被调用。默认
    // 返回空串 = 该 backend(trace/桩/后台派生类)不提供,诊断账记
    // "不可得"(-1),不冒充 0。
    virtual std::string SerializeForDiagnostics(const Request& request) const { (void)request; return {}; }
};

}  // namespace lubancode::api
