// ChannelInteractionBroker(QQ 接入单 Q6 §12.2):渠道会话的工具审批
// 中立件——复用 runtime::InteractionFuture 的四态合同与 app-server/
// 助理 Web 的生命周期先例(登记→悬起→任意线程 resolve→悬空收口),把
// "问"从本机键盘接到远端渠道按钮上。
//
// 职责边界(§12.2 第一行:宿主裁决,平台只传按钮和身份):
//   - request_id/token 由宿主分配:pending 表绑 channel/account/conversation
//     /session/turn/tool_use_id、规范参数 hash、期限与 operator(配对
//     sender)——按钮 data 只放 opaque token 与决定码,服务端映射完整身份;
//     不用前八位 ID,不把命令或密钥塞按钮数据。
//   - 裁决(ResolveByToken)校验操作者身份:配对 sender 才有效,他人代按
//     NotAuthorized;未知/过期/跨账号 token 不唤醒 future;同 token 重复
//     回调幂等(Duplicate 只返回已处理)。
//   - 超时默认拒绝不默认放行(沿 Web 面 AssistantApprovalBroker 同款政策):
//     审批窗到期 = 悬空收口(future 返回 nullopt),拒绝文案由调用方写
//     "没人可答",不冒充用户拒绝。
//   - 重启不复活:pending 表在内存,进程重启即作废(审计留 V3 事实行,
//     由泵在 turn 收场后落账)。取消的 turn 不复活:turn 取消即 Cancel。
//
// 线程模型:Ask 在渠道 turn 的工作线程(阻塞等按钮);Resolve/Cancel 从
// 泵 tick 线程(互动回调排水)或任意线程进来。表由 mutex 保护,promise
// 在锁外 resolve(与 app_server::InteractionLedger 同款纪律)。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "runtime/interaction.hpp"

namespace lubancode::runtime {

// 一枚渠道审批请求的身份上下文(泵从 WorkItem 冻结;模型不可伪造)。
struct ChannelApprovalContext {
    std::string channel_id;
    std::string account_id;
    std::string conversation_id;
    std::string session_key;   // 渠道会话键(路由冻结)
    std::string turn_key;      // 泵的在跑键 "<ch>/<acct>/<sid>"
    std::string turn_id;       // V3 轮 id(账面锚,执行后回填)
    std::string tool_use_id;
    std::string tool_name;
    std::string operator_id;   // 配对 sender(唯一有权按按钮的人)
    std::string message_id;    // 触发来信(卡片被动回复锚)
    std::string summary;       // 脱敏摘要(卡片正文;零密钥零全文参数)
    std::string args_sha256;   // 规范参数 hash(参数变化须重新申请)
    std::int64_t deadline_ms = 0;
    std::int64_t timeout_ms = 0;  // 审批窗毫秒(等待侧按 steady_clock 计;
                                  // 0 = 不限时——测试/防御)
    std::int64_t received_at_ms = 0;
};

// 悬起未来(渠道侧;分片轮询同 app-server PendingFuture/助理 broker:审批
// 是人工节奏,50ms 醒一次查打断旗与期限,不造额外线程)。nullopt = 悬空
// 收口(超时/取消/卡片失败),等价拒绝。
class ChannelApprovalFuture final : public InteractionFuture {
public:
    std::optional<ApprovalResponse> WaitApproval() override;
    // 提问路渠道审批用不上:空实现返回悬空收口(合同允许,实现窄)。
    std::optional<QuestionResponse> WaitQuestion() override { return std::nullopt; }

    void SetTimeout(std::chrono::milliseconds timeout) { timeout_ = timeout; }
    void WatchInterrupt(const std::atomic<bool>* flag) { interrupt_flag_ = flag; }

    // broker 内部用(共享 promise 的两头)。
    std::shared_ptr<std::promise<std::optional<ApprovalResponse>>> promise;

private:
    std::chrono::milliseconds timeout_{0};
    const std::atomic<bool>* interrupt_flag_ = nullptr;
};

class ChannelInteractionBroker {
public:
    // 裁决结果(泵按此回平台回应码)。
    enum class Resolution { Applied, Duplicate, Stale, NotAuthorized };
    // 收口原因(V3 事实行的 decision)。
    enum class Outcome { Pending, Approved, Declined, Timeout, Cancelled };

