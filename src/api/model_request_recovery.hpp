// 请求级恢复(《子代理监督器、agent_watch 与停滞恢复设计》P0-1):主路与
// 子路共用的 provider 重试链。此前主、子请求错一次就明败——上游 429 一阵
// 风,七只子代理全断,靠人手写 handoff 救(2026-09-01 的教训)。这里把
// "哪类错可以安全重发、隔多久重发、几轮放弃"抽成一份,两路一起吃。
//
// 安全合同(单子 §8.1/§8.3):
//   * 一轮模型输出先进临时 assembler,合法终止才原子提交进 history;
//     断在半途的正文/半截 tool JSON 永不落地——send_stream 不归,就没提交。
//   * 因此"流中途失败"永远发生在提交边界之前,从同一 history 提交边界
//     重发是幂等的;已提交的 ToolResult 绝不重跑,恢复只重发模型请求。
//   * 用户取消(Cancelled)压过一切重试;退避等待中也能被打断。
//   * 总墙钟不因重试放宽:重试消耗的时间落在墙钟之内(墙钟到点照收)。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::api {

// 一次逻辑请求的一次尝试(单子 §8.1)。committed_assistant_message 由
// 调用方在消息真正入 history 后补记(P0-0 的台账账),本结构自身只在
// 尝试期内活动。
struct ModelRequestAttempt {
    std::string logical_request_id;
    int attempt = 1;
    std::string history_commit_hash;  // 重发边界:同一逻辑请求的各尝试相同
    bool saw_headers = false;         // Backend 层不见头,首枚流事件即旁证
    bool saw_stream_event = false;
    bool committed_assistant_message = false;
    std::string error_code;  // 稳定码(ReasonCodeOfError),成功时为空
};

// 尝试相位(恢复账的事件流)。
enum class RequestAttemptPhase {
    Started,    // 第 N 次尝试发出
    Retrying,   // 本次失败且判定可安全重发,退避后重来(显示层据此丢半截流)
    Exhausted,  // 重试用尽或不可重试,按错误收口
    Succeeded,  // 本次尝试流式收口成功
};

// 稳定错误码:监督账/agent_watch/遥测共用,不带错误正文(单子 §五·11)。
// 形如 "network.connect_failed" / "http.429" / "cancelled"。
std::string ReasonCodeOfError(const Error& error);

// 重发边界的凭据:对请求的消息序列(条数 + 角色 + 内容尺寸)取短哈希。
// 不哈希正文全文——边界身份要的是"同一份 history 提交",尺寸序列足够指认,
// 长会话也不为记账多烧一遍全量散列。
std::string HistoryCommitHashOf(const Request& request);

// 可自动重试白名单(单子 §8.2):DNS/connect reset/TLS 瞬断(Network 类)、
// 408/429/502/503/504、首字节前超时。Cancelled/Parse/Api 与其余 HTTP 码
// 不重试。provider 的 Retry-After 目前不经过 Backend 接口,拿不到——尊重
// 它这件事记在案,后端若将来透出,再在阶梯上叠加(不越过总墙钟)。
bool IsRetryableError(const Error& error);

// 重试阶梯(单子 §8.2):第 1 次失败等 250~750ms,第 2 次等 1~2s,第 3 次
// 失败即收口。attempt = 刚失败的那次尝试号(1 起)。
std::chrono::milliseconds BackoffMsForAttempt(int attempt);

// 一条逻辑请求最多几次尝试(含首发)。
constexpr int kMaxRequestAttempts = 3;

struct RequestRecoveryHooks {
    // 恢复账出水口(空 = 没人记账,恢复照跑)。在发送线程上同步调。
    std::function<void(const ModelRequestAttempt&, RequestAttemptPhase)> on_attempt;
};

// 单次尝试的执行体:调用方(AgentLoop::Run)自备 assembler/gates 等局部,
// 每次进入先重置(半截流就这么丢掉,不拼两段正文);顺手把首枚流事件等
// 旁证写进 attempt 账(恢复环每次尝试前会重置这些位)。
using AttemptSender = std::function<std::expected<void, Error>(ModelRequestAttempt&)>;

// 恢复环本体:策略(分类/阶梯/相位账)在这,机械(发流/攒消息)在调用方。
// 主路与子路共用——两路都经 AgentLoop::Run,那里把 assembler 重置与显示
// 回滚挂进 on_attempt(Retrying)。cancel:贯穿发送与退避等待的用户取消链,
// 置位即停,不再重试。
std::expected<void, Error> RunRequestWithRecovery(const AttemptSender& send_once,
                                                  const RequestRecoveryHooks& hooks,
                                                  const std::atomic<bool>* cancel = nullptr);

// 直发便捷路(不经 assembler 的调用方与单测):Backend 的一次 send_stream
// 当一次尝试,on_event 原样转发。
std::expected<void, Error> SendStreamWithRecovery(Backend& backend, const Request& request,
                                                  const std::function<void(const StreamEvent&)>& on_event,
                                                  const RequestRecoveryHooks& hooks,
                                                  const std::atomic<bool>* cancel = nullptr);

}  // namespace lubancode::api
