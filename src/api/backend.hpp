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

    // 内部消息序 -> wire 元素序的拍平对照(轨迹 v3 差距清单 §8.2 第 7 条)。
    // 供 v3 账 model.request.prepared 的 inputMessageRefs 与实际发出的 wire
    // 消息序对账/验尸:内外消息数量不一一相等是常态(schema §8.1 横切),
    // 不能假定逐条对位。AgentLoop 把结果随 RequestPreparedContext 递给
    // 边界账。默认 nullopt = 该 backend(trace/桩/后台派生类)不提供,
    // 消费方按"映射不可得"处理,不冒充。
    virtual std::optional<WireMessageMap> BuildWireMessageMap(const Request& request) const {
        (void)request;
        return std::nullopt;
    }

    // extra_body 覆盖后的有效输出上限(轨迹 v3 差距清单 §8.2 第 8 条,
    // 单子 §1.18"容量判断采用本次请求实际生效的输出上限")。
    //   tokens —— wire 上真带的有效上限(覆盖后);nullopt = 请求与
    //             extra_body 都没写(chat/responses/gemini 不带字段,交
    //             服务端默认;anthropic 必填,由自家 client 落公开兜底,
    //             不会是 nullopt)。
    //   overridden —— provider 级或请求级 extra_body 真写过输出上限键。
    //             真时容量侧的输出预留直接吃 tokens(用户手笔,不受能力
    //             级封顶,与 ConfigFile 同款例外);假时 tokens 即
    //             Request::max_tokens 原值,预留走既有封顶路(主会话
    //             输出预留占坑单 §4.1 的帽不因此失效)。
    // 默认原样返回(不提供覆盖面的 trace/桩后端,与从前一字不差)。
    struct EffectiveOutputLimit {
        std::optional<int> tokens;
        bool overridden = false;
    };
    virtual EffectiveOutputLimit GetEffectiveOutputLimit(const Request& request) const {
        return {request.max_tokens, false};
    }

    // 收窄后的输出上限写进请求级 extra_body 覆盖位(差距清单 §8.2 第 8 条
    // 写侧):extra_body 写过输出上限键时,只改 Request::max_tokens 出不
    // 了门——extra_body 尾部合并会把宽的覆盖值压回去,窄值必须写进合并
    // 序最后的请求级键上(压过 provider 级)。自家 extra_body 没写过该键
    // 的请求是 no-op:不无中生有造键,出口形状与从前逐字节一致。默认
    // no-op(trace/桩后端)。
    virtual void ForceMaxOutputTokensOverride(Request& request, int tokens) const {
        (void)request;
        (void)tokens;
    }
};

}  // namespace lubancode::api
