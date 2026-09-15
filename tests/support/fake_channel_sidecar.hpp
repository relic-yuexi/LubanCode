// FakeChannelSidecar:纯内存管道模拟一只 Channel Plugin sidecar 的握手+
// 收发(多渠道消息接入单阶段 1 交付项"fake-channel-sidecar")。不起真
// 进程、不碰真 stdin/stdout——喂它 host 发来的帧字节,吐它要回给 host 的
// 帧字节,内部状态机照 docs/architecture/channels/bridge-protocol.md
// §3-5 的样例形状应答。
//
// 设计给阶段 1 的冒烟测试用,也给未来真渠道适配器的 conformance suite
// (README.md §9 阶段 1 "供后续真渠道适配器复用同一套测试路数")复用同一
// 套喂字节/取字节的路数——真 sidecar 接进程 stdio 后,测试脚本换个字节
// 来源,断言逻辑不用大改。
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/frame.hpp"
#include "channel/types.hpp"

namespace lubancode::test_support {

class FakeChannelSidecar {
public:
    // 喂入 host 发来的字节(增量;一帧可拆多次喂,也可几帧粘一次喂——内部
    // FrameDecoder 管拆包/粘包)。每次喂完自动处理已凑齐的完整帧。
    void FeedFromHost(const std::byte* data, std::size_t size);
    void FeedFromHost(std::string_view bytes);

    // 取出待发给 host 的全部字节并清空内部缓冲(FIFO 顺序已编码好帧)。
    std::vector<std::byte> DrainToHost();
    bool HasBytesForHost() const { return !to_host_bytes_.empty(); }

    // 主动排一条 channel.inbound 通知,编码进待发缓冲(不需要等 host 先
    // 发什么——sidecar 本就是"主动上报入站"的一方,见 bridge-protocol.md
    // §1)。
    void EmitInboundEvent(const lubancode::channel::ChannelInboundEvent& event);

    // ---- Q6 互动回调(QQ 按钮审批)-----------------------------------------
    // 主动排一条 channel.interaction.create 通知(按钮点击;params 形状见
    // bridge 协议 InteractionCreate 表项)。
    void EmitInteractionNotification(const nlohmann::json& params);
    // 宿主回的 channel.interaction.ack 观测账(裁决结果对账用)。
    struct RecordedInteractionAck {
        std::string interaction_id;
        int code = 0;
    };
    const std::vector<RecordedInteractionAck>& interaction_acks() const {
        return interaction_acks_;
    }

    // ---- 观测账(冒烟测试断言用) ----
    bool handshake_completed() const { return handshake_completed_; }
    bool started() const { return started_; }
    bool stopped() const { return stopped_; }
    const std::string& negotiated_channel_id() const { return channel_id_; }
    const std::string& negotiated_account_id() const { return account_id_; }

    struct RecordedSend {
        std::string client_id;
        nlohmann::json params;
        std::string provider_message_id;
    };
    const std::vector<RecordedSend>& sent_messages() const { return sent_messages_; }
    const std::vector<std::string>& acked_delivery_ids() const { return acked_delivery_ids_; }
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

    // ---- 发送脚本(QQ 接入单 Q2 测试:manager/outbox 投递族用) ----------
    // AutoAccept 缺省 = 原行为(立刻回 accepted)。其余:
    //   RateLimitedFirst  同 client_id 前 rate_limited_first_n 次回
    //                     rate_limited domain 错,之后成功(退避重试同载荷);
    //   PermanentReject   回 permanent_reject(msg_id expired 时 detail 带
    //                     "expired"——分型 reply_window_expired);
    //   LoginRequired     回 login_required;
    //   Silent            不应答(超时 → delivery_unknown 测试)。
    enum class SendScript {
        AutoAccept,
        RateLimitedFirst,
        PermanentReject,
        LoginRequired,
        Silent,
    };
    void set_send_script(SendScript script) { send_script_ = script; }
    void set_rate_limited_first(int sends) { rate_limited_first_ = sends; }
    void set_reject_detail(std::string detail) { reject_detail_ = std::move(detail); }
    // 该 client_id 收到过几次 channel.send(同载荷重试计数)。
    int send_count_for(const std::string& client_id) const;
    // 主动排一条 delivery.receipt 通知(§七回执族的测试口)。
    void EmitDeliveryReceipt(const std::string& outbound_delivery_id,
                             const std::string& outcome, const std::string& reason = "");

    // 下一次握手时故意回一个不认得的 protocol_version(测试
    // protocol_incompatible 明败路径用)。
    void ForceProtocolMismatchOnNextHandshake() { force_protocol_mismatch_ = true; }

private:
    void HandleIncomingJson(const nlohmann::json& frame_json);
    void SendBack(const nlohmann::json& message_json);

    lubancode::channel::FrameDecoder decoder_;
    std::vector<std::byte> to_host_bytes_;

    bool handshake_completed_ = false;
    bool started_ = false;
    bool stopped_ = false;
    bool force_protocol_mismatch_ = false;
    std::string channel_id_;
    std::string account_id_;
    int next_provider_message_seq_ = 1;

    SendScript send_script_ = SendScript::AutoAccept;
    int rate_limited_first_ = 1;
    std::string reject_detail_ = "content rejected";
    std::map<std::string, int> send_counts_;

    std::vector<RecordedSend> sent_messages_;
    std::vector<std::string> acked_delivery_ids_;
    std::vector<RecordedInteractionAck> interaction_acks_;
    std::vector<std::string> diagnostics_;
};

}  // namespace lubancode::test_support
