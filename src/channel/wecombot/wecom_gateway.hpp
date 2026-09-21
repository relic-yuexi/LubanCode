// 企业微信智能机器人长连接网关状态机(W1,设计单 §六):拨号 →
// aibot_subscribe(等 errcode=0)→ 心跳 30s → 读循环 → 退避重连
// 1s..60s。凭据错即止;disconnected_event(新连踢旧)即止不抢线。
//
// 写侧串行化(§六 6.5"回话帧走同一条 WS 连接"):一切帧——subscribe/
// ping/出站 aibot_respond_msg——只由网关线程写。WsClient 合同是"一只连接
// 归一只线程"(transport/ws_client.hpp 头注),发送线程不直接碰 socket:
// 它经 SubmitFrame 入队、按 headers.req_id 等平台回执;读循环每
// drain_poll_ms 一拍排水,出站帧的延迟上界即此值。
//
// 与 QqGatewaySession 的两处刻意差异(有据,不是漏抄):
//   - 企微无平台序号/Resume 补发,durable 游标退化成适配器 spool +
//     宿主 ingress 的 msgid 去重;落盘失败不断线(QQ 断线后靠 Resume
//     补发,企微没有这条路——断了也拿不回,照常上报 + Fatal 留痕)。
//   - 凭据错即止:订阅回执落凭据族错误码时RunLoop 终止(明报,不空转)。
//
// 线程模型:RunLoop 归一只网关线程;CancelInFlight 供停止方外部打断
// 在途连接(共享所有权,照 QqGatewaySession)。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/transport/gateway_transport.hpp"  // 传输 seam(SV-07 起中立位,平台无关)
#include "channel/wecombot/wecom_proto.hpp"

namespace lubancode::channel::wecombot {

// 传输 seam(SV-07 自 qq_gateway.hpp 迁 channel/transport,平台无关,
// 工厂底下同用共享层 WsClient)。WecomTransport 为迁移期别名,新代码可
// 直接用 channel::transport::IGatewayTransport。
using WecomTransport = channel::transport::IGatewayTransport;
using WecomTransportFactory = std::function<std::unique_ptr<WecomTransport>()>;

// 连接阶段稳定名(连接状态口径):connecting(TCP/TLS/WS 升级)→
// subscribing(订阅等回执)→ connected → stopped。
inline constexpr char kStageConnecting[] = "connecting";
inline constexpr char kStageSubscribing[] = "subscribing";
inline constexpr char kStageConnected[] = "connected";
inline constexpr char kStageStopped[] = "stopped";

// 出口事件(适配器消费)。
struct WecomGatewayEvent {
    enum class Kind {
        MessageCallback,  // 用户消息(body 原文 + req_id 回话锚)
        EventCallback,    // 事件回调(body 原文;enter_chat/卡片/反馈只记账)
        UnsupportedFrame, // 认不得的帧(detail = cmd/形状说明;计数留痕)
        SessionReady,     // 订阅过(连接可用)
        StageChanged,     // 连接阶段推进(stage 带稳定名)
        ConnectFailed,    // 一轮尝试没订阅成就断(stage/error_code/detail = 根因)
        Disconnected,     // 订阅成过后断线(stage/error_code/detail = 根因)
        BackoffScheduled, // 失败后的退避排程(attempt/next_retry_at_ms;不带根因)
        Stopped,          // RunLoop 收口(停止/致命)
    };
    Kind kind = Kind::Disconnected;
    nlohmann::json body;   // MessageCallback/EventCallback 的 body 原文
    std::string req_id;    // MessageCallback 的回话锚(透传给 respond)
    std::string detail;
    std::string stage;     // StageChanged/ConnectFailed/Disconnected 时有值
    std::string error_code;
    int attempt = 0;       // 尝试编号(1 起):ConnectFailed/Disconnected 与
                           // 随后的 BackoffScheduled 同轮同号
    std::int64_t next_retry_at_ms = 0;
};

// 发送线程递交一帧的结果(SubmitFrame 出参)。
struct WecomSubmitOutcome {
    enum class Status {
        Acked,        // 平台回了回执(errcode/errmsg 在场)
        Timeout,      // 写出去了但等不到回执(重试由调用方裁决)
        SendFailed,   // 写失败(连接已按断线处置)
        NotConnected, // 无可用连接(退避窗内;递交留在队列,下轮连接排水)
        Stopped,      // RunLoop 已收口
    };
    Status status = Status::NotConnected;
    std::int64_t errcode = 0;
    std::string errmsg;
    std::string detail;
};

// 递交回执(内部件:发送线程等、网关线程填;SubmitFrame 见类注)。
// done 用原子:排水侧免锁判"这条已超时/已失败,不必再写"。
struct WecomPendingReceipt {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> done{false};
    WecomSubmitOutcome outcome;  // mutex 保护(done 置位后只读)

