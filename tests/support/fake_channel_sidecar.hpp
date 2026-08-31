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

    std::vector<RecordedSend> sent_messages_;
    std::vector<std::string> acked_delivery_ids_;
    std::vector<std::string> diagnostics_;
};

}  // namespace lubancode::test_support
