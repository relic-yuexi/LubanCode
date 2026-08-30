// TrajectoryRecorder(P0 新轨迹记录单 §七):一场 run 一只 Recorder。
//
// 提交口照 §7.2:
//   RecordRequest { kind, scope, links, payload }
//   RecordReceipt { status, event_id, seq, event_hash, error_code }
//   TrajectorySink::Record(request, durability)
// 调用方不传 seq/event_id/hash/time,由 Recorder 一处生成。
//
// 单写者流程照 §7.3,一把 mutex 罩全程:
//   lock -> schema validate -> state-machine validate -> assign seq/event_id
//        -> offload blobs -> canonical JSON -> hash chain -> append
//        -> flush by durability -> update in-memory indexes -> unlock
//
// 状态机硬约束 18 条(§6.2)在提交时即时检查,违例回 Rejected 带稳定
// error_code(state.*),先写后说不行。I/O 失败回 IoFailed,此后句柄判
// broken,后续提交一律 IoFailed——按 §7.4 停在耐久边界,不偷跑下一步。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/blob_store.hpp"
#include "trajectory/event.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"

namespace lubancode::trajectory {

// 提交时的身份与可见面(§2.2"先分型再落盘"的调用方申报部分)。
// workspace/session/run 四项与 Recorder 底座恒等,不一致拒绝(scope.mismatch)。
struct EventScope {
    std::string workspace_key;
    std::string session_id;
    std::string run_id;
    RunKind run_kind = RunKind::MainSession;
    std::optional<std::string> turn_id;
    std::optional<std::string> request_id;
    std::optional<std::string> call_id;
    Actor actor = Actor::Host;
    Origin origin = Origin::RecoveryRuntime;
    std::vector<Visibility> visibility;
    TrainingPolicy training_policy = TrainingPolicy::Exclude;
};

// 因果与关系字段(§6.3 通用关系字段)。
struct EventLinks {
    std::optional<std::string> causation_id;    // 直接因由(event id)
    std::optional<std::string> correlation_id;  // 同一 request/call 的聚合键
    std::optional<std::string> retry_of;
    std::optional<std::string> compensates;
    std::optional<std::string> parent_call_id;
    std::optional<std::string> parent_run_id;
    std::optional<std::string> child_run_id;
    std::vector<std::string> blocked_by;
};

struct RecordRequest {
    EventKind kind = EventKind::RunStarted;
    EventScope scope;
    EventLinks links;
    nlohmann::json payload = nlohmann::json::object();
};

struct RecordReceipt {
    enum class Status { Committed, Rejected, IoFailed } status = Status::Rejected;
    std::string event_id;
    std::uint64_t seq = 0;
    std::string event_hash;
    std::string error_code;  // 稳定错误码,见 recorder.cpp 注释总表
};

// runtime 只认这只口(§7.1);装配层把真 Recorder 挂上。
class TrajectorySink {
public:
    virtual ~TrajectorySink() = default;
    virtual RecordReceipt Record(RecordRequest request, Durability durability) = 0;
};

// 时间注入(单测喂固定钟;不注入走墙钟)。
struct RecorderClock {
    virtual ~RecorderClock() = default;
    virtual std::int64_t WallMs() const;
    virtual std::int64_t MonotonicNs() const;
};

struct RecorderOptions {
    BlobStoreOptions blobs;
    std::string recorder_version = "trajectory-recorder-v1";
    // 本 stream 的 event schema major(Token 账本单 §6.1.1):1 = v1(usage
    // 挂 completed.payload.usage,legacy);2 = v2(usage 走
    // model.usage.recorded canonical owner,completed 不复制)。一场 run
    // 一只 recorder,一条 stream 只得一个版本,不混。
    int event_schema_version = kEnvelopeSchemaVersion;
};

class TrajectoryRecorder : public TrajectorySink {
public:
    // 开一场 run:create-new 占住 stream_path(recorder 是唯一 writer),
    // artifact_root 钉死为 session_artifact_root(§3.8)。clock 可空。
    static std::expected<TrajectoryRecorder, std::string> Start(
        const std::filesystem::path& stream_path, const std::filesystem::path& artifact_root,
        EventScope base_scope, RecorderOptions options = RecorderOptions{},
        const RecorderClock* clock = nullptr);

    // 续账(恢复场景):Append 打开已有 JSONL。先整本验账(hash 链/schema/
    // canonical),再逐行重放状态机账,才许续写;验不过拒开(recorder.
    // continue_not_clean)。base scope 从首行 run.started 推(workspace/
    // session/run/kind 与 actor/origin);空文件或无 run.started 拒
    // (recorder.continue_no_scope)。已封 session.ended 的账本照常可开,
    // 只是后续提交一律拒——恢复器据此读终态。
    static std::expected<TrajectoryRecorder, std::string> Continue(
        const std::filesystem::path& stream_path, const std::filesystem::path& artifact_root,
        RecorderOptions options = RecorderOptions{}, const RecorderClock* clock = nullptr);

    RecordReceipt Record(RecordRequest request, Durability durability) override;

    // ---- 封口便捷口(自动带 §8.3 终态封口四件套) ----

    // run.started;extra 并入 payload(start_reason 等放这里)。links 带
    // relations(子账的 parent_run_id/parent_call_id owner 边,§3.5)。
    RecordReceipt WriteRunStarted(nlohmann::json extra, Durability durability,
                                  EventLinks links = {});
    // run.completed/failed/cancelled;reason 可空。
    RecordReceipt FinishRun(EventKind terminal_kind, std::string reason, Durability durability);
    // session.ended(只许 main stream;须在 run terminal 之后,§5.1)。
    RecordReceipt EndSession(std::string reason, std::optional<std::string> next_session_id,
                             std::string close_quality, Durability durability);

    // 关柄并算 journal_sha256(§8.3)。此后一切提交拒绝。调它之前应已
    // 收齐 run terminal / session.ended。
    std::expected<std::string, std::string> Close();

    // ---- 观测(实现在 .cpp;Impl 是不完整类型) ----
    const std::filesystem::path& stream_path() const;
    std::uint64_t next_seq() const;
    std::string last_event_hash() const;
    const EventScope& base_scope() const;
    bool broken() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit TrajectoryRecorder(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
};

}  // namespace lubancode::trajectory