    bool IsDone() const { return done.load(); }
    // 完成一次(超时/已完成的重复完成是空操作)。
    void Complete(WecomSubmitOutcome result) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (done.load()) {
                return;
            }
            outcome = std::move(result);
            done.store(true);
        }
        cv.notify_all();
    }
};

class WecomGatewaySession {
public:
    struct Options {
        WecomTransportFactory transport_factory;
        std::string endpoint = std::string(kWecomDefaultEndpoint);
        std::string bot_id;  // 管理后台 BotID(账号配置 app_id 字段)
        std::string secret;  // 长连接专用 Secret(进程内持有,不落日志)
        std::function<void(const WecomGatewayEvent&)> on_event;
        std::function<std::int64_t()> now_ms;
        std::int64_t ping_interval_ms = 30'000;  // 官方口径 30s
        int subscribe_timeout_ms = 10'000;       // 订阅回执等待窗
        int missed_ack_limit = 2;                // 连续 N 拍无任何入站帧判死线
        int max_backoff_ms = 60'000;
        double backoff_scale = 1.0;  // 测试设 0.001 把秒级阶梯压成毫秒
        int drain_poll_ms = 200;     // 读循环粒度 = 出站帧延迟上界
        // 装配预检确认的 TLS 信任根加载失败(稳定码 + 脱敏 detail):非空 =
        // 每轮连接入口直接短路,不碰网(同 QQ §四;重启/配置变化再装配)。
        std::string trust_load_block_code;
        std::string trust_load_block_detail;
    };

    explicit WecomGatewaySession(Options options) : options_(std::move(options)) {}
    ~WecomGatewaySession();

    WecomGatewaySession(const WecomGatewaySession&) = delete;
    WecomGatewaySession& operator=(const WecomGatewaySession&) = delete;

    // 网关线程入口。返回 = stop 置位、致命错误(凭据错/被新连顶替)或致命
    // 配置(已报 ConnectFailed;随后必有 Stopped)。
    void RunLoop(std::atomic<bool>* stop);

    // 停止方从外部打断在途连接/读(共享所有权,照 QqGatewaySession)。
    void CancelInFlight();

    // 发送线程递交口:入队一帧(已序列化 JSON 文本),按 req_id 等平台回执
    // 或超时。RunLoop 收口后恒回 Stopped。同 req_id 重复递交(重试)合法
    // ——官方分段回复共用 req_id;新递交覆盖同 id 旧账。
    WecomSubmitOutcome SubmitFrame(const std::string& frame_text, const std::string& req_id,
                                   int timeout_ms);

    // ---- 观测(诊断/测试) ----
    std::string state_name() const;
    int connect_attempts() const { return connect_attempts_.load(); }
    int unexpected_frame_count() const { return unexpected_frames_.load(); }
    // 当前是否在订阅后的在线窗内(发送线程的 NotConnected 快速路判据)。
    bool connection_active() const { return connection_active_.load(); }

private:
    enum class State { Idle, Connecting, Subscribing, Running, Backoff, Stopped };
    struct RunOutcome {
        bool stable = false;  // 这轮在线过(收到过任何回执/回调)
        bool fatal = false;   // 凭据错/被顶替:RunLoop 终止,不退避
    };
    struct WriteEntry {
        std::string frame;
        std::shared_ptr<WecomPendingReceipt> receipt;
    };

    RunOutcome RunOneConnection(std::atomic<bool>* stop, int attempt_number);
    void SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms);
    void EmitEvent(const WecomGatewayEvent& event);
    std::string NextReqId();
    // 出站排水(网关线程独占写):写失败 = 连接死,回错误说明。
    std::optional<std::string> DrainWrites(const std::shared_ptr<WecomTransport>& transport);
    // 回执匹配:Ack 帧对上 pending 即完成。返回是否匹配到。
    bool CompletePending(const std::string& req_id, std::int64_t errcode,
                         const std::string& errmsg);
    void FailAllPendings(WecomSubmitOutcome::Status status, const std::string& detail);

    Options options_;
    std::atomic<State> state_{State::Idle};
    std::atomic<int> connect_attempts_{0};
    std::atomic<int> unexpected_frames_{0};
    std::atomic<bool> connection_active_{false};
    std::atomic<bool> loop_stopped_{false};
    std::atomic<std::uint64_t> req_id_sequence_{0};

    // 递交账:pending 回执表(req_id → receipt)与出站队列。发送线程写
    // 队列、网关线程排水;pending 表两边都碰(read loop 完成账、SubmitFrame
    // 登记/超时清理),各归各锁。
    std::mutex pending_mutex_;
    std::map<std::string, std::shared_ptr<WecomPendingReceipt>> pendings_;
    std::mutex write_mutex_;
    std::deque<WriteEntry> write_queue_;

    // 在途传输共享所有权(取消方拷 shared_ptr 再调 Cancel)。
    std::mutex in_flight_mutex_;
    std::shared_ptr<WecomTransport> in_flight_;
};

}  // namespace lubancode::channel::wecombot
