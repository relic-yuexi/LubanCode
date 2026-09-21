// 轨迹旁路模型请求桥(AR-12 机械拆分:自 trajectory_session.hpp 按桥类
// 边界拆出,类体与合同一字未动)。Token 账本单 A1:compact 的 map/reduce、
// 记忆抽取、会话起名、doctor 探针这类"回合外的宿主小请求"的模型边界 ->
// trajectory 事件。接口与主桥同一只(agent::LoopBoundaryRecorder),
// SampleModel/探针只认接口。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "agent/loop.hpp"
#include "api/types.hpp"
#include "runtime/trajectory_turn_bridge.hpp"
#include "telemetry/wake.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 旁路模型请求桥(Token 账本单 A1):compact 的 map/reduce、记忆抽取、
// 会话起名、doctor 探针这类"回合外的宿主小请求"的模型边界 -> trajectory
// 事件。§11.2"旁路请求也走公共 ModelRequestRecorder"落在它身上:接口
// 与主桥同一只(agent::LoopBoundaryRecorder),SampleModel/探针只认接口。
//
// 与主桥的差异:没有工具栅栏簿记,每次 OnRequestPrepared 自开一只
// scheduled_host 小 turn 并把请求的首条 user 消息记作 input(recorder
// 状态机约束:sent/output 须落 turn 内,首 sent 前须有 input),output
// 三态收口时把 turn 一并收掉。一只桥一次采样用完即弃,不跨线程共享
// ——每处调用自己造(recorder 提交全程持锁,多桥并发在盘上仍串行)。
// ---------------------------------------------------------------------------
class TrajectoryBypassBridge : public agent::LoopBoundaryRecorder {
public:
    TrajectoryBypassBridge(trajectory::TrajectoryRecorder& recorder, trajectory::EventScope base_scope,
                           TrajectoryTurnBridge::Identity identity);
    // v3 写模式(取消误报 ESC 单 Bug 2):v2 recorder 不在,主账是 V3Writer,
    // 走 v3 的 typed 事件/消息合同,不往 v3 文件硬塞 v2 行。books 是账本的
    // 会话共享账(借读 active_main_turn_id,旁路行 parentTurnId 挂主回合);
    // purpose 定本桥服务的请求用途(消息 purpose 映射按它)。identity_scope
    // 只借 workspace/session/run 三枚身份(wake 投递用),不进事件信封——
    // v3 行的身份在信封自己的 sessionId/runId。
    TrajectoryBypassBridge(trajectory::v3::V3Writer* v3_writer, V3SessionBooks* v3_books,
                           trajectory::EventScope identity_scope, TrajectoryTurnBridge::Identity identity,
                           accounting::RequestPurpose purpose);
    ~TrajectoryBypassBridge() override;

    TrajectoryBypassBridge(const TrajectoryBypassBridge&) = delete;
    TrajectoryBypassBridge& operator=(const TrajectoryBypassBridge&) = delete;

    // ---- agent::LoopBoundaryRecorder(采样/探针在模型边界调) ----
    std::string OnRequestPrepared(const api::Request& request,
                                  const agent::RequestPreparedContext& ctx) override;
    // false = sent 落不住(失败与恢复单 P1-C):采样停在发送边界。
    bool OnRequestSent(const std::string& request_id) override;
    void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id,
                         int cache_epoch = 0, bool prefix_append_only = true,
                         bool cache_read_reported_by_provider = false,
                         bool cache_creation_reported_by_provider = false,
                         const std::string& usage_anomaly = std::string()) override;
    bool OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                           const std::string& stop_reason,
                           const std::string& provider_response_id) override;
    void OnOutputFailed(const std::string& request_id, const std::string& reason) override;
    void OnOutputCancelled(const std::string& request_id, agent::OutputCancelSource source) override;

    // 诊断:最近一枚提交失败 receipts 的稳定码。
    std::vector<std::string> recent_errors() const { return recent_errors_; }
    // v3 模式的落账错误共享汇(账本持有,/doctor trajectory 从这读);
    // v2 路不碰,行为与从前一致。
    void SetErrorSink(std::vector<std::string>* sink) { error_sink_ = sink; }
    // T1 committed wake(与主桥同款;默认空 = 零行为)。
    void SetCommitWake(telemetry::CommitObserver* wake, std::string stream_id) {
        commit_wake_ = wake;
        wake_stream_id_ = std::move(stream_id);
    }

