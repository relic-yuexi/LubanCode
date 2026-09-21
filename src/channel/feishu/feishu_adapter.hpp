// FeishuBotAdapter:飞书进程内适配器(飞书/企微设计单 F1)。照 QqBotAdapter
// 的定案形态:实现 ChannelBridgeTransport 字节面,Bridge 隔离语义原样保留
//(握手/method/帧协议/关停次序),宿主 ChannelManager/ingress/pairing/路由
// 零改动;"sidecar"是进程内线程组(一只网关线程 + 一只发送线程)。
//
// 入站水路(§5.6/§一):网关事件 → Map+spool 落盘 → emit channel.inbound;
// handler 落盘成败即网关 ACK code(200/500)。出站水路:channel.send →
// 发送线程 POST reply(锚 message_id,msg_type=text 首版唯一)。
//
// 复用件(跨渠道中性):
//   - QqSpoolStore(R0 后仍住 qq 命名空间,按原位借用,不重复造):
//     delivery_id+事件 JSON 的先落盘再上报账(路径自拼
//     <root>/feishu/<acct>/…,与 qq 互不相见);
//   - channel/connection_state.hpp 的 ConnectionSnapshot:连接状态快照
//     (SV-07 起三平台同款中立合同;注册表的 connection_state 口同用)。
//
// 线程与锁:宿主在 ChannelManager 锁内调 WriteToSidecar/DrainFromSidecar,
// 两条口只入队/取队;IO 归内部线程;凭据只在进程内持有。Bridge 帧收发
//(宿主来向解码 + to_host 出站缓冲)归共用件 InProcessBridgeEndpoint
//(SV-08);发送队列由 send_mutex_ 保护。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "channel/bridge_endpoint.hpp"
#include "channel/channel_config.hpp"
#include "channel/connection_state.hpp"
#include "channel/credentials.hpp"
#include "channel/feishu/feishu_auth.hpp"
#include "channel/feishu/feishu_gateway.hpp"
#include "channel/feishu/feishu_http.hpp"
#include "channel/feishu/feishu_messages.hpp"
#include "channel/manager.hpp"
#include "channel/qq/qq_spool.hpp"  // 入站 spool(渠道中性账,R0 后仍住 qq 名下)

namespace lubancode::channel::feishu {

// manifest 能力声明:首版只宣称 websocket/direct/text/send/credentials
//(范围纪律:媒体/卡片/流式全不虚报)。
nlohmann::json FeishuBotCapabilities();

class FeishuBotAdapter final : public ChannelBridgeTransport {
public:
    struct Options {
        std::string channel_id = "feishu";
        std::string account_id;
        ChannelAccountUserConfig config;       // 账号配置(app_id/…)
        ResolvedChannelCredential credential;  // 进程内持有,不外泄
        std::filesystem::path state_root;      // spool 落位(宿主递进)
        FeishuHttpFunc http;                   // 生产 MakeDefaultFeishuHttpFunc;测试注假
        std::function<std::unique_ptr<IFeishuGatewayTransport>()> transport_factory;
        std::string ca_pem;                    // wss 信任锚(生产探测/配置)
        std::function<std::int64_t()> now_ms;
        std::string open_base = "https://open.feishu.cn";  // 首版仅国内域(§5.10)
        // 引导端点(生产 POST {open_base}/callback/ws/endpoint;测试注入
        // 固定 ws://127.0.0.1:…)。空 = 生产件。
        std::string bootstrap_url;
        // 装配预检确认的信任根加载失败(照 QQ §四口径:本地已知 TLS 不可用
        // 就短路引导请求,阻断无效重试)。非空 = endpoint_provider 入口直接
        // 短路。
        std::string trust_load_block_code;
        std::string trust_load_block_detail;
    };

    explicit FeishuBotAdapter(Options options);
    ~FeishuBotAdapter() override;

    FeishuBotAdapter(const FeishuBotAdapter&) = delete;
    FeishuBotAdapter& operator=(const FeishuBotAdapter&) = delete;

    // ---- ChannelBridgeTransport(宿主在 manager 锁内调;只入队/取队) ----
    void WriteToSidecar(const std::byte* data, std::size_t size) override;
    std::vector<std::byte> DrainFromSidecar() override;

    // ---- 观测(诊断/测试) ----
    std::size_t spool_pending_count() const;
    bool gateway_thread_running() const { return gateway_thread_ != nullptr; }
    std::string gateway_state() const {
        return session_ ? session_->state_name() : std::string("idle");
    }
    // 平台连接状态快照(channel 中立合同,SV-07 起三平台同款;stage 用
    // 飞书的稳定名:bootstrapping/connecting/connected/stopped)。
    channel::ConnectionSnapshot ConnectionState() const;
    // 收到但未建模的事件的明确终结记录(card/未知事件类型/无效载荷)。
    std::uint64_t unsupported_event_count() const {
        return unsupported_event_count_.load();
    }
    // 分片缺片到期静默弃的组数(§5.5)。
    std::uint64_t dropped_fragment_groups() const {
        return session_ ? session_->dropped_fragment_groups() : 0;
    }
    // 故障注入(仅测试):spool 落盘恒败——ACK 500/平台重推链路的复现口。
    void SetSpoolAppendFaultForTest(bool fail);

private:
    // Bridge 帧分派(宿主锁内上下文)。
    void HandleHostFrame(const nlohmann::json& frame);
    void ReplyResult(std::int64_t id, const nlohmann::json& result);
    void ReplyDomainError(std::int64_t id, DomainErrorName name, const std::string& detail);
    void EmitNotification(BridgeMethod method, const nlohmann::json& params);
    // 网关事件落地:MessageReceive = Map + spool 先落 + emit,返回 Ok/Retry
    //(网关据此回平台 ACK 200/500);连接事件记 ConnectionState 的账。
    FeishuGatewayAck HandleGatewayEvent(const FeishuGatewayEvent& event);
    bool StartGatewayLocked();
    void StopGatewayLocked(const std::string& reason);
    void SenderLoop();
    std::string NextDeliveryId();

    Options options_;
    // 停止旗声明在前:HTTP seam 在构造时就要拿 &stop_ 盖章(A08 口径)。
    std::atomic<bool> stop_{false};
    FeishuTokenManager token_manager_;
    // 发送队列项:宿主 channel.send 的 request_id + 冻结载荷。
    struct PendingSend {
        std::int64_t request_id = 0;
        FeishuReplyRequest request;
        int attempts = 0;
    };
    std::vector<PendingSend> send_queue_;  // 由 send_mutex_ 保护(sender_wake_ 同锁)
    std::optional<FeishuMessageSender> sender_;
    std::optional<qq::QqSpoolStore> spool_;

    std::unique_ptr<FeishuGatewaySession> session_;
    std::unique_ptr<std::thread> gateway_thread_;
    std::unique_ptr<std::thread> sender_thread_;
    std::condition_variable sender_wake_;
    std::atomic<std::uint64_t> unsupported_event_count_{0};

    // ConnectionState 的账(网关线程写/宿主线程读,独立小锁)。
    mutable std::mutex connection_mutex_;
    channel::ConnectionSnapshot connection_;

    // Bridge 帧收发机械(SV-08 共用件):解码循环/出站缓冲/输出锁全在
    // bridge_;本类只剩 HandleHostFrame 业务分派与平台账。
    InProcessBridgeEndpoint bridge_;
    std::mutex send_mutex_;  // send_queue_ 的账

    std::mt19937 delivery_rng_{std::random_device{}()};
    std::atomic<std::uint64_t> delivery_counter_{0};
};

}  // namespace lubancode::channel::feishu
