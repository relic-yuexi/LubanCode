// BridgeRouter:Channel Bridge v1 的双向 request/response/notification
// router 骨架(多渠道消息接入单阶段 1)。纯结构,不接真网络、不 spawn 进程
// ——字节收发是阶段 2 起 ChannelManager 的事,这里只管:
//
//   - id 记账:request id 在各自方向独立递增,不回收,不跨方向串
//     (bridge-protocol.md §2"request id 在各自方向独立递增;响应只配本
//     方向 pending")。
//   - 出站队列:本端待发给对端的 request/notification,已编好 JSON-RPC
//     形状,等调用方真正编码发送字节。
//   - 进站队列:对端发来的 request/notification(response 不进这条队——
//     它们要现场配对 pending,语义是"解出一份等待",不是"排队消费")。
//   - 陌生 method、迟到/重复/陌生 id 的响应:不崩宿主,记诊断账
//     (bridge-protocol.md §2)。
//
// 一枚 BridgeRouter 代表连接的一端(host 或 sidecar 皆可,协议本身对称:
// "两边都能发 request,也都能回 response"——bridge-protocol.md §2)。
// 两端各自持一枚 router,id 空间天然不串:每枚 router 只认自己发出去的
// pending,对端 id 从 1 起也不会撞车。
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/bridge_protocol.hpp"

namespace lubancode::channel {

struct PendingBridgeRequest {
    std::int64_t id = 0;
    BridgeMethod method = BridgeMethod::Initialize;
};

class BridgeRouter {
public:
    // 发起一条请求给对端:分配下一个本端 id、记入 pending、推进出站队列。
    // 返回分配到的 id(调用方留着,将来从 DispatchResult::matched_request_id
    // 核对是不是这一笔的回复)。
    std::int64_t EnqueueOutgoingRequest(BridgeMethod method, const nlohmann::json& params);
    // notification 不留 pending,直接进出站队列。
    void EnqueueOutgoingNotification(BridgeMethod method, const nlohmann::json& params);

    bool HasOutbound() const { return !outbound_.empty(); }
    // 取出下一条待发消息(FIFO)。队列空时抛 std::out_of_range——调用方须
    // 先 HasOutbound() 判过。
    nlohmann::json PopOutbound();

    struct DispatchResult {
        enum class Action {
            RequestQueued,       // 对端发来的 request,已入进站队列,等调用方处理并回 response
            NotificationQueued,  // 对端发来的 notification,已入进站队列
            ResponseMatched,     // 对端的 response 配对到本端某笔 pending request
            UnknownMethod,       // 对端发来陌生 method 的 request/notification(未入队,已留诊断)
            StaleResponse,       // response id 对不上任何 pending(迟到/重复/陌生),已丢弃留诊断
            Malformed,           // 帧结构本身不是合法 JSON-RPC 2.0 形状(直接透传 IncomingMessage 的判定)
        };
        Action action = Action::Malformed;
        std::string diagnostic;
        // ResponseMatched 时:被配对消费掉的那笔 pending request 的 id/method。
        std::optional<std::int64_t> matched_request_id;
        std::optional<BridgeMethod> matched_request_method;
    };

    // 喂入一条已解出的 IncomingMessage(对端发来的)。
    DispatchResult Dispatch(const IncomingMessage& message);

    bool HasInbound() const { return !inbound_.empty(); }
    // 取出下一条对端发来的 request/notification(FIFO)。队列空时抛
    // std::out_of_range。
    IncomingMessage PopInbound();

    // 还有多少笔本端发出、尚未收到回复的 request(诊断/关闭前排空用)。
    std::size_t PendingRequestCount() const { return pending_.size(); }

    const std::vector<std::string>& Diagnostics() const { return diagnostics_; }

private:
    std::int64_t next_id_ = 1;
    std::map<std::int64_t, PendingBridgeRequest> pending_;
    std::deque<nlohmann::json> outbound_;
    std::deque<IncomingMessage> inbound_;
    std::vector<std::string> diagnostics_;
};

}  // namespace lubancode::channel