private:
    trajectory::RecordReceipt Put(trajectory::EventKind kind, std::optional<std::string> request_id,
                                  trajectory::Actor actor, trajectory::Origin origin, nlohmann::json payload,
                                  trajectory::Durability durability = trajectory::Durability::ProcessCrash);
    void NoteError(const trajectory::RecordReceipt& receipt, const char* where);
    void OpenTurn();
    void CloseTurn(bool ok, bool cancelled, const std::string& reason);
    std::string NextRequestId();
    std::string NextTurnId();
    std::string NextInputId();
    std::string NextOutputId();

    // ---- v3 写模式(V3Mode() 为假时一只方法都不进) ----
    // v3 旁路请求簿:一桥一采样;usage 暂存到 assistant 成行时一并写
    //(§4.12 usage 唯一 owner 是 assistant message);流 started 懒起
    //(与主桥同款:零 delta 的旁路采样也保 started+completed 闭环形状)。
    struct V3RequestBook {
        std::string step_id;
        std::string model;
        std::string turn_id;  // 内部回合号(memory-turn-*)
        std::optional<nlohmann::json> usage;
        bool usage_reported = false;
        std::string provider_response_id;
        bool output_committed = false;
        std::string stream_id;
        std::string reserved_message_id;
        bool stream_started = false;
    };
    std::string V3RequestPrepared(const api::Request& request, const agent::RequestPreparedContext& ctx);
    // T11-A:旁路用途 → 消息 purpose(memory_extract/title_refine 分铺)。
    trajectory::v3::MessagePurpose V3MessagePurpose() const;
    bool V3RequestSent(const std::string& request_id);
    void V3UsageRecorded(const std::string& request_id, const api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id);
    // 失败/取消终态后放行暂存的 usage(model.usage.appended 单独立账,§4.12
    // 的补报路):半截失败的用量不许跟着 assistant 一起沉没——旁路请求没有
    // assistant 行可挂,appended 就是唯一落点。
    void V3FlushUsageAppended(const V3RequestBook& book, const std::string& request_id);
    bool V3EnsureStreamStarted(const std::string& request_id);
    bool V3OutputCompleted(const std::string& request_id, const api::Message& assistant,
                           const std::string& stop_reason, const std::string& provider_response_id);
    void V3OutputFailed(const std::string& request_id, const std::string& reason);
    void V3OutputCancelled(const std::string& request_id, agent::OutputCancelSource source);
    void NoteV3Error(const trajectory::v3::WriteReceipt& receipt, const char* where);
    void V3NotifyCommitted(const trajectory::v3::WriteReceipt& receipt);
    bool V3Mode() const { return v3_writer_ != nullptr; }

    trajectory::TrajectoryRecorder* recorder_ = nullptr;  // v2 主账(引用改指针:类要装得下 v3 模式)
    trajectory::v3::V3Writer* v3_writer_ = nullptr;        // v3 主账
    V3SessionBooks* v3_books_ = nullptr;                   // v3 会话共享账(借读,不持有)
    accounting::RequestPurpose purpose_ = accounting::RequestPurpose::OtherHostRequest;
    trajectory::EventScope base_scope_;
    TrajectoryTurnBridge::Identity identity_;
    std::string turn_id_;
    bool turn_open_ = false;
    bool dead_ = false;  // 开不了小 turn(主 turn 在开着)后哑火,不再连发
    std::map<std::string, std::string> request_prepared_;  // request_id -> prepared event id
    std::map<std::string, V3RequestBook> v3_requests_;     // v3 模式的请求簿
    std::string last_input_event_id_;
    std::uint64_t request_counter_ = 0;
    std::uint64_t turn_counter_ = 0;
    std::uint64_t input_counter_ = 0;
    std::uint64_t output_counter_ = 0;
    std::vector<std::string> recent_errors_;
    std::vector<std::string>* error_sink_ = nullptr;    // v3 落账错误共享汇(默认空)
    telemetry::CommitObserver* commit_wake_ = nullptr;  // T1 committed wake(默认空)
    std::string wake_stream_id_;
};

}  // namespace lubancode::runtime
