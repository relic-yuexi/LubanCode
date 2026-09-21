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

    // FD-02(请求最终出站状态与影子预算映射收敛)的窄口:一次拼出请求的
    // 最终出站状态——出门体、有效输出上限、拍平映射共享同一条拼装路
    //(SanitizeRequest + BuildRequestJson 带映射指针 + extra_body 尾部
    // 合并)。四家真后端 override 它,上面三口(序列化/映射/上限)的
    // 实现都从这份结果取数,不再各算一份影子。后继单(HC-08 包装层、
    // AR-10 采用门禁)直接消费这只口:出站的最终事实只此一份。
    // 默认实现给"不可得"形态(trace/桩/包装后端):body 空、映射
    // nullopt、上限回退 Request::max_tokens 原值——与旧
    // GetEffectiveOutputLimit 默认一字不差,不换皮的派生类行为不漂。
    virtual PreparedWireRequest PrepareWireRequest(const Request& request) const {
        PreparedWireRequest prepared;
        prepared.output_limit = request.max_tokens;
        return prepared;
    }

    // 内部消息序 -> wire 元素序的拍平对照(轨迹 v3 差距清单 §8.2 第 7 条)。
    // 供 v3 账 model.request.prepared 的 inputMessageRefs 与实际发出的 wire
    // 消息序对账/验尸:内外消息数量不一一相等是常态(schema §8.1 横切),
    // 不能假定逐条对位。AgentLoop 把结果随 RequestPreparedContext 递给
    // 边界账。默认 nullopt = 该 backend(trace/桩/后台派生类)不提供,
    // 消费方按"映射不可得"处理,不冒充。四家真后端经 PrepareWireRequest
    // 取数(FD-02):消息容器被 extra_body 覆盖时同样如实 nullopt——出门
    // 的数组已换,旧图不可用,也不补造。
    virtual std::optional<WireMessageMap> BuildWireMessageMap(const Request& request) const {
        (void)request;
        return std::nullopt;
    }

    // extra_body 覆盖后的有效输出上限(轨迹 v3 差距清单 §8.2 第 8 条,
    // 单子 §1.18"容量判断采用本次请求实际生效的输出上限")。四家真后端
    // 经 PrepareWireRequest 从最终 body 解析(FD-02),与出门 JSON 同源。
    //   tokens —— wire 上真带的有效上限;nullopt = 请求与 extra_body 都
    //             没写(chat/responses/gemini 不带字段,交服务端默认;
    //             anthropic 必填,由自家 client 落公开兜底,不会是
    //             nullopt),或 extra_body 把上限覆盖成了非整数(null/
    //             字符串,如实 unknown,不回退覆盖前旧值)。
    //   overridden —— provider 级或请求级 extra_body 真写过输出上限键
    //             (值类型不限)。真时容量侧的输出预留直接吃 tokens(用户
    //             手笔,不受能力级封顶,与 ConfigFile 同款例外);假时
    //             tokens 即 Request::max_tokens 原值,预留走既有封顶路
    //             (主会话输出预留占坑单 §4.1 的帽不因此失效)。
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
    // 序最后的请求级键上(压过 provider 级)。写过 = 键在场、值类型不限
    //(FD-02):字符串/null 的覆盖手笔同样要被窄值压过,解析不出整数
    // 不等于覆盖不存在。自家 extra_body 没写过该键的请求是 no-op:不无
    // 中生有造键,出口形状与从前逐字节一致。默认 no-op(trace/桩后端)。
    virtual void ForceMaxOutputTokensOverride(Request& request, int tokens) const {
        (void)request;
        (void)tokens;
    }
};

}  // namespace lubancode::api