    struct RequestedFact {
        std::string token;        // opaque 凭证(只进按钮 data;V3 只落 hash)
        std::string token_hash;   // SHA-256(token) hex
        ChannelApprovalContext context;
    };
    struct ResolvedFact {
        std::string token_hash;
        Outcome outcome = Outcome::Pending;
        std::string by;           // 操作者 id(Approved/Declined)或收口原因
        std::string interaction_id;  // 平台回调身份(Approved/Declined)
        std::string turn_key;
        std::int64_t resolved_at_ms = 0;
    };

    // summary 注入[now_ms 供期限记账]。
    explicit ChannelInteractionBroker(std::function<std::int64_t()> now_ms = nullptr)
        : now_ms_(std::move(now_ms)) {}

    // 发一枚审批请求:分配随机 opaque token、登记 pending、回调 on_requested
    //(泵在回调里发审批卡),返回可 Wait 的 future。同 turn_key 的旧请求
    // 不清(一次只挂一枚是泵的单飞语义,这里不越权)。
    std::shared_ptr<ChannelApprovalFuture> AskApproval(
        const ChannelApprovalContext& context,
        const std::function<void(const RequestedFact&)>& on_requested);

    // 按钮回调裁决:token 配 pending 表 + 操作者身份复核。Applied/Duplicate
    // 都算"回调已处理"(幂等:重复只返回已处理);Stale/NotAuthorized 不
    // 唤醒 future。裁决后 pending 摘表、流水进 resolved 账(泵收走落 V3)。
    Resolution ResolveByToken(const std::string& token, bool accept,
                              const std::string& operator_id,
                              const std::string& interaction_id);

    // 悬空收口(卡片发送失败/turn 取消/关停):resolve 成 cancel 语义,
    // pending 摘表,流水进 resolved 账。未知 token 静默(防御,不记账)。
    void CancelByToken(const std::string& token, const std::string& reason);

    // 等待侧超时收口(Ask 的工作线程在 future 超时后调):摘表记 Timeout
    // 事实(promise 不再 resolve——调用方已按 nullopt 收口,拒绝文案由
    // 调用方写"没人可答",不冒充用户拒绝)。未知 token 静默。
    void NoteTimeout(const std::string& token);

    // 查询:token 是否还在 pending(卡片投递前核账用)。
    bool IsPending(const std::string& token) const;

    // 按轮收走审批流水(turn 收场后落 V3 用;多 turn 并发时各取各的,
    // 不属本 turn 的留在账上)。requested 与 resolved 分开返回。
    void TakeFactsForTurn(const std::string& turn_key, std::vector<RequestedFact>* requested,
                          std::vector<ResolvedFact>* resolved);

    // 全量收口(观测/测试)。
    std::vector<ResolvedFact> DrainResolved();
    std::vector<RequestedFact> DrainRequested();

    // 观测。
    std::size_t pending_count() const;

private:
    std::int64_t NowMs() const { return now_ms_ ? now_ms_() : 0; }
    std::string NewToken();

    mutable std::mutex mutex_;
    struct Entry {
        RequestedFact fact;
        std::shared_ptr<std::promise<std::optional<ApprovalResponse>>> promise;
    };
    std::map<std::string, Entry> pending_;  // token -> entry
    // 已裁决 token 的幂等账(有界):pending 摘表后仍能识别"重复回调"与
    // "陌生 token"的区别——前者回平台"已处理",后者不唤醒任何 future。
    std::map<std::string, bool> recently_resolved_;  // token -> accept
    std::vector<RequestedFact> requested_log_;
    std::vector<ResolvedFact> resolved_log_;
    std::function<std::int64_t()> now_ms_;
    std::uint64_t token_counter_ = 0;
};

// 随机 opaque token 的按钮 data 编解码("qai:<token>:<1|0>";决定码 1=
// 允许这次,0=拒绝)。解析失败返回 false(不唤醒任何 future)。
std::string EncodeApprovalButtonData(const std::string& token, bool accept);
bool DecodeApprovalButtonData(const std::string& button_data, std::string* token, bool* accept);

// token 的 SHA-256 hex(账面锚;token 本体不入 V3)。
std::string ChannelApprovalTokenHash(const std::string& token);

}  // namespace lubancode::runtime
